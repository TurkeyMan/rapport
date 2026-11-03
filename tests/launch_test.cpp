// Starting programs without waiting (common/launch.h), shared by the core
// and the shell extensions: Windows command-line quoting, and on POSIX the
// detached spawn and its wait.
//   rapport-launch-test
#include "launch.h"

#include <cstdio>
#include <string>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <shellapi.h>  // after windows.h, whose types it uses
// clang-format on
#else
#include <unistd.h>
#endif

namespace
{

int fails = 0;

void check(bool ok, const char* what)
{
    if (!ok) ++fails;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

std::wstring quoted(std::wstring_view arg)
{
    std::wstring line;
    rap::launch::append_quoted(line, arg);
    return line;
}

void quoting()
{
    check(quoted(L"plain") == L"plain", "quote: a plain word as it is");
    check(quoted(L"") == L"\"\"", "quote: an empty argument");
    check(quoted(L"a b") == L"\"a b\"", "quote: a space");
    check(quoted(L"a\tb") == L"\"a\tb\"" && quoted(L"a\nb") == L"\"a\nb\"" &&
              quoted(L"a\vb") == L"\"a\vb\"",
          "quote: tab, newline and vertical tab");
    check(quoted(L"C:\\dir\\file") == L"C:\\dir\\file", "quote: backslashes alone as they are");
    check(quoted(L"a\"b") == L"\"a\\\"b\"", "quote: a quote escaped");
    check(quoted(L"a\\\"b") == L"\"a\\\\\\\"b\"", "quote: backslashes before a quote double");
    check(quoted(L"C:\\a dir\\") == L"\"C:\\a dir\\\\\"",
          "quote: a trailing backslash doubles before the closing quote");
    check(quoted(L"a\\\\b c") == L"\"a\\\\b c\"", "quote: backslashes inside as they are");
    std::wstring line;
    rap::launch::append_quoted(line, L"prog");
    rap::launch::append_quoted(line, L"x y");
    rap::launch::append_quoted(line, L"");
    check(line == L"prog \"x y\" \"\"", "quote: arguments a space apart");

#ifdef _WIN32
    // What Windows reads back.
    const wchar_t* args[] = {
        L"C:\\Program Files\\x.exe", L"",   L"a\"b",     L"a\\\"b", L"end\\", L"two  spaces\\",
        L"\\\\server\\share\\",      L"\"", L"tab\there"};
    std::wstring all;
    for (const wchar_t* a : args) rap::launch::append_quoted(all, a);
    int n = 0;
    wchar_t** back = CommandLineToArgvW(all.c_str(), &n);
    bool same = back && n == int(std::size(args));
    for (int i = 0; same && i < n; ++i) same = std::wstring_view(back[i]) == args[i];
    LocalFree(back);
    check(same, "quote: CommandLineToArgvW reads the arguments back");
#endif
}

#ifndef _WIN32
void spawning()
{
    check(rap::launch::spawn_detached({"true"}), "spawn: a program found on PATH");
    check(!rap::launch::spawn_detached({"rapport-no-such-program"}), "spawn: a program not found");
    check(!rap::launch::spawn_detached({"true"}, "/rapport-no-such-dir"),
          "spawn: a directory that isn't there");
    check(rap::launch::spawn_detached({"true"}, "/"), "spawn: in a directory");
    // waitpid fails for what isn't a child (ECHILD): not a success.
    check(!rap::launch::exited_ok(int(getpid())), "spawn: a failed wait isn't a success");
}
#endif

}  // namespace

int main()
{
    quoting();
#ifndef _WIN32
    spawning();
#endif
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
