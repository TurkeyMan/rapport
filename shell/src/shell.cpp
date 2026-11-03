// Helpers for providers: where the extension is, and starting its app.
#include <rapport/shell.h>

#include "launch.h"  // common/: shared with the core, compiled in here

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
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
    rap::launch::append_quoted(line, program.wstring());
    for (const std::string& a : args) rap::launch::append_quoted(line, wide(a));
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

bool launch(const std::filesystem::path& program, const std::vector<std::string>& args,
            const std::filesystem::path& working_dir)
{
    std::vector<std::string> argv{program.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    return rap::launch::spawn_detached(argv, working_dir);
}

#endif

}  // namespace rap::shell
