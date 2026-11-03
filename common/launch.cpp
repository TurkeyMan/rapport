#include "launch.h"

#ifndef _WIN32
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace rap::launch
{

void append_quoted(std::wstring& line, std::wstring_view arg)
{
    if (!line.empty()) line += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
    {
        line += arg;
        return;
    }
    line += L'"';
    size_t slashes = 0;
    for (wchar_t c : arg)
    {
        if (c == L'\\')
        {
            ++slashes;
            continue;
        }
        // Backslashes before a quote double, and the quote is escaped;
        // elsewhere they're as they are.
        line.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        line += c;
    }
    line.append(slashes * 2, L'\\');  // ...as they do before the closing quote
    line += L'"';
}

#ifndef _WIN32

// Without fork(), which is unsafe with other threads running (only
// async-signal-safe calls may follow it). The arguments stay argv, so
// nothing needs quoting; the directory goes through the environment.
bool spawn_detached(const std::vector<std::string>& argv, const std::filesystem::path& dir)
{
    if (argv.empty()) return false;
    std::string script = "command -v \"$0\" >/dev/null 2>&1 || exit 127\n"
                         "exec </dev/null >/dev/null 2>&1\n"
                         "if command -v setsid >/dev/null; then setsid \"$0\" \"$@\" & "
                         "else \"$0\" \"$@\" & fi";
    if (!dir.empty()) script = "cd \"$RAPPORT_LAUNCH_DIR\" 2>/dev/null || exit 1\n" + script;
    std::vector<std::string> owned{"/bin/sh", "-c", std::move(script)};
    owned.insert(owned.end(), argv.begin(), argv.end());
    std::vector<char*> args;
    for (std::string& a : owned) args.push_back(a.data());
    args.push_back(nullptr);

    std::string dir_var = "RAPPORT_LAUNCH_DIR=" + dir.string();
    std::vector<char*> env;
    for (char** e = environ; *e; ++e)
        if (!std::string_view(*e).starts_with("RAPPORT_LAUNCH_DIR=")) env.push_back(*e);
    if (!dir.empty()) env.push_back(dir_var.data());
    env.push_back(nullptr);

    pid_t sh;
    if (posix_spawn(&sh, "/bin/sh", nullptr, nullptr, args.data(), env.data()) != 0) return false;
    return exited_ok(sh);
}

bool exited_ok(int pid)
{
    int status = 0;
    pid_t got;
    while ((got = waitpid(pid_t(pid), &status, 0)) < 0 && errno == EINTR)
    {
    }
    // A failed wait leaves status as it was: no exit known.
    return got == pid_t(pid) && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#endif

}  // namespace rap::launch
