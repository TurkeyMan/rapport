// rap::sys's HTTP: the origins credentials are tied to.
//   rapport-http-test
#include "rapport/sys.h"

#include <cstdio>
#include <string>

namespace
{

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

}  // namespace

int main()
{
    origins();
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
