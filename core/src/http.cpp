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
#include <condition_variable>
#include <memory>
#include <mutex>

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

// A URI scheme can only appear at the start of a reference.
bool has_scheme(std::string_view ref)
{
    constexpr std::string_view letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    constexpr std::string_view scheme_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+.-";
    if (ref.empty() || letters.find(ref.front()) == std::string_view::npos) return false;
    const size_t end = ref.find_first_not_of(scheme_chars);
    return end != std::string_view::npos && ref[end] == ':';
}

// A Location against the URL it came from.
std::string resolve(const std::string& base, const std::string& location)
{
    if (has_scheme(location)) return location;
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

// Spaces after the authority as %20: servers send "Location: /a b" unescaped,
// and browsers follow it so. One in the authority stays, for url_origin to
// refuse, as it does every other control character anywhere.
std::string spaces_escaped(std::string url)
{
    const size_t colon = url.find("://");
    if (colon == std::string::npos) return url;
    const size_t path = url.find_first_of("/?#", colon + 3);
    for (size_t at = path; (at = url.find(' ', at)) != std::string::npos; at += 3) url.replace(at, 1, "%20");
    return url;
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
hop redirected(const hop& from, int status, const std::string& to,
               const std::optional<origin>& first_origin)
{
    hop next = from;
    next.url = to;
    if ((status == 303 && from.method != "HEAD") || ((status == 301 || status == 302) && from.method == "POST"))
        next.method = "GET", next.with_body = false;
    next.credentials = from.credentials && first_origin && url_origin(to) == first_origin;
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

// A request's hops, the same for both transports, which do only the I/O:
// each hop's URL http: or https: (a redirect to file: would read a local
// file), at most max_redirects of them, and all of them within the
// request's time limit.
class chain
{
public:
    chain(std::string_view method, const std::string& url, const http_options& o)
        : o_(o)
        , hop_{std::string(method), url}
        , first_(url_origin(url))
        , deadline_(std::chrono::steady_clock::now() + o.timeout)
    {
    }
    const hop& current() const { return hop_; }

    // Why the current hop mustn't be sent; "" when it may.
    std::string refused() const
    {
        if (!url_origin(hop_.url))
            return redirects_ ? "redirected to an unsupported URL" : "unsupported URL";
        if (redirects_ > max_redirects) return "too many redirects";
        if (late()) return "timed out";
        return {};
    }
    // The time the next wait may take: what's left of the whole request's
    // limit (at least 1 ms, as 0 is no limit to both libraries); with
    // `stall` set, the limit for connecting, each hop afresh.
    std::chrono::milliseconds left() const
    {
        if (o_.stall.count()) return o_.timeout;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline_ - std::chrono::steady_clock::now());
        return std::max(ms, std::chrono::milliseconds(1));
    }
    bool late() const { return !o_.stall.count() && std::chrono::steady_clock::now() >= deadline_; }

    // A response came: true, and the next hop current, if it's a redirect
    // to follow (`location` as sent, or absolute; its spaces escaped).
    bool follow(int status, const std::string* location)
    {
        if (!is_redirect(status) || !location) return false;
        ++redirects_;
        hop_ = redirected(hop_, status, spaces_escaped(resolve(hop_.url, *location)), first_);
        return true;
    }

private:
    const http_options& o_;
    hop hop_;
    std::optional<origin> first_;
    std::chrono::steady_clock::time_point deadline_;
    int redirects_ = 0;
};

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

std::string origin::str() const
{
    return scheme + "://" + host + ":" + std::to_string(port);
}

std::optional<origin> url_origin(std::string_view url)
{
    const auto lower = [](std::string_view s)
    {
        std::string out(s);
        std::ranges::transform(out, out.begin(),
                               [](char c) { return char(std::tolower(uint8_t(c))); });
        return out;
    };
    // A NUL, any other control character, a space or DEL: libcurl and
    // WinHTTP read a URL only up to its NUL and treat the rest their own
    // way, so "http://other\0@trusted/" would be parsed here as trusted
    // but connect to other. No valid URL has them unescaped.
    if (std::ranges::any_of(url, [](char c) { return uint8_t(c) <= 0x20 || uint8_t(c) == 0x7f; }))
        return std::nullopt;
    const size_t colon = url.find(':');
    if (colon == std::string_view::npos || url.substr(colon + 1, 2) != "//") return std::nullopt;
    origin o{.scheme = lower(url.substr(0, colon))};
    if (o.scheme != "http" && o.scheme != "https") return std::nullopt;
    std::string_view authority = url.substr(colon + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (const size_t at = authority.rfind('@'); at != std::string_view::npos)
        authority.remove_prefix(at + 1);

    // The host: [an IPv6 literal], or names of letters, digits and -._~ (or
    // UTF-8) a dot apart, maybe with a dot after the last.
    std::string_view host = authority, port;
    if (host.starts_with('['))
    {
        const size_t close = host.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        port = host.substr(close + 1);
        host = host.substr(0, close + 1);
        const std::string_view inside = host.substr(1, host.size() - 2);
        const auto v6 = [](char c) { return std::isxdigit(uint8_t(c)) || c == ':' || c == '.'; };
        if (inside.find(':') == std::string_view::npos || !std::ranges::all_of(inside, v6))
            return std::nullopt;
    }
    else
    {
        const size_t c = host.find(':');
        port = c == std::string_view::npos ? std::string_view() : host.substr(c);
        host = host.substr(0, c);
        if (host.ends_with('.')) host.remove_suffix(1);
        const auto named = [](char ch)
        { return std::isalnum(uint8_t(ch)) || uint8_t(ch) >= 0x80 || std::strchr("-._~", ch); };
        if (host.empty() || host.starts_with('.') || host.ends_with('.') ||
            host.find("..") != std::string_view::npos || !std::ranges::all_of(host, named))
            return std::nullopt;
    }
    o.host = lower(host);

    // The port: the scheme's when there's none, or nothing after the colon.
    if (!port.empty())
    {
        if (port.front() != ':') return std::nullopt;
        port.remove_prefix(1);
    }
    if (port.empty())
    {
        o.port = o.scheme == "https" ? 443 : 80;
        return o;
    }
    unsigned n = 0;
    for (char c : port)
        if (!std::isdigit(uint8_t(c)) || (n = n * 10 + unsigned(c - '0')) > 65535)
            return std::nullopt;
    if (n == 0) return std::nullopt;
    o.port = std::uint16_t(n);
    return o;
}

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

// What WinHTTP's thread says of a request's call: whether it's done, and how.
// With it, what WinHTTP may write or read until it's done with the request,
// which, should it never say so, outlives the call to http_request.
struct completion
{
    std::mutex m;
    std::condition_variable cv;
    bool ready = false, closed = false;
    DWORD error = 0;  // the call's, if it failed
    DWORD bytes = 0;  // a read's, into `buf`
    std::string body;  // sent
    char buf[65536];
};

void CALLBACK on_status(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD length)
{
    auto* c = reinterpret_cast<completion*>(context);
    if (!c) return;
    std::lock_guard<std::mutex> lock(c->m);
    switch (status)
    {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: c->bytes = length; break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: c->error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError; break;
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING: c->closed = true; break;
    default: return;
    }
    c->ready = true;
    c->cv.notify_all();
}

// A request in WinHTTP's asynchronous mode, each call begun and then waited
// for here: the way to end one on time, as only an asynchronous request may
// be closed while a call of its is under way. (A synchronous one's calls
// can't be cut short, and its own timeouts go by each wait rather than the
// whole, in coarse steps: a read on a quiet server ran on to the next 4 s.)
class async_request
{
public:
    // How a call ended: on its own, or by the wait's limit or the request
    // being cancelled, which close the request.
    enum class end { done, failed, timed_out, cancelled };
    using time = std::chrono::steady_clock::time_point;

    async_request(HINTERNET connection, const std::wstring& verb, const std::wstring& target, bool secure)
        : c_(new completion)
    {
        h_ = WinHttpOpenRequest(connection, verb.c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        const DWORD_PTR context = DWORD_PTR(c_);
        if (h_ && (WinHttpSetStatusCallback(h_, on_status,
                                            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
                                            0) == WINHTTP_INVALID_STATUS_CALLBACK ||
                   !WinHttpSetOption(h_, WINHTTP_OPTION_CONTEXT_VALUE, const_cast<DWORD_PTR*>(&context),
                                     sizeof context)))
        {
            c_->error = GetLastError();
            WinHttpCloseHandle(std::exchange(h_, nullptr));  // calls back with no context, if at all
        }
        else if (!h_)
            c_->error = GetLastError();
    }
    // Closed, and its completion freed once WinHTTP is done with it.
    ~async_request()
    {
        close();
        delete c_;
    }
    async_request(const async_request&) = delete;
    async_request& operator=(const async_request&) = delete;

    explicit operator bool() const { return h_ != nullptr; }
    HINTERNET get() const { return h_; }
    // The last call's error, when it failed.
    DWORD error() const { return c_ ? c_->error : 0; }

    // The body copied (WinHTTP reads it till it's done with the request).
    end send(const std::wstring& headers, std::string_view body, time until, const std::atomic<bool>* cancelled)
    {
        if (!arm()) return end::failed;
        c_->body = body;
        void* data = body.empty() ? WINHTTP_NO_REQUEST_DATA : c_->body.data();
        return wait(WinHttpSendRequest(h_, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                       DWORD(-1L), data, DWORD(body.size()), DWORD(body.size()), DWORD_PTR(c_)),
                    until, cancelled);
    }
    end receive(time until, const std::atomic<bool>* cancelled)
    {
        if (!arm()) return end::failed;
        return wait(WinHttpReceiveResponse(h_, nullptr), until, cancelled);
    }
    // A piece of the body, valid till the next read; empty at its end.
    end read(std::string_view& got, time until, const std::atomic<bool>* cancelled)
    {
        got = {};
        if (!arm()) return end::failed;
        const end e = wait(WinHttpReadData(h_, c_->buf, sizeof c_->buf, nullptr), until, cancelled);
        if (e == end::done) got = std::string_view(c_->buf, c_->bytes);
        return e;
    }

private:
    bool arm()
    {
        if (!h_) return false;
        std::lock_guard<std::mutex> lock(c_->m);
        c_->ready = false;
        c_->error = c_->bytes = 0;
        return true;
    }

    // The call begun's end; at `until`, or once cancelled, the request
    // closed. `cancelled` is a flag, so it's looked at every 50 ms.
    end wait(BOOL begun, time until, const std::atomic<bool>* cancelled)
    {
        using namespace std::chrono_literals;
        std::unique_lock<std::mutex> lock(c_->m);
        if (!begun)
        {
            c_->error = GetLastError();
            return end::failed;
        }
        for (;;)
        {
            if (c_->ready) return c_->error ? end::failed : end::done;
            const auto now = std::chrono::steady_clock::now();
            const end e = cancelled && *cancelled ? end::cancelled : now >= until ? end::timed_out : end::done;
            if (e != end::done)
            {
                lock.unlock();
                close();
                return e;
            }
            c_->cv.wait_until(lock, std::min(until, now + 50ms));
        }
    }

    // Closes the request, ending a call under way, and waits for WinHTTP's
    // last word on it; should that not come, the completion is left to it.
    void close()
    {
        if (!h_) return;
        WinHttpCloseHandle(std::exchange(h_, nullptr));
        std::unique_lock<std::mutex> lock(c_->m);
        if (!c_->cv.wait_for(lock, std::chrono::seconds(5), [&] { return c_->closed; }))
        {
            lock.unlock();
            c_ = nullptr;
        }
    }

    HINTERNET h_ = nullptr;
    completion* c_;
};

}  // namespace

http_response http_request(std::string_view method, const std::string& url, std::string_view body,
                           const http_options& o)
{
    using clock = std::chrono::steady_clock;
    http_response r;
    handle session(WinHttpOpen(L"rapport", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!session)
    {
        r.error = failure("WinHTTP unavailable");
        return r;
    }
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;  // Windows 8.1 on; ignored before
    WinHttpSetOption(session.h, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);

    chain hops(method, url, o);
    handle connection;
    std::unique_ptr<async_request> request;  // after the connection: closed before it
    // Waiting for the response, and for each piece of the body: within the
    // stall time, once connected, or what's left of the whole request's
    // limit. Connecting is within that limit, or with a stall time, within
    // its own.
    const auto data_by = [&] { return clock::now() + (o.stall.count() ? o.stall : hops.left()); };
    // Why a call didn't end well, in words ("" when it did).
    const auto outcome = [&](async_request::end e, const char* what, bool data) -> std::string
    {
        switch (e)
        {
        case async_request::end::done: return {};
        case async_request::end::cancelled: return "cancelled";
        case async_request::end::timed_out:
            return data && o.stall.count() ? "timed out (no data for " + std::to_string(o.stall.count()) + " ms)"
                                           : "timed out";
        case async_request::end::failed: break;
        }
        SetLastError(request->error());
        return failure(what);
    };
    DWORD status = 0;
    for (;;)
    {
        if (o.cancelled && *o.cancelled) return {.error = "cancelled"};
        if (std::string why = hops.refused(); !why.empty())
        {
            r.error = std::move(why);
            return r;
        }
        const hop& h = hops.current();
        const std::wstring wurl = wide(h.url);
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
        request.reset();
        connection = handle(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
        if (!connection)
        {
            r.error = failure("could not connect");
            return r;
        }
        request = std::make_unique<async_request>(connection.h, wide(h.method), target,
                                                  parts.nScheme == INTERNET_SCHEME_HTTPS);
        if (!*request)
        {
            SetLastError(request->error());
            r.error = failure("could not connect");
            return r;
        }
        // Ending the request on time is ours (above): WinHTTP's own limits,
        // for resolving and connecting what's left, for sending and
        // receiving only a backstop well after ours, as its coarse timer
        // goes off early as well as late.
        const auto left = hops.left();
        const int backstop = int(std::max(left, o.stall).count()) + 5000;
        WinHttpSetTimeouts(request->get(), int(left.count()), int(left.count()), backstop, backstop);
        // Redirects are ours (above): WinHTTP's would send the headers on.
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request->get(), WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
        std::wstring extra;
        for (const auto& [name, value] : headers_for(h, o.headers)) extra += wide(name) + L": " + wide(value) + L"\r\n";
        // Sent (connected, through TLS, the request and its body out), then
        // its response's headers in.
        std::string why = outcome(request->send(extra, h.with_body ? body : std::string_view(),
                                                clock::now() + left, o.cancelled),
                                  "request failed", false);
        if (why.empty() && o.cancelled && *o.cancelled) why = "cancelled";
        if (why.empty() && hops.late()) why = "timed out";
        if (why.empty()) why = outcome(request->receive(data_by(), o.cancelled), "request failed", true);
        if (why == "cancelled") return {.error = "cancelled"};
        if (!why.empty())
        {
            r.error = std::move(why);
            return r;
        }

        status = 0;
        DWORD size = sizeof status;
        WinHttpQueryHeaders(request->get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        r.headers.clear();
        size = 0;
        WinHttpQueryHeaders(request->get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                            &size, WINHTTP_NO_HEADER_INDEX);
        if (size)
        {
            std::wstring raw(size / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(request->get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
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
        if (!hops.follow(int(status), header(r.headers, "Location"))) break;
    }

    // Undone chunking and decompression are WinHTTP's. A read that's done is
    // taken, however late; cancelling and the limit are looked at before the
    // next, as reads of a body that's there already end at once.
    receiver in{r, o};
    in.start();
    for (;;)
    {
        if (o.cancelled && *o.cancelled) return {.error = "cancelled"};
        if (hops.late())
        {
            r.error = "timed out";
            break;
        }
        std::string_view got;
        std::string why = outcome(request->read(got, data_by(), o.cancelled), "read failed", true);
        if (why == "cancelled") return {.error = "cancelled"};
        if (!why.empty())
        {
            r.error = std::move(why);
            break;
        }
        if (got.empty() || !in.take(got.data(), got.size())) break;
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
// The protocols it may speak: by name from 7.85; as a mask before (since
// deprecated, still heeded).
constexpr int opt_protocols_str = 10318, opt_redir_protocols_str = 10319, opt_protocols = 181,
              opt_redir_protocols = 182;
constexpr long proto_http = 1 << 0, proto_https = 1 << 1;
constexpr int info_response_code = 0x200002, info_redirect_url = 0x10001F, info_pretransfer_time_t = 0x600035;
constexpr long global_default = 3;
constexpr int curle_write_error = 23, curle_operation_timedout = 28;

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
    const curl_api& c;
    CURL* e;
    int status = 0;
    bool started = false;
    bool connected = false;  // through TLS: the stall time runs from then
    std::chrono::steady_clock::time_point last_data = std::chrono::steady_clock::now();
    bool stalled = false;
};

size_t on_body(char* data, size_t size, size_t count, void* p)
{
    auto& t = *static_cast<transfer*>(p);
    const size_t n = size * count;
    t.last_data = std::chrono::steady_clock::now();
    t.connected = true;
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

// About once a second, data or not: cancelled, or nothing for the stall time
// since connected (connecting has the time limit, as WinHTTP's does).
int on_progress(void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto& t = *static_cast<transfer*>(p);
    if (t.o.stall.count() && !t.connected)
    {
        curl_off_t ready = 0;  // microseconds to the transfer's start; 0 before
        if (t.c.easy_getinfo(t.e, info_pretransfer_time_t, &ready) == 0 && ready > 0)
            t.connected = true, t.last_data = std::chrono::steady_clock::now();
    }
    if (t.o.stall.count() && t.connected && std::chrono::steady_clock::now() - t.last_data > t.o.stall)
        t.stalled = true;
    return (t.o.cancelled && *t.o.cancelled) || t.stalled ? 1 : 0;  // non-zero aborts
}

}  // namespace

http_response http_request(std::string_view method, const std::string& url, std::string_view body,
                           const http_options& o)
{
    http_response r;
    chain hops(method, url, o);
    if (std::string why = hops.refused(); !why.empty())
    {
        r.error = std::move(why);
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
    c.easy_setopt(e, opt_useragent, "rapport");
    c.easy_setopt(e, opt_followlocation, 0L);   // ours (above), the same as WinHTTP's
    c.easy_setopt(e, opt_accept_encoding, "");  // whatever it can decompress
    c.easy_setopt(e, opt_nosignal, 1L);         // safe on worker threads
    // Only http(s), as the chain checks too: no file: or the like, whatever
    // the URL turns out to be to libcurl.
    if (c.easy_setopt(e, opt_protocols_str, "http,https") != 0)
        c.easy_setopt(e, opt_protocols, proto_http | proto_https);
    if (c.easy_setopt(e, opt_redir_protocols_str, "http,https") != 0)
        c.easy_setopt(e, opt_redir_protocols, proto_http | proto_https);
    c.easy_setopt(e, opt_writefunction, &on_body);
    c.easy_setopt(e, opt_headerfunction, &on_header);
    c.easy_setopt(e, opt_noprogress, 0L);
    c.easy_setopt(e, opt_xferinfofunction, &on_progress);

    int rc = 0;
    bool stalled = false;
    for (;;)
    {
        if (std::string why = hops.refused(); !why.empty())
        {
            c.easy_cleanup(e);
            r.error = std::move(why);
            return r;
        }
        const hop& h = hops.current();
        // Connecting, and with no stall time the whole hop, within what's
        // left of the request's limit (else on_progress watches).
        const long left = long(hops.left().count());
        c.easy_setopt(e, opt_connecttimeout_ms, left);
        if (!o.stall.count()) c.easy_setopt(e, opt_timeout_ms, left);
        transfer t{in, o, c, e};
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
        if (rc != 0 || c.easy_getinfo(e, info_redirect_url, &next) != 0 || !next) break;
        const std::string to = next;  // resolved by libcurl
        if (!hops.follow(t.status, &to)) break;
    }
    long status = 0;
    c.easy_getinfo(e, info_response_code, &status);
    c.easy_cleanup(e);

    if (o.cancelled && *o.cancelled) return {.error = "cancelled"};
    if (!(rc == curle_write_error && in.ended_early()) && rc != 0)
    {
        // Timeouts in WinHTTP's words, the same everywhere.
        r.error = stalled                           ? "timed out (no data for " + std::to_string(o.stall.count()) + " ms)"
                  : rc == curle_operation_timedout ? "timed out"
                                                   : c.easy_strerror(rc);
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
