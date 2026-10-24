// Helpers for providers: where the extension is, and starting its app.
#include <rapport/shell.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace rap::shell
{

#ifdef _WIN32

namespace
{

std::wstring wide(std::string_view s)
{
    if (s.empty()) return {};
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

// One argument as CommandLineToArgvW reads it back.
void append_quoted(std::wstring& line, const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        line += arg;
        return;
    }
    line += L'"';
    for (auto it = arg.begin();; ++it)
    {
        size_t slashes = 0;
        while (it != arg.end() && *it == L'\\')
        {
            ++it;
            ++slashes;
        }
        if (it == arg.end())
        {
            line.append(slashes * 2, L'\\');
            break;
        }
        if (*it == L'"')
            line.append(slashes * 2 + 1, L'\\');
        else
            line.append(slashes, L'\\');
        line += *it;
    }
    line += L'"';
}

}  // namespace

std::filesystem::path module_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_dir), &self);
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        DWORD n = GetModuleFileNameW(self, path.data(), DWORD(path.size()));
        if (n == 0) return {};
        if (n < path.size())
        {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path();
}

bool launch(const std::filesystem::path& program, const std::vector<std::string>& args,
            const std::filesystem::path& working_dir)
{
    std::wstring line;
    append_quoted(line, program.wstring());
    for (const std::string& a : args)
    {
        line += L' ';
        append_quoted(line, wide(a));
    }
    STARTUPINFOW si{.cb = sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_DEFAULT_ERROR_MODE, nullptr,
                        working_dir.empty() ? nullptr : working_dir.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

#else

std::filesystem::path module_dir()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&module_dir), &info) || !info.dli_fname) return {};
    std::error_code ec;
    std::filesystem::path p = std::filesystem::canonical(info.dli_fname, ec);
    return (ec ? std::filesystem::path(info.dli_fname) : p).parent_path();
}

namespace
{

// Starts a program and doesn't wait for it, without fork() (unsafe with
// other threads running: only async-signal-safe calls may follow it). A
// shell is spawned to background the program and exits at once, so the
// program is reparented to init and nothing is left to reap; the arguments
// stay argv, so nothing needs quoting. A program not found fails (127).
bool spawn_detached(const std::vector<std::string>& argv, const std::filesystem::path& dir)
{
    std::vector<std::string> owned{
        "/bin/sh", "-c",
        "command -v \"$0\" >/dev/null 2>&1 || exit 127\n"
        "exec </dev/null >/dev/null 2>&1\n"
        "if command -v setsid >/dev/null; then setsid \"$0\" \"$@\" & else \"$0\" \"$@\" & fi"};
    owned.insert(owned.end(), argv.begin(), argv.end());
    std::vector<char*> args;
    for (std::string& a : owned) args.push_back(a.data());
    args.push_back(nullptr);
    pid_t sh;
    // The working directory through the shell too: cd before it runs.
    if (!dir.empty()) owned[2] = "cd \"$RAPPORT_LAUNCH_DIR\" || exit 1\n" + owned[2];
    args.clear();
    for (std::string& a : owned) args.push_back(a.data());
    args.push_back(nullptr);
    std::vector<std::string> env_owned;
    std::vector<char*> env;
    for (char** e = environ; *e; ++e) env.push_back(*e);
    if (!dir.empty())
    {
        env_owned.push_back("RAPPORT_LAUNCH_DIR=" + dir.string());
        env.push_back(env_owned.back().data());
    }
    env.push_back(nullptr);
    if (posix_spawn(&sh, "/bin/sh", nullptr, nullptr, args.data(), env.data()) != 0) return false;
    int status = 0;
    while (waitpid(sh, &status, 0) < 0 && errno == EINTR)
    {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

bool launch(const std::filesystem::path& program, const std::vector<std::string>& args,
            const std::filesystem::path& working_dir)
{
    std::vector<std::string> argv{program.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    return spawn_detached(argv, working_dir);
}

#endif

}  // namespace rap::shell
