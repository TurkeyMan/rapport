// rap::sys::http_get: WinHTTP on Windows; elsewhere libcurl, loaded on first
// use with the few functions it needs declared here (their interface has been
// stable for decades), so the core neither builds nor installs against it.
#include "rapport/sys.h"

#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <mutex>
#endif

namespace rap::sys
{
namespace
{

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

bool full(const http_response& r, const http_options& o)
{
    return o.max_bytes && r.body.size() >= o.max_bytes;
}

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
    case ERROR_WINHTTP_REDIRECT_FAILED: return "redirect failed";
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
    HINTERNET h;
    explicit handle(HINTERNET in) : h(in) {}
    ~handle()
    {
        if (h) WinHttpCloseHandle(h);
    }
    handle(const handle&) = delete;
    handle& operator=(const handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

}  // namespace

http_response http_get(const std::string& url, const http_options& o)
{
    http_response r;
    const std::wstring wurl = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP))
    {
        r.error = "unsupported URL";
        return r;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring target = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                                std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    handle session(WinHttpOpen(L"rapport", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
    {
        r.error = failure("WinHTTP unavailable");
        return r;
    }
    const int ms = int(o.timeout.count());
    WinHttpSetTimeouts(session.h, ms, ms, ms, ms);
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;  // Windows 8.1 on; ignored before
    WinHttpSetOption(session.h, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
    handle connection(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
    handle request(connection ? WinHttpOpenRequest(connection.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                              : nullptr);
    if (!request)
    {
        r.error = failure("could not connect");
        return r;
    }
    std::wstring extra;
    for (const auto& [name, value] : o.headers) extra += wide(name) + L": " + wide(value) + L"\r\n";
    if (!WinHttpSendRequest(request.h, extra.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra.c_str(), DWORD(-1L),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
    {
        r.error = failure("request failed");
        return r;
    }

    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    size = 0;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (size)
    {
        std::wstring raw(size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                &size, WINHTTP_NO_HEADER_INDEX))
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
    size = 0;
    WinHttpQueryOption(request.h, WINHTTP_OPTION_URL, nullptr, &size);
    if (size)
    {
        std::wstring final_url(size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryOption(request.h, WINHTTP_OPTION_URL, final_url.data(), &size))
            r.url = narrow(final_url.c_str(), wcslen(final_url.c_str()));
    }

    // Undone chunking and decompression are WinHTTP's; redirects are followed.
    char buf[16384];
    for (;;)
    {
        if (o.cancelled && *o.cancelled) return {.error = "cancelled"};
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf, sizeof buf, &got))
        {
            r.error = failure("read failed");
            break;
        }
        if (got == 0) break;
        r.body.append(buf, got);
        if (full(r, o))
        {
            r.body.resize(o.max_bytes);
            r.error = "body cut short at " + std::to_string(o.max_bytes) + " bytes";
            break;
        }
    }
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
constexpr int opt_writedata = 10001, opt_url = 10002, opt_useragent = 10018, opt_httpheader = 10023,
              opt_headerdata = 10029, opt_noprogress = 43, opt_followlocation = 52, opt_maxredirs = 68,
              opt_nosignal = 99, opt_timeout_ms = 155, opt_connecttimeout_ms = 156,
              opt_accept_encoding = 10102, opt_xferinfodata = 10057, opt_writefunction = 20011,
              opt_headerfunction = 20079, opt_xferinfofunction = 20219;
constexpr int info_effective_url = 0x100001, info_response_code = 0x200002;
constexpr long global_default = 3;

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

struct transfer
{
    http_response& r;
    const http_options& o;
    bool cut = false;
};

size_t on_body(char* data, size_t size, size_t count, void* p)
{
    auto& t = *static_cast<transfer*>(p);
    const size_t n = size * count;
    t.r.body.append(data, n);
    if (full(t.r, t.o))
    {
        t.cut = true;
        return 0;  // stops the transfer
    }
    return n;
}

size_t on_header(char* data, size_t size, size_t count, void* p)
{
    auto& t = *static_cast<transfer*>(p);
    add_header_line(std::string_view(data, size * count), t.r.headers);
    return size * count;
}

int on_progress(void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto& t = *static_cast<transfer*>(p);
    return t.o.cancelled && *t.o.cancelled ? 1 : 0;  // non-zero aborts
}

}  // namespace

http_response http_get(const std::string& url, const http_options& o)
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
    CURL* h = c.easy_init();
    if (!h)
    {
        r.error = "libcurl failed to start";
        return r;
    }
    transfer t{r, o};
    curl_slist* list = nullptr;
    for (const auto& [name, value] : o.headers) list = c.slist_append(list, (name + ": " + value).c_str());
    const long ms = long(o.timeout.count());
    c.easy_setopt(h, opt_url, url.c_str());
    c.easy_setopt(h, opt_useragent, "rapport");
    c.easy_setopt(h, opt_httpheader, list);
    c.easy_setopt(h, opt_followlocation, 1L);
    c.easy_setopt(h, opt_maxredirs, 10L);
    c.easy_setopt(h, opt_accept_encoding, "");  // whatever it can decompress
    c.easy_setopt(h, opt_nosignal, 1L);         // safe on worker threads
    c.easy_setopt(h, opt_connecttimeout_ms, ms);
    c.easy_setopt(h, opt_timeout_ms, ms);
    c.easy_setopt(h, opt_writefunction, &on_body);
    c.easy_setopt(h, opt_writedata, &t);
    c.easy_setopt(h, opt_headerfunction, &on_header);
    c.easy_setopt(h, opt_headerdata, &t);
    c.easy_setopt(h, opt_noprogress, 0L);
    c.easy_setopt(h, opt_xferinfofunction, &on_progress);
    c.easy_setopt(h, opt_xferinfodata, &t);
    const int rc = c.easy_perform(h);
    long status = 0;
    c.easy_getinfo(h, info_response_code, &status);
    const char* final_url = nullptr;
    if (c.easy_getinfo(h, info_effective_url, &final_url) == 0 && final_url) r.url = final_url;
    c.easy_cleanup(h);
    c.slist_free_all(list);

    if (o.cancelled && *o.cancelled) return {.error = "cancelled"};
    if (t.cut)
    {
        r.body.resize(o.max_bytes);
        r.error = "body cut short at " + std::to_string(o.max_bytes) + " bytes";
    }
    else if (rc != 0)
    {
        r.error = c.easy_strerror(rc);
        if (!status) r.body.clear();
    }
    r.status = int(status);
    return r;
}

#endif

}  // namespace rap::sys
