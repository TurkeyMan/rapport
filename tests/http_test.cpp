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
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
    explicit server(std::function<std::string(std::string_view path, std::string_view headers)> answer)
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
                    answer_(std::string_view(request).substr(from, to - from),
                            std::string_view(request).substr(request.find("\r\n")));
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

    std::function<std::string(std::string_view, std::string_view)> answer_;
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

    constexpr std::string_view nested = "/login?next=https://example.org/";
    const auto received = [&](std::string_view path, std::string_view headers)
    {
        if (path != nested) return response("404 Not Found", {}, "");
        const bool auth = headers.find("\r\nAuthorization: Bearer test\r\n") != std::string_view::npos;
        return response("200 OK", {}, auth ? "with credentials" : "without credentials");
    };
    server other(received);
    server s(
        [&](std::string_view path, std::string_view headers) -> std::string
        {
            if (path == "/same-origin") return response("302 Found", nested, "");
            if (path == "/other-origin") return response("302 Found", other.url(nested), "");
            if (path == nested) return received(path, headers);
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
    if (!s || !other)
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

    const rap::sys::http_options auth{.headers = {{"Authorization", "Bearer test"}}};
    const auto same = rap::sys::http_get(s.url("/same-origin"), auth);
    check(same.status == 200 && same.body == "with credentials" && same.url == s.url(nested),
          "redirect: a URL in a relative target's query stays on the original origin");
    const auto across = rap::sys::http_get(s.url("/other-origin"), auth);
    check(across.status == 200 && across.body == "without credentials" && across.url == other.url(nested),
          "redirect: a different port receives the target without credentials");

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

// http_options::on_redirect: asked about each hop, in order; stop keeps the
// 3xx; follow_without_credentials keeps them off that hop and every later
// one, back at the first origin too; follow can't send them across origins;
// unset, as ever; an exception fails the request, the hop not sent.
void redirect_decisions()
{
    std::mutex m;
    std::map<std::string, std::string> seen;  // path -> the credentials it came with
    const auto record = [&](std::string_view path, std::string_view headers)
    {
        std::string had;
        for (const char* h : {"Authorization", "Cookie", "Proxy-Authorization"})
            if (headers.find("\r\n" + std::string(h) + ": ") != std::string_view::npos) had += std::string(had.empty() ? "" : " ") + h;
        std::lock_guard lock(m);
        seen[std::string(path)] = had.empty() ? "none" : had;
    };
    const auto had = [&](const std::string& path)
    {
        std::lock_guard lock(m);
        const auto it = seen.find(path);
        return it == seen.end() ? std::string("not asked") : it->second;
    };
    const auto forget = [&]
    {
        std::lock_guard lock(m);
        seen.clear();
    };
    server other(
        [&](std::string_view path, std::string_view headers)
        {
            record(path, headers);
            return response("200 OK", {}, "other");
        });
    server s(
        [&](std::string_view path, std::string_view headers) -> std::string
        {
            record(path, headers);
            // /hop1 -> /hop2 -> /final, 302 then 307.
            if (path == "/hop1") return response("302 Found", "/hop2", "");
            if (path == "/hop2") return response("307 Temporary Redirect", "/final", "");
            if (path == "/final") return response("200 OK", {}, "final");
            // A tenant's URL to another's, on the same host, and back.
            if (path == "/orgA/start") return response("302 Found", "/orgB/next", "");
            if (path == "/orgB/next") return response("302 Found", "/orgA/end", "");
            if (path == "/orgA/end") return response("200 OK", {}, "end");
            if (path == "/stop") return response("302 Found", "/final", "moved along");
            if (path == "/across") return response("302 Found", other.url("/landing"), "");
            return response("404 Not Found", {}, "");
        });
    if (!s || !other)
    {
        check(false, "on_redirect: servers on the loopback");
        return;
    }
    if (auto probe = rap::sys::http_get(s.url("/final")); probe.error.find("libcurl") != std::string::npos)
    {
        std::printf("skip on_redirect: %s\n", probe.error.c_str());
        return;
    }
    const rap::sys::http_headers credentials{
        {"Authorization", "Bearer test"}, {"Cookie", "session=1"}, {"Proxy-Authorization", "Basic cHJveHk="}};
    using rap::sys::redirect_action;

    // Each hop, in order: from, to (absolute), status.
    {
        std::vector<std::string> calls;
        rap::sys::http_options o;
        o.on_redirect = [&](std::string_view from, std::string_view to, int status)
        {
            calls.push_back(std::string(from) + " -> " + std::string(to) + " " + std::to_string(status));
            return redirect_action::follow;
        };
        const auto r = rap::sys::http_get(s.url("/hop1"), o);
        const std::vector<std::string> want{s.url("/hop1") + " -> " + s.url("/hop2") + " 302",
                                            s.url("/hop2") + " -> " + s.url("/final") + " 307"};
        if (calls != want)
            for (const auto& c : calls) std::printf("     called: %s\n", c.c_str());
        check(calls == want && r.status == 200 && r.body == "final",
              "on_redirect: asked about each hop in order, with from, to and status");
    }

    // stop: the 3xx is the response, Location and body, no error; the hop
    // not requested.
    {
        forget();
        rap::sys::http_options o;
        o.on_redirect = [](std::string_view, std::string_view, int) { return redirect_action::stop; };
        const auto r = rap::sys::http_get(s.url("/stop"), o);
        const std::string* where = nullptr;
        for (const auto& [n, v] : r.headers)
            if (n == "Location") where = &v;
        if (r.status != 302)
            std::printf("     status %d, error [%s], body [%s]\n", r.status, r.error.c_str(), r.body.c_str());
        check(r.status == 302 && r.error.empty() && where && *where == "/final" && r.body == "moved along" &&
                  r.url == s.url("/stop") && had("/final") == "not asked",
              "on_redirect: stop returns the 3xx with its Location and body, not followed");
    }

    // follow_without_credentials to another tenant: none there, nor back at
    // the first one after it.
    {
        forget();
        rap::sys::http_options o{.headers = credentials};
        o.on_redirect = [](std::string_view, std::string_view to, int)
        { return to.find("/orgB/") != std::string_view::npos ? redirect_action::follow_without_credentials
                                                              : redirect_action::follow; };
        const auto r = rap::sys::http_get(s.url("/orgA/start"), o);
        std::printf("     orgA/start %s, orgB/next %s, orgA/end %s\n", had("/orgA/start").c_str(),
                    had("/orgB/next").c_str(), had("/orgA/end").c_str());
        check(r.status == 200 && had("/orgA/start") == "Authorization Cookie Proxy-Authorization" &&
                  had("/orgB/next") == "none",
              "on_redirect: follow_without_credentials sends none on that hop (Authorization, Cookie, "
              "Proxy-Authorization)");
        check(had("/orgA/end") == "none", "on_redirect: nor on a later hop back to the first origin");
    }

    // follow on a hop to another origin: still none there.
    {
        forget();
        rap::sys::http_options o{.headers = credentials};
        o.on_redirect = [](std::string_view, std::string_view, int) { return redirect_action::follow; };
        const auto r = rap::sys::http_get(s.url("/across"), o);
        // (Proxy-Authorization is the proxy's, which the origin rule leaves
        // as ever: only follow_without_credentials drops it.)
        const std::string there = had("/landing");
        if (there != "Proxy-Authorization") std::printf("     /landing had: %s\n", there.c_str());
        check(r.status == 200 && r.body == "other" && there == "Proxy-Authorization",
              "on_redirect: follow on a hop to another origin still sends no Authorization or Cookie");
    }

    // Unset: as ever (same origin, credentials kept).
    {
        forget();
        const auto r = rap::sys::http_get(s.url("/orgA/start"), {.headers = credentials});
        check(r.status == 200 && had("/orgB/next") == "Authorization Cookie Proxy-Authorization" &&
                  had("/orgA/end") == "Authorization Cookie Proxy-Authorization",
              "on_redirect unset: as before, credentials on same-origin hops");
    }

    // An exception: the request fails, the hop not sent.
    {
        forget();
        rap::sys::http_options o{.headers = credentials};
        o.on_redirect = [](std::string_view, std::string_view, int) -> redirect_action
        { throw std::runtime_error("no"); };
        const auto r = rap::sys::http_get(s.url("/orgA/start"), o);
        if (r.error.empty()) std::printf("     status %d, body [%s]\n", r.status, r.body.c_str());
        check(!r.error.empty() && r.status == 0 && had("/orgB/next") == "not asked",
              ("on_redirect: an exception fails the request, the hop not sent (" + r.error + ")").c_str());
    }
}

// on_redirect's to_url is the URL the hop is sent to, on either transport:
// dot segments out ("/orgA/../orgB/secret" is "/orgB/secret", which is
// what the server is asked for), so a policy on its path sees the path that
// goes; a Location of a fragment alone, or empty, is the same path and
// query again (not the directory); a query alone, the path with it. The
// first URL's dot segments out too, so from_url and an inherited path are
// what's sent. And a hop without credentials goes without the URL's
// userinfo, which libcurl would send as Basic credentials.
void redirect_targets()
{
    std::mutex m;
    std::map<std::string, std::string> seen;  // path -> the credentials it came with
    std::string self;                         // the server's own URL, for an absolute Location
    std::string elsewhere;                    // another origin's, with userinfo
    const auto had = [&](const std::string& path)
    {
        std::lock_guard lock(m);
        const auto it = seen.find(path);
        return it == seen.end() ? std::string("not asked") : it->second;
    };
    server s(
        [&](std::string_view path, std::string_view headers) -> std::string
        {
            {
                std::lock_guard lock(m);
                seen[std::string(path)] =
                    headers.find("\r\nAuthorization: ") != std::string_view::npos ? "Authorization"
                                                                                  : "none";
            }
            if (path == "/orgA/dots") return response("302 Found", "/orgA/../orgB/secret", "");
            if (path == "/orgA/rel/dots") return response("302 Found", "../../orgB/./secret", "");
            if (path == "/orgA/abs")
            {
                std::lock_guard lock(m);
                return response("302 Found", self + "/orgA/../orgB/secret", "");
            }
            if (path == "/orgB/secret") return response("200 OK", {}, "secret");
            if (path == "/page/here?q=1") return response("302 Found", "#top", "");
            if (path == "/page/empty?q=2")
                return "HTTP/1.1 302 Found\r\nConnection: close\r\nLocation: \r\nContent-Length: "
                       "0\r\n\r\n";
            if (path == "/page/query?q=3") return response("302 Found", "?r=4", "");
            if (path == "/cred/start") return response("302 Found", "/cred/next", "");
            if (path == "/cred/next") return response("200 OK", {}, "next");
            if (path == "/cred/across")
            {
                std::lock_guard lock(m);
                return response("302 Found", elsewhere, "");
            }
            return response("404 Not Found", {}, "");
        });
    server other(
        [&](std::string_view path, std::string_view headers)
        {
            std::lock_guard lock(m);
            seen["other" + std::string(path)] =
                headers.find("\r\nAuthorization: ") != std::string_view::npos ? "Authorization"
                                                                              : "none";
            return response("200 OK", {}, "other");
        });
    if (!s)
    {
        check(false, "redirect targets: a server on the loopback");
        return;
    }
    {
        std::lock_guard lock(m);
        self = s.url("");
        elsewhere = other.url("/landing").insert(7, "user:secret@");
    }
    if (auto probe = rap::sys::http_get(s.url("/orgB/secret"));
        probe.error.find("libcurl") != std::string::npos)
    {
        std::printf("skip redirect targets: %s\n", probe.error.c_str());
        return;
    }
    using rap::sys::redirect_action;

    // A policy on the path: credentials only on to /orgA/.
    for (const char* from : {"/orgA/dots", "/orgA/rel/dots", "/orgA/abs"})
    {
        {
            std::lock_guard lock(m);
            seen.clear();
        }
        std::vector<std::string> told;
        rap::sys::http_options o{.headers = {{"Authorization", "Bearer orgA"}}};
        o.on_redirect = [&](std::string_view, std::string_view to, int)
        {
            told.emplace_back(to);
            return to.find("/orgA/") != std::string_view::npos
                       ? redirect_action::follow
                       : redirect_action::follow_without_credentials;
        };
        const auto r = rap::sys::http_get(s.url(from), o);
        const std::string to = told.empty() ? "(not asked)" : told.front();
        check(told.size() == 1 && to == s.url("/orgB/secret") && r.status == 200 &&
                  r.body == "secret",
              (std::string("redirect targets: ") + from +
               "'s dot segments out, the URL that's sent: " + to)
                  .c_str());
        check(had("/orgB/secret") == "none" && r.url == s.url("/orgB/secret"),
              (std::string("redirect targets: ") + from +
               ": a policy on the path keeps credentials off /orgB/, " + had("/orgB/secret"))
                  .c_str());
    }

    // Asked only: what each Location resolves to, stopped there; and where
    // from, the URL that answered.
    std::string answered;
    const auto target = [&](const std::string& from)
    {
        std::string to = "(not asked)";
        answered = "(not asked)";
        rap::sys::http_options o;
        o.on_redirect = [&](std::string_view f, std::string_view t, int)
        {
            answered = std::string(f), to = std::string(t);
            return redirect_action::stop;
        };
        rap::sys::http_get(s.url(from), o);
        return to;
    };
    const struct
    {
        const char* from;
        std::string want;
        const char* what;
    } cases[] = {
        {"/page/here?q=1", s.url("/page/here?q=1"), "a fragment alone: the same path and query"},
        {"/page/empty?q=2", s.url("/page/empty?q=2"), "an empty Location: the same path and query"},
        {"/page/query?q=3", s.url("/page/query?r=4"), "a query alone: the same path with it"},
        // The first URL's dot segments out, so the path these inherit is the
        // one sent, not /orgA/...
        {"/orgA/../page/here?q=1", s.url("/page/here?q=1"),
         "a fragment alone after a first URL with dots: the path sent"},
        {"/orgA/../page/empty?q=2", s.url("/page/empty?q=2"),
         "an empty Location after a first URL with dots: the path sent"},
        {"/orgA/./../page/query?q=3", s.url("/page/query?r=4"),
         "a query alone after a first URL with dots: the path sent"},
    };
    for (const auto& c : cases)
    {
        const std::string to = target(c.from);
        check(to == c.want, (std::string("redirect targets: ") + c.what + ", " + to).c_str());
        // The first URL as sent: from its /page/ on.
        const std::string sent =
            s.url(std::string(c.from).substr(std::string(c.from).find("/page/")));
        check(answered == sent, (std::string("redirect targets: ") + c.what +
                                 ", where from is the URL sent: " + answered)
                                    .c_str());
    }

    // Userinfo in the URL: off a hop without credentials (on libcurl it'd
    // be Basic credentials), both a relative one at the same origin that
    // on_redirect takes credentials off, and one to another origin.
    if (!other)
    {
        check(false, "redirect targets: a second server on the loopback");
        return;
    }
    {
        {
            std::lock_guard lock(m);
            seen.clear();
        }
        rap::sys::http_options o;
        o.on_redirect = [](std::string_view, std::string_view, int)
        { return redirect_action::follow_without_credentials; };
        const auto r = rap::sys::http_get(s.url("/cred/start").insert(7, "user:secret@"), o);
        check(r.status == 200 && r.body == "next" && had("/cred/next") == "none" &&
                  r.url == s.url("/cred/next"),
              ("redirect targets: follow_without_credentials drops the URL's userinfo, " +
               had("/cred/next") + " to " + r.url)
                  .c_str());
    }
    {
        {
            std::lock_guard lock(m);
            seen.clear();
        }
        std::string to = "(not asked)";
        rap::sys::http_options o;
        o.on_redirect = [&](std::string_view, std::string_view t, int)
        {
            to = std::string(t);
            return redirect_action::follow;
        };
        const auto r = rap::sys::http_get(s.url("/cred/across"), o);
        check(r.status == 200 && r.body == "other" && had("other/landing") == "none" &&
                  to == other.url("/landing") && r.url == other.url("/landing"),
              ("redirect targets: to another origin, its userinfo dropped, " +
               had("other/landing") + ", told " + to)
                  .c_str());
    }
}

#ifndef _WIN32
// Through a proxy with credentials of its own, from the environment
// (http_proxy's user:pw, which libcurl sends as Proxy-Authorization): the
// machine's way to the network, not the request's. They go to the proxy on
// every hop, follow_without_credentials or not (else the next hop is a 407),
// and no origin's ever sent them (a proxy keeps Proxy-Authorization). What
// follow_without_credentials drops is the caller's: Authorization, and a
// Proxy-Authorization header given in the options (sent when the
// configuration has no proxy credentials; libcurl sends those in its
// place). Two origins that resolve nowhere (.invalid), so a hop that went
// around the proxy fails.
void proxied_redirects()
{
    struct seen_hop
    {
        std::string url, proxy_auth, auth;
    };
    std::mutex m;
    std::vector<seen_hop> seen;
    const auto value = [](std::string_view headers, std::string_view name)
    {
        const std::string key = "\r\n" + std::string(name) + ": ";
        const size_t at = headers.find(key);
        if (at == std::string_view::npos) return std::string("none");
        const size_t from = at + key.size();
        return std::string(headers.substr(from, headers.find("\r\n", from) - from));
    };
    // What reaches the proxy; an origin is sent all of it but
    // Proxy-Authorization, which is the proxy's.
    server proxy(
        [&](std::string_view url, std::string_view headers)
        {
            {
                std::lock_guard lock(m);
                seen.push_back({std::string(url), value(headers, "Proxy-Authorization"),
                                value(headers, "Authorization")});
            }
            if (url == "http://a.invalid/start")
                return response("302 Found", "http://b.invalid/end", "");
            if (url == "http://a.invalid/here") return response("302 Found", "/there", "");
            if (url == "http://b.invalid/end" || url == "http://a.invalid/there")
                return response("200 OK", {}, "through the proxy");
            return response("404 Not Found", {}, "");
        });
    if (!proxy)
    {
        check(false, "proxied redirects: a proxy on the loopback");
        return;
    }
    // This process's environment only, put back after.
    const auto saved = [](const char* name)
    {
        const char* v = std::getenv(name);
        return v ? std::optional<std::string>(v) : std::nullopt;
    };
    const auto put_back = [](const char* name, const std::optional<std::string>& v)
    {
        if (v)
            setenv(name, v->c_str(), 1);
        else
            unsetenv(name);
    };
    const auto http_proxy = saved("http_proxy"), no_proxy = saved("no_proxy"),
               no_proxy_upper = saved("NO_PROXY");
    setenv("http_proxy", proxy.url("").insert(7, "proxyuser:proxypw@").c_str(), 1);
    unsetenv("no_proxy");
    unsetenv("NO_PROXY");

    const std::string env_auth = "Basic cHJveHl1c2VyOnByb3h5cHc=";  // proxyuser:proxypw
    using rap::sys::redirect_action;
    const auto run = [&](const std::string& from, redirect_action act, bool caller_proxy_auth)
    {
        {
            std::lock_guard lock(m);
            seen.clear();
        }
        rap::sys::http_options o{.headers = {{"Authorization", "Bearer origin"}}};
        if (caller_proxy_auth) o.headers.emplace_back("Proxy-Authorization", "Bearer caller");
        o.on_redirect = [act](std::string_view, std::string_view, int) { return act; };
        const auto r = rap::sys::http_get(from, o);
        std::lock_guard lock(m);
        std::string hops;
        for (const auto& h : seen)
            hops += " [" + h.url + " proxy: " + h.proxy_auth + ", origin: " + h.auth + "]";
        return std::pair(r.status == 200 && r.body == "through the proxy" && seen.size() == 2,
                         hops);
    };
    const auto hop = [&](size_t i) -> seen_hop
    {
        std::lock_guard lock(m);
        return i < seen.size() ? seen[i] : seen_hop{"(none)", "(none)", "(none)"};
    };

    if (auto probe = rap::sys::http_get("http://a.invalid/there");
        probe.error.find("libcurl") != std::string::npos)
        std::printf("skip proxied redirects: %s\n", probe.error.c_str());
    else
    {
        // Another origin, followed: the proxy's own on both hops;
        // Authorization only to the first origin.
        auto [ok, hops] = run("http://a.invalid/start", redirect_action::follow, false);
        check(ok && hop(0).proxy_auth == env_auth && hop(1).proxy_auth == env_auth,
              ("proxied redirects: to another origin, the environment's proxy credentials on both "
               "hops," +
               hops)
                  .c_str());
        check(hop(0).auth == "Bearer origin" && hop(1).auth == "none",
              ("proxied redirects: to another origin, Authorization only to the first," + hops)
                  .c_str());

        // follow_without_credentials at the same origin: the proxy's own still
        // go; Authorization doesn't.
        std::tie(ok, hops) =
            run("http://a.invalid/here", redirect_action::follow_without_credentials, false);
        check(ok && hop(0).proxy_auth == env_auth && hop(1).proxy_auth == env_auth,
              ("proxied redirects: follow_without_credentials keeps the environment's proxy "
               "credentials," +
               hops)
                  .c_str());
        check(
            hop(0).auth == "Bearer origin" && hop(1).auth == "none",
            ("proxied redirects: follow_without_credentials drops Authorization," + hops).c_str());

        // The caller's own Proxy-Authorization, to a proxy with no
        // credentials configured (libcurl sends the configuration's in its
        // place when there are some): dropped by follow_without_credentials;
        // followed, it goes on.
        setenv("http_proxy", proxy.url("").c_str(), 1);
        std::tie(ok, hops) =
            run("http://a.invalid/here", redirect_action::follow_without_credentials, true);
        check(ok && hop(0).proxy_auth == "Bearer caller" && hop(1).proxy_auth == "none",
              ("proxied redirects: follow_without_credentials drops the caller's "
               "Proxy-Authorization," +
               hops)
                  .c_str());
        std::tie(ok, hops) = run("http://a.invalid/start", redirect_action::follow, true);
        check(ok && hop(0).proxy_auth == "Bearer caller" && hop(1).proxy_auth == "Bearer caller",
              ("proxied redirects: followed, the caller's Proxy-Authorization goes on," + hops)
                  .c_str());
    }
    put_back("http_proxy", http_proxy);
    put_back("no_proxy", no_proxy);
    put_back("NO_PROXY", no_proxy_upper);
}
#endif

}  // namespace

int main()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    origins();
    requests();
    redirect_decisions();
    redirect_targets();
#ifndef _WIN32
    proxied_redirects();
#endif
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
