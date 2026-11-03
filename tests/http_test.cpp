// rap::sys's HTTP: the origins credentials are tied to, and requests to a
// server of the test's own on the loopback (redirects elsewhere, and the
// whole request's time limit across them).
//   rapport-http-test
#include "rapport/sys.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCKET = -1;
#define close_socket ::close
#endif

namespace
{

using namespace std::chrono_literals;

int fails = 0;

void check(bool ok, const char* what)
{
    if (!ok) ++fails;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

std::string origin_of(std::string_view url)
{
    const auto o = rap::sys::url_origin(url);
    return o ? o->str() : "none";
}

void eq(const std::string& got, const std::string& want, const char* what)
{
    if (got != want) std::printf("     got [%s] want [%s]\n", got.c_str(), want.c_str());
    check(got == want, what);
}

void origins()
{
    eq(origin_of("https://example.com/a/b?c#d"), "https://example.com:443",
       "origin: https's default port");
    eq(origin_of("http://example.com"), "http://example.com:80", "origin: http's default port");
    eq(origin_of("https://example.com:8443/x"), "https://example.com:8443",
       "origin: an explicit port");
    eq(origin_of("https://example.com:443/"), "https://example.com:443",
       "origin: the default port spelt out");
    eq(origin_of("https://example.com:/"), "https://example.com:443",
       "origin: an empty port is the default");
    eq(origin_of("http://[::1]:8443/x"), "http://[::1]:8443",
       "origin: an IPv6 literal with a port");
    eq(origin_of("https://[2001:DB8::1]/"), "https://[2001:db8::1]:443",
       "origin: an IPv6 literal, lower case");
    eq(origin_of("https://user:pass@host.example/x"), "https://host.example:443",
       "origin: userinfo isn't the host");
    eq(origin_of("https://user@evil.example@good.example/"), "https://good.example:443",
       "origin: the host follows the last @");
    eq(origin_of("https://good.example:443@evil.example/"), "https://evil.example:443",
       "origin: a host:port before @ is userinfo");
    eq(origin_of("https://evil.example/@good.example"), "https://evil.example:443",
       "origin: an @ in the path isn't userinfo");
    eq(origin_of("HTTPS://Example.COM/Path"), "https://example.com:443", "origin: upper case");
    eq(origin_of("https://example.com./x"), "https://example.com:443", "origin: a trailing dot");
    eq(origin_of("https://127.0.0.1:8080"), "https://127.0.0.1:8080", "origin: an IPv4 address");
    check(rap::sys::url_origin("https://a.example") ==
              rap::sys::url_origin("https://A.example.:443/x"),
          "origin: equal origins compare equal");
    check(rap::sys::url_origin("https://a.example") != rap::sys::url_origin("http://a.example"),
          "origin: http and https differ");

    // Explicit-length URLs with a NUL or other control character: the
    // transports stop at the NUL, so the part after it must never decide
    // the origin (http://other.example\0@trusted.example/ connects to other).
    using namespace std::string_view_literals;
    for (std::string_view bad :
         {"http://other.example\0@trusted.example/"sv, "https://trusted.example\0.other.example/"sv,
          "https://trusted.example/\0"sv, "https://a.example\r\nHost: b/"sv,
          "https://a.example\t/"sv, "https://a .example/"sv, "https://a.example\x7f/"sv})
        check(!rap::sys::url_origin(bad), "origin: none for a URL with a NUL, control or space");

    const char* malformed[] = {
        "",
        "example.com",
        "/relative/path",
        "//example.com/x",
        "https:example.com",
        "https:/example.com",
        "https://",
        "https:///path",
        "https://user@/x",
        "https://:443/",
        "https://example.com:0/",
        "https://example.com:65536/",
        "https://example.com:44a/",
        "https://example.com:-1/",
        "https://example.com:443:80/",
        "https://exa mple.com/",
        "https://example.com../",
        "https://.example.com/",
        "https://a..b/",
        "https://exa\\mple.com/",
        "https://[::1/",
        "https://[::1]x/",
        "https://[]/",
        "https://[1.2.3.4]/",
        "https://[::g]/",
        "file:///etc/hostname",
        "ftp://example.com/",
        "ssh://git@example.com:22/x",
        "javascript://example.com/",
    };
    int refused = 0;
    for (const char* url : malformed)
        if (rap::sys::url_origin(url))
            std::printf("     [%s] has an origin\n", url);
        else
            ++refused;
    check(refused == int(std::size(malformed)),
          "origin: none for malformed and other schemes' URLs");
}

// An HTTP/1.1 server on 127.0.0.1, one connection at a time, each closed
// after its response: `answer` maps a request's path to the response. A
// path starting /quiet is then held open, silent, for up to 3 s, until the
// client goes.
class server
{
public:
    explicit server(std::function<std::string(std::string_view path)> answer)
        : answer_(std::move(answer))
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof a;
        if (listener_ == INVALID_SOCKET || ::bind(listener_, (sockaddr*)&a, sizeof a) != 0 ||
            ::listen(listener_, 8) != 0 || ::getsockname(listener_, (sockaddr*)&a, &size) != 0)
            return;
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~server()
    {
        stop_ = true;
        if (thread_.joinable())
        {
            // Wakes accept() for it to see stop_.
            socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(port_);
            ::connect(s, (sockaddr*)&a, sizeof a);
            close_socket(s);
            thread_.join();
        }
        if (listener_ != INVALID_SOCKET) close_socket(listener_);
    }
    std::string url(std::string_view path) const
    {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }
    explicit operator bool() const { return port_ != 0; }

private:
    void serve()
    {
        while (!stop_)
        {
            socket_t c = ::accept(listener_, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            std::string request;
            char buf[4096];
            while (request.find("\r\n\r\n") == std::string::npos)
            {
                const int n = int(::recv(c, buf, sizeof buf, 0));
                if (n <= 0) break;
                request.append(buf, size_t(n));
            }
            // "GET /path HTTP/1.1"
            const size_t from = request.find(' ') + 1, to = request.find(' ', from);
            if (!stop_ && from != 0 && to != std::string::npos)
            {
                const std::string reply =
                    answer_(std::string_view(request).substr(from, to - from));
                ::send(c, reply.data(), int(reply.size()), 0);
                if (std::string_view(request).substr(from).starts_with("/quiet")) hold(c);
            }
            close_socket(c);
        }
    }

    // Until the client closes its end, 3 s pass, or the server stops.
    void hold(socket_t c)
    {
        for (int i = 0; i < 60 && !stop_; ++i)
        {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(c, &readable);
            timeval wait{0, 50000};
            if (::select(int(c) + 1, &readable, nullptr, nullptr, &wait) > 0)
            {
                char b;
                if (::recv(c, &b, 1, 0) <= 0) return;
            }
        }
    }

    std::function<std::string(std::string_view)> answer_;
    socket_t listener_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_ = false;
    std::thread thread_;
};

std::string response(std::string_view status, std::string_view location, std::string_view body)
{
    std::string r = "HTTP/1.1 " + std::string(status) + "\r\nConnection: close\r\n";
    if (!location.empty()) r += "Location: " + std::string(location) + "\r\n";
    return r + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

// "file:///..." of a local path.
std::string file_url(const std::filesystem::path& p)
{
    std::string s = rap::sys::path_to_utf8(p);
    for (char& c : s)
        if (c == '\\') c = '/';
    return s.starts_with('/') ? "file://" + s : "file:///" + s;
}

void requests()
{
    // A local file, which no redirect may read.
    const std::filesystem::path secret = std::filesystem::temp_directory_path() /
                                         ("rapport-http-test-" + std::to_string(std::rand()));
    std::ofstream(secret) << "local secret";
    const std::string secret_url = file_url(secret);

    server s(
        [&](std::string_view path) -> std::string
        {
            if (path == "/ok") return response("200 OK", {}, "hello");
            if (path == "/to-file") return response("302 Found", secret_url, "");
            if (path == "/to-ftp") return response("302 Found", "ftp://127.0.0.1/x", "");
            if (path == "/loop") return response("302 Found", "/loop", "");
            if (path == "/spaced") return response("302 Found", "/a b?q=c d", "");
            // libcurl resolves a redirect itself, a query's space as '+'.
            if (path == "/a%20b?q=c%20d" || path == "/a%20b?q=c+d") return response("200 OK", {}, "spaced");
            // /quiet: some of a long body, then nothing (held open above).
            // /stream: a body of 8 MB, all there to read at once.
            if (path == "/stream") return response("200 OK", {}, std::string(8 << 20, 'x'));
            if (path == "/quiet")
                return "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 1000000\r\n\r\n" +
                       std::string(65536, 'x');
            // /slow/N: each answer 700 ms late, N redirects before the last.
            if (path.starts_with("/slow/"))
            {
                std::this_thread::sleep_for(700ms);
                const int n = std::atoi(path.data() + 6);
                return n > 0 ? response("302 Found", "/slow/" + std::to_string(n - 1), "")
                             : response("200 OK", {}, "done");
            }
            return response("404 Not Found", {}, "");
        });
    if (!s)
    {
        check(false, "http: a server on the loopback");
        return;
    }

    auto ok = rap::sys::http_get(s.url("/ok"));
    if (ok.error.find("libcurl") != std::string::npos)
    {
        std::printf("skip http: %s\n", ok.error.c_str());
        std::filesystem::remove(secret);
        return;
    }
    check(ok.status == 200 && ok.body == "hello" && ok.error.empty(), "http: a plain GET");

    bool refused = true;
    for (const char* path : {"/to-file", "/to-ftp"})
    {
        auto r = rap::sys::http_get(s.url(path));
        if (!r.error.empty() && r.body.empty() && r.status == 0) continue;
        std::printf("     %s: status %d, error [%s], body [%s]\n", path, r.status, r.error.c_str(),
                    r.body.c_str());
        refused = false;
    }
    check(refused, "http: redirects to file: and ftp: are refused, with no body");
    auto direct = rap::sys::http_get(secret_url);
    check(!direct.error.empty() && direct.body.empty(), "http: a file: URL is refused");

    auto loop = rap::sys::http_get(s.url("/loop"));
    check(loop.error == "too many redirects" && loop.body.empty(), "http: endless redirects stop");

    // The whole request's limit is for all its hops: four answers of 700 ms
    // each don't fit in 1.5 s.
    const auto start = std::chrono::steady_clock::now();
    auto slow = rap::sys::http_get(s.url("/slow/3"), {.timeout = 1500ms});
    const auto took = std::chrono::steady_clock::now() - start;
    if (slow.error.empty() || took > 2300ms)
        std::printf("     status %d, error [%s], took %lld ms\n", slow.status, slow.error.c_str(),
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(took).count());
    check(!slow.error.empty() && slow.body.empty() && took < 2300ms,
          "http: the time limit is for the whole chain of redirects");
    auto in_time = rap::sys::http_get(s.url("/slow/1"), {.timeout = 3000ms});
    check(in_time.status == 200 && in_time.body == "done", "http: redirects within the limit");

    // A Location with spaces, as some servers send it: followed escaped.
    auto spaced = rap::sys::http_get(s.url("/spaced"));
    if (spaced.body != "spaced")
        std::printf("     url [%s], error [%s]\n", spaced.url.c_str(), spaced.error.c_str());
    check(spaced.status == 200 && spaced.body == "spaced" && spaced.url.find(' ') == std::string::npos,
          "http: a redirect's spaces are escaped");

    // A server gone quiet mid-body ends the request on time, not some
    // seconds after (WinHTTP's own timeouts go in steps of 4 s), whether by
    // the stall time, the whole request's limit, or being cancelled. libcurl
    // looks about once a second, hence the slack.
    const auto quiet = [&](const rap::sys::http_options& o, const char* what, auto ok)
    {
        const auto from = std::chrono::steady_clock::now();
        auto r = rap::sys::http_get(s.url("/quiet"), o);
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from);
        const bool good = ok(r) && ms < 2000ms;
        if (!good) std::printf("     error [%s], took %lld ms\n", r.error.c_str(), (long long)ms.count());
        check(good, what);
    };
    quiet({.stall = 500ms}, "http: a stall ends a quiet transfer on time",
          [](const auto& r) { return r.error.find("no data for 500 ms") != std::string::npos; });
    quiet({.timeout = 1000ms}, "http: the time limit ends a quiet transfer on time",
          [](const auto& r) { return r.error == "timed out"; });
    std::atomic<bool> cancel = false;
    std::thread canceller(
        [&]
        {
            std::this_thread::sleep_for(300ms);
            cancel = true;
        });
    quiet({.timeout = 10000ms, .cancelled = &cancel}, "http: cancelling ends a quiet transfer at once",
          [](const auto& r) { return r.error == "cancelled"; });
    canceller.join();

    // A body whose every read ends at once still stops when cancelled, and
    // at the limit: taken slowly, 20 ms a piece, it would take seconds.
    const auto streaming = [&](rap::sys::http_options o, const char* what, std::string_view error)
    {
        std::uint64_t taken = 0;
        o.max_bytes = 0;
        o.sink = [&](std::string_view piece)
        {
            taken += piece.size();
            std::this_thread::sleep_for(20ms);
            return true;
        };
        const auto from = std::chrono::steady_clock::now();
        auto r = rap::sys::http_get(s.url("/stream"), o);
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from);
        const bool good = r.error == error && taken < (8u << 20) && ms < 2000ms;
        if (!good)
            std::printf("     error [%s], %llu bytes, took %lld ms\n", r.error.c_str(), (unsigned long long)taken,
                        (long long)ms.count());
        check(good, what);
    };
    std::atomic<bool> stop = false;
    rap::sys::http_options cancelling{.timeout = 10000ms, .cancelled = &stop};
    cancelling.progress = [&](std::uint64_t got, std::optional<std::uint64_t>)
    {
        if (got >= (1u << 20)) stop = true;
    };
    streaming(cancelling, "http: cancelling ends a body that keeps coming", "cancelled");
    streaming({.timeout = 500ms}, "http: the time limit ends a body that keeps coming", "timed out");

    std::filesystem::remove(secret);
}

}  // namespace

int main()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    origins();
    requests();
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
