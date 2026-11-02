// rap::sys::http_request: WinHTTP on Windows; elsewhere libcurl, loaded on first
// use with the few functions it needs declared here (their interface has been
// stable for decades), so the core neither builds nor installs against it.
// Redirects are followed here, not by either library, so both drop the
// credentials the same way when one leads elsewhere.
#include "rapport/sys.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace rap::sys
{
namespace
{

constexpr int max_redirects = 10;

bool iequals(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b, [](char x, char y) { return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
}

const std::string* header(const http_headers& h, std::string_view name)
{
    for (const auto& [n, v] : h)
        if (iequals(n, name)) return &v;
    return nullptr;
}

// "Name: value" lines into headers; a status line starts a new response
// (each redirect's headers are dropped for the next one's).
void add_header_line(std::string_view line, http_headers& out)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    if (line.starts_with("HTTP/"))
    {
        out.clear();
        return;
    }
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return;
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    out.emplace_back(std::string(line.substr(0, colon)), std::string(value));
}

// -- redirects ------------------------------------------------------------

// "scheme://host:port" of an absolute URL, lower case, the port spelt out:
// what credentials are tied to.
std::string origin(std::string_view url)
{
    const size_t colon = url.find("://");
    if (colon == std::string_view::npos) return {};
    std::string scheme(url.substr(0, colon));
    std::string_view authority = url.substr(colon + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (const size_t at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);
    std::string host(authority), port;
    const size_t close = host.find(']');  // [IPv6]
    if (const size_t c = host.rfind(':'); c != std::string::npos && (close == std::string::npos || c > close))
        port = host.substr(c + 1), host.resize(c);
    std::ranges::transform(scheme, scheme.begin(), [](char ch) { return char(std::tolower(uint8_t(ch))); });
    std::ranges::transform(host, host.begin(), [](char ch) { return char(std::tolower(uint8_t(ch))); });
    if (port.empty()) port = scheme == "https" ? "443" : "80";
    return scheme + "://" + host + ":" + port;
}

// A Location against the URL it came from.
std::string resolve(const std::string& base, const std::string& location)
{
    if (location.find("://") != std::string::npos) return location;
    const size_t colon = base.find("://");
    if (colon == std::string::npos) return location;
    if (location.starts_with("//")) return base.substr(0, colon + 1) + location;
    const size_t path = base.find_first_of("/?#", colon + 3);
    const std::string root = base.substr(0, path);
    if (location.starts_with('/')) return root + location;
    std::string rest = path == std::string::npos ? "/" : base.substr(path);
    rest = rest.substr(0, rest.find_first_of("?#"));
    if (location.starts_with('?')) return root + rest + location;
    return root + rest.substr(0, rest.rfind('/') + 1) + location;
}

// One request of a chain of redirects.
struct hop
{
    std::string method, url;
    bool with_body = true;    // the request's body goes too
    bool credentials = true;  // Authorization and Cookie go too
};

bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Where a redirect leads: 307 and 308 as they were, 303 (and 301 or 302
// after a POST) as a GET without the body; credentials only while still at
// the first request's origin.
hop redirected(const hop& from, int status, const std::string& to, const std::string& first_origin)
{
    hop next = from;
    next.url = to;
    if ((status == 303 && from.method != "HEAD") || ((status == 301 || status == 302) && from.method == "POST"))
        next.method = "GET", next.with_body = false;
    next.credentials = from.credentials && origin(to) == first_origin;
    return next;
}

// The options' headers for a hop: without credentials where they mustn't go.
http_headers headers_for(const hop& h, const http_headers& all)
{
    http_headers out;
    for (const auto& [name, value] : all)
        if (h.credentials || !(iequals(name, "Authorization") || iequals(name, "Cookie")))
            out.emplace_back(name, value);
    return out;
}

// -- the body -------------------------------------------------------------

// Where the final response's body goes, and how much of it has come.
struct receiver
{
    http_response& r;
    const http_options& o;
    std::uint64_t got = 0;
    std::optional<std::uint64_t> total;
    bool cut = false, stopped = false;

    // The final response's headers are in: what Content-Length promises,
    // unless it's of a compressed body that arrives undone.
    void start()
    {
        got = 0;
        total.reset();
        const std::string* encoding = header(r.headers, "Content-Encoding");
        if (const std::string* length = header(r.headers, "Content-Length");
            length && (!encoding || iequals(*encoding, "identity")))
        {
            char* end = nullptr;
            const unsigned long long n = std::strtoull(length->c_str(), &end, 10);
            if (end != length->c_str()) total = n;
        }
    }
    // A piece of it; false stops the transfer.
    bool take(const char* data, size_t n)
    {
        got += n;
        if (o.sink)
        {
            if (!o.sink(std::string_view(data, n))) return !(stopped = true);
        }
        else
        {
            r.body.append(data, n);
            if (o.max_bytes && r.body.size() >= o.max_bytes)
            {
                r.body.resize(o.max_bytes);
                return !(cut = true);
            }
        }
        if (o.progress) o.progress(got, total);
        return true;
    }
    // Why it ended early, if it did; true then.
    bool ended_early()
    {
        if (cut) r.error = "body cut short at " + std::to_string(o.max_bytes) + " bytes";
        else if (stopped) r.error = "stopped";
        return cut || stopped;
    }
};

}  // namespace

#if defined(_WIN32)

namespace
{

std::wstring wide(const std::string& s)
{
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

// WinHTTP's commonest failures in words, as libcurl gives them.
std::string failure(const char* what)
{
    const DWORD e = GetLastError();
    switch (e)
    {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "could not resolve host name";
    case ERROR_WINHTTP_TIMEOUT: return "timed out";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "could not connect";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "connection reset";
    case ERROR_WINHTTP_SECURE_FAILURE: return "TLS failure (certificate or protocol)";
    default: return std::string(what) + " (WinHTTP " + std::to_string(e) + ")";
    }
}

std::string narrow(const wchar_t* w, size_t n)
{
    std::string s(size_t(WideCharToMultiByte(CP_UTF8, 0, w, int(n), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, int(n), s.data(), int(s.size()), nullptr, nullptr);
    return s;
}

// A WinHTTP handle, closed when it goes.
struct handle
{
    HINTERNET h = nullptr;
    handle() = default;
    explicit handle(HINTERNET in) : h(in) {}
    ~handle()
    {
        if (h) WinHttpCloseHandle(h);
    }
    handle(const handle&) = delete;
    handle& operator=(const handle&) = delete;
    handle& operator=(handle&& o) noexcept
    {
        std::swap(h, o.h);
        return *this;
    }
    explicit operator bool() const { return h != nullptr; }
};

}  // namespace

http_response http_request(std::string_view method, const std::string& url, std::string_view body,
                           const http_options& o)
{
    http_response r;
    handle session(WinHttpOpen(L"rapport", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
    {
        r.error = failure("WinHTTP unavailable");
        return r;
    }
    // Connecting within the timeout; then each wait for data within it too,
    // or within the stall time (the whole request's limit is checked below).
    const int ms = int(o.timeout.count()), wait = int((o.stall.count() ? o.stall : o.timeout).count());
    WinHttpSetTimeouts(session.h, ms, ms, wait, wait);
    const auto deadline = std::chrono::steady_clock::now() + o.timeout;
    const auto late = [&] { return !o.stall.count() && std::chrono::steady_clock::now() > deadline; };
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;  // Windows 8.1 on; ignored before
    WinHttpSetOption(session.h, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);

    hop h{std::string(method), url};
    const std::string first_origin = origin(url);
    handle connection, request;
    DWORD status = 0;
    for (int redirects = 0;; ++redirects)
    {
        const std::wstring wurl = wide(h.url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof parts;
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) ||
            (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP))
        {
            r.error = redirects ? "redirected to an unsupported URL" : "unsupported URL";
            return r;
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        const std::wstring target = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                                    std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        request = handle();
        connection = handle(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
        if (connection)
            request = handle(WinHttpOpenRequest(connection.h, wide(h.method).c_str(), target.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
        if (!request)
        {
            r.error = failure("could not connect");
            return r;
        }
        // Redirects are ours (above): WinHTTP's would send the headers on.
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
        std::wstring extra;
        for (const auto& [name, value] : headers_for(h, o.headers)) extra += wide(name) + L": " + wide(value) + L"\r\n";
        const std::string_view sent = h.with_body ? body : std::string_view();
        void* data = sent.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(sent.data());
        if (!WinHttpSendRequest(request.h, extra.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra.c_str(), DWORD(-1L),
                                data, DWORD(sent.size()), DWORD(sent.size()), 0) ||
            !WinHttpReceiveResponse(request.h, nullptr))
        {
            r.error = failure("request failed");
            return r;
        }
        if (o.cancelled && *o.cancelled) return {.error = "cancelled"};

        status = 0;
        DWORD size = sizeof status;
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        r.headers.clear();
        size = 0;
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (size)
        {
            std::wstring raw(size / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                    raw.data(), &size, WINHTTP_NO_HEADER_INDEX))
            {
                const std::string text = narrow(raw.c_str(), size / sizeof(wchar_t));
                for (size_t at = 0; at < text.size();)
                {
                    const size_t end = text.find('\n', at);
                    add_header_line(std::string_view(text).substr(at, end - at), r.headers);
                    at = end == std::string::npos ? text.size() : end + 1;
                }
            }
        }
        r.url = h.url;
        const std::string* location = header(r.headers, "Location");
        if (!is_redirect(int(status)) || !location) break;
        if (redirects == max_redirects)
        {
            r.error = "too many redirects";
            return r;
        }
        h = redirected(h, int(status), resolve(h.url, *location), first_origin);
    }

    // Undone chunking and decompression are WinHTTP's.
    receiver in{r, o};
    in.start();
    char buf[65536];
    for (;;)
    {
        if (o.cancelled && *o.cancelled) return {.error = "cancelled"};
        if (late())
        {
            r.error = "timed out";
            break;
        }
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf, sizeof buf, &got))
        {
            r.error = failure("read failed");
            break;
        }
        if (got == 0 || !in.take(buf, got)) break;
    }
    in.ended_early();
    r.status = int(status);
    return r;
}

#else  // libcurl

namespace
{

// The easy interface's few calls and constants, as curl.h has them.
using CURL = void;
struct curl_slist;
using curl_off_t = long long;
constexpr int opt_writedata = 10001, opt_url = 10002, opt_postfields = 10015, opt_useragent = 10018,
              opt_httpheader = 10023, opt_headerdata = 10029, opt_customrequest = 10036,
              opt_noprogress = 43, opt_nobody = 44,
              opt_httpget = 80, opt_postfieldsize_large = 30120, opt_followlocation = 52,
              opt_nosignal = 99, opt_timeout_ms = 155, opt_connecttimeout_ms = 156, opt_accept_encoding = 10102,
              opt_xferinfodata = 10057, opt_writefunction = 20011, opt_headerfunction = 20079,
              opt_xferinfofunction = 20219;
constexpr int info_response_code = 0x200002, info_redirect_url = 0x10001F;
constexpr long global_default = 3;
constexpr int curle_write_error = 23;

struct curl_api
{
    int (*global_init)(long);
    CURL* (*easy_init)();
    int (*easy_setopt)(CURL*, int, ...);
    int (*easy_perform)(CURL*);
    int (*easy_getinfo)(CURL*, int, ...);
    void (*easy_cleanup)(CURL*);
    const char* (*easy_strerror)(int);
    curl_slist* (*slist_append)(curl_slist*, const char*);
    void (*slist_free_all)(curl_slist*);
    std::string missing;  // set when libcurl couldn't be loaded
};

const curl_api& curl()
{
    static curl_api api = []
    {
        curl_api a{};
#if defined(__APPLE__)
        const char* names[] = {"libcurl.4.dylib"};
#else
        const char* names[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl-nss.so.4"};
#endif
        for (const char* name : names)
        {
            auto lib = library::open(name);
            if (!lib) continue;
            a.global_init = lib->function<int(long)>("curl_global_init");
            a.easy_init = lib->function<CURL*()>("curl_easy_init");
            a.easy_setopt = lib->function<int(CURL*, int, ...)>("curl_easy_setopt");
            a.easy_perform = lib->function<int(CURL*)>("curl_easy_perform");
            a.easy_getinfo = lib->function<int(CURL*, int, ...)>("curl_easy_getinfo");
            a.easy_cleanup = lib->function<void(CURL*)>("curl_easy_cleanup");
            a.easy_strerror = lib->function<const char*(int)>("curl_easy_strerror");
            a.slist_append = lib->function<curl_slist*(curl_slist*, const char*)>("curl_slist_append");
            a.slist_free_all = lib->function<void(curl_slist*)>("curl_slist_free_all");
            if (a.global_init && a.easy_init && a.easy_setopt && a.easy_perform && a.easy_getinfo &&
                a.easy_cleanup && a.easy_strerror && a.slist_append && a.slist_free_all)
            {
                lib->release();  // for the life of the process
                a.global_init(global_default);  // once, before any thread uses it
                return a;
            }
        }
        a = {};
        a.missing = "HTTP needs libcurl (libcurl.so.4), which isn't installed";
        return a;
    }();
    return api;
}

// One response of the chain: its status line read from the headers, its body
// dropped while it's a redirect's, else given to the receiver.
struct transfer
{
    receiver& in;
    const http_options& o;
    int status = 0;
    bool started = false;
    std::chrono::steady_clock::time_point last_data = std::chrono::steady_clock::now();
    bool stalled = false;
};

size_t on_body(char* data, size_t size, size_t count, void* p)
{
    auto& t = *static_cast<transfer*>(p);
    const size_t n = size * count;
    t.last_data = std::chrono::steady_clock::now();
    if (is_redirect(t.status) && header(t.in.r.headers, "Location")) return n;  // followed below
    if (!t.started) t.in.start(), t.started = true;
    return t.in.take(data, n) ? n : 0;  // 0 stops the transfer
}

size_t on_header(char* data, size_t size, size_t count, void* p)
{
    auto& t = *static_cast<transfer*>(p);
    const std::string_view line(data, size * count);
    if (line.starts_with("HTTP/"))
        if (const size_t sp = line.find(' '); sp != std::string_view::npos) t.status = std::atoi(line.data() + sp + 1);
    add_header_line(line, t.in.r.headers);
    return size * count;
}

// About once a second, data or not: cancelled, or nothing for the stall time.
int on_progress(void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto& t = *static_cast<transfer*>(p);
    if (t.o.stall.count() && std::chrono::steady_clock::now() - t.last_data > t.o.stall) t.stalled = true;
    return (t.o.cancelled && *t.o.cancelled) || t.stalled ? 1 : 0;  // non-zero aborts
}

}  // namespace

http_response http_request(std::string_view method, const std::string& url, std::string_view body,
                           const http_options& o)
{
    http_response r;
    if (!url.starts_with("https://") && !url.starts_with("http://"))
    {
        r.error = "unsupported URL";
        return r;
    }
    const curl_api& c = curl();
    if (!c.missing.empty())
    {
        r.error = c.missing;
        return r;
    }
    CURL* e = c.easy_init();
    if (!e)
    {
        r.error = "libcurl failed to start";
        return r;
    }
    receiver in{r, o};
    const long ms = long(o.timeout.count());
    c.easy_setopt(e, opt_useragent, "rapport");
    c.easy_setopt(e, opt_followlocation, 0L);   // ours (above), the same as WinHTTP's
    c.easy_setopt(e, opt_accept_encoding, "");  // whatever it can decompress
    c.easy_setopt(e, opt_nosignal, 1L);         // safe on worker threads
    c.easy_setopt(e, opt_connecttimeout_ms, ms);
    if (!o.stall.count()) c.easy_setopt(e, opt_timeout_ms, ms);  // else on_progress watches
    c.easy_setopt(e, opt_writefunction, &on_body);
    c.easy_setopt(e, opt_headerfunction, &on_header);
    c.easy_setopt(e, opt_noprogress, 0L);
    c.easy_setopt(e, opt_xferinfofunction, &on_progress);

    hop h{std::string(method), url};
    const std::string first_origin = origin(url);
    int rc = 0;
    bool stalled = false;
    for (int redirects = 0;; ++redirects)
    {
        transfer t{in, o};
        c.easy_setopt(e, opt_writedata, &t);
        c.easy_setopt(e, opt_headerdata, &t);
        c.easy_setopt(e, opt_xferinfodata, &t);
        c.easy_setopt(e, opt_url, h.url.c_str());
        curl_slist* list = nullptr;
        for (const auto& [name, value] : headers_for(h, o.headers))
            list = c.slist_append(list, (name + ": " + value).c_str());
        c.easy_setopt(e, opt_httpheader, list);
        // Each hop's method from a plain GET: POST with its body; HEAD with
        // none; another verb by name, its body sent if there is one.
        const std::string_view sent = h.with_body ? body : std::string_view();
        c.easy_setopt(e, opt_customrequest, static_cast<const char*>(nullptr));
        c.easy_setopt(e, opt_nobody, 0L);
        c.easy_setopt(e, opt_httpget, 1L);
        if (h.method == "HEAD")
            c.easy_setopt(e, opt_nobody, 1L);
        else if (h.method != "GET")
        {
            if (h.method == "POST" || !sent.empty())
            {
                c.easy_setopt(e, opt_postfields, sent.data());  // not copied: outlives the transfer
                c.easy_setopt(e, opt_postfieldsize_large, curl_off_t(sent.size()));
            }
            if (h.method != "POST") c.easy_setopt(e, opt_customrequest, h.method.c_str());
        }
        rc = c.easy_perform(e);
        stalled = t.stalled;
        c.easy_setopt(e, opt_httpheader, static_cast<curl_slist*>(nullptr));
        c.slist_free_all(list);
        r.url = h.url;
        const char* next = nullptr;
        if (rc != 0 || !is_redirect(t.status) || c.easy_getinfo(e, info_redirect_url, &next) != 0 || !next) break;
        if (redirects == max_redirects)
        {
            c.easy_cleanup(e);
            r.error = "too many redirects";
            return r;
        }
        h = redirected(h, t.status, next, first_origin);
    }
    long status = 0;
    c.easy_getinfo(e, info_response_code, &status);
    c.easy_cleanup(e);

    if (o.cancelled && *o.cancelled) return {.error = "cancelled"};
    if (!(rc == curle_write_error && in.ended_early()) && rc != 0)
    {
        r.error = stalled ? "timed out (no data for " + std::to_string(o.stall.count()) + " ms)" : c.easy_strerror(rc);
        if (!status) r.body.clear();
    }
    r.status = int(status);
    return r;
}

#endif

http_response http_get(const std::string& url, const http_options& o)
{
    return http_request("GET", url, {}, o);
}

http_response http_download(const std::string& url, const std::filesystem::path& to, const http_options& o)
{
    std::filesystem::path part = to;
    part += ".part";
    std::ofstream file(part, std::ios::binary | std::ios::trunc);
    if (!file) return {.error = "can't write " + part.string()};
    http_options with = o;
    with.sink = [&](std::string_view piece)
    {
        file.write(piece.data(), std::streamsize(piece.size()));
        return bool(file) && (!o.sink || o.sink(piece));
    };
    http_response r = http_request("GET", url, {}, with);
    file.close();
    std::error_code ec;
    if (r.error.empty() && !file) r.error = "can't write " + part.string();
    if (r.error.empty() && r.status >= 200 && r.status < 300)
    {
        std::filesystem::rename(part, to, ec);
        if (ec) r.error = "can't rename " + part.string() + ": " + ec.message();
    }
    if (!r.error.empty() || r.status < 200 || r.status >= 300) std::filesystem::remove(part, ec);
    return r;
}

}  // namespace rap::sys
