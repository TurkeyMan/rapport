#include "anyui/sys.h"

#include <cstdlib>
#include <cstring>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace anyui::sys
{

// ---------------------------------------------------------------- strings, paths

fs::path pathFromUtf8(std::string_view utf8)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string pathToUtf8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::optional<std::string> env(const char* name)
{
#ifdef _WIN32
    const std::wstring wname = pathFromUtf8(name).wstring();
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring value(n, L'\0');
    value.resize(GetEnvironmentVariableW(wname.c_str(), value.data(), n));
    return pathToUtf8(fs::path(value));
#else
    if (const char* v = std::getenv(name)) return std::string(v);
    return std::nullopt;
#endif
}

// ---------------------------------------------------------------- files

std::FILE* fopen(const fs::path& path, const char* mode)
{
#ifdef _WIN32
    const std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(path.c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// ---------------------------------------------------------------- locations

fs::path executablePath()
{
#ifdef _WIN32
    std::wstring buf(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n < buf.size())
        {
            buf.resize(n);
            return fs::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p;
#endif
}

namespace
{

fs::path ensure(fs::path p)
{
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

#ifdef _WIN32
fs::path knownFolder(const KNOWNFOLDERID& id)
{
    PWSTR raw = nullptr;
    fs::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) p = raw;
    CoTaskMemFree(raw);
    return p;
}
#else
fs::path xdg(const char* var, const char* fallback)
{
    if (auto v = env(var); v && !v->empty() && v->front() == '/') return fs::path(*v);
    return fs::path(env("HOME").value_or("/tmp")) / fallback;
}
#endif

}  // namespace

fs::path configDir(std::string_view app)
{
#ifdef _WIN32
    return ensure(knownFolder(FOLDERID_RoamingAppData) / pathFromUtf8(app));
#else
    return ensure(xdg("XDG_CONFIG_HOME", ".config") / pathFromUtf8(app));
#endif
}

fs::path cacheDir(std::string_view app)
{
#ifdef _WIN32
    return ensure(knownFolder(FOLDERID_LocalAppData) / pathFromUtf8(app) / "cache");
#else
    return ensure(xdg("XDG_CACHE_HOME", ".cache") / pathFromUtf8(app));
#endif
}

fs::path dataDir(std::string_view app)
{
#ifdef _WIN32
    return ensure(knownFolder(FOLDERID_LocalAppData) / pathFromUtf8(app));
#else
    return ensure(xdg("XDG_DATA_HOME", ".local/share") / pathFromUtf8(app));
#endif
}

// ---------------------------------------------------------------- shared libraries

std::expected<Library, std::string> Library::open(const fs::path& name)
{
#ifdef _WIN32
    // A bare name searches the application directory and System32 (not the
    // current directory); a path loads exactly that file and lets its
    // dependencies resolve beside it.
    const DWORD flags = name.has_parent_path()
                            ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
                            : LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    if (HMODULE h = LoadLibraryExW(name.c_str(), nullptr, flags))
        return Library(reinterpret_cast<void*>(h));
    return std::unexpected(pathToUtf8(name) + ": " +
                           std::system_category().message(int(GetLastError())));
#else
    if (void* h = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL)) return Library(h);
    const char* err = dlerror();
    return std::unexpected(std::string(err ? err : "dlopen failed"));
#endif
}

Library& Library::operator=(Library&& other) noexcept
{
    if (this != &other)
    {
        this->~Library();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

Library::~Library()
{
    if (!handle_) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
}

void* Library::symbol(const char* name) const
{
    if (!handle_) return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
}

namespace
{

// Where the loader looks, in its order.
std::vector<fs::path> librarySearchPath()
{
    std::vector<fs::path> dirs;
    dirs.push_back(executablePath().parent_path());
#ifdef _WIN32
    wchar_t sys[MAX_PATH];
    if (UINT n = GetSystemDirectoryW(sys, MAX_PATH); n && n < MAX_PATH)
        dirs.emplace_back(std::wstring(sys, n));
#else
    // The loader's own list for this process: LD_LIBRARY_PATH, the
    // executable's RPATH/RUNPATH, the system defaults.
    if (void* self = dlopen(nullptr, RTLD_LAZY))
    {
        Dl_serinfo size;
        if (dlinfo(self, RTLD_DI_SERINFOSIZE, &size) == 0)
        {
            std::vector<char> buf(size.dls_size);
            auto* info = reinterpret_cast<Dl_serinfo*>(buf.data());
            *info = size;
            if (dlinfo(self, RTLD_DI_SERINFO, info) == 0)
                for (unsigned i = 0; i < info->dls_cnt; ++i)
                    dirs.emplace_back(info->dls_serpath[i].dls_name);
        }
        dlclose(self);
    }
    // Directories reached only through ld.so.cache.
#if defined(__x86_64__)
    const char* triplet = "x86_64-linux-gnu";
#elif defined(__aarch64__)
    const char* triplet = "aarch64-linux-gnu";
#else
    const char* triplet = nullptr;
#endif
    if (triplet)
        for (const char* base : {"/usr/lib", "/lib"}) dirs.push_back(fs::path(base) / triplet);
    for (const char* d : {"/usr/lib64", "/lib64", "/usr/lib", "/lib", "/usr/local/lib"})
        dirs.emplace_back(d);
#endif
    return dirs;
}

}  // namespace

bool libraryInstalled(std::string_view name)
{
    static const std::vector<fs::path> dirs = librarySearchPath();
    const fs::path file = pathFromUtf8(name);
    std::error_code ec;
    if (file.has_parent_path()) return fs::exists(file, ec);
    for (const fs::path& d : dirs)
        if (fs::exists(d / file, ec)) return true;
    return false;
}

// ---------------------------------------------------------------- threads, memory

void lowerThreadPriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 10);
#endif
}

std::size_t availableMemory()
{
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    return GlobalMemoryStatusEx(&status) ? std::size_t(status.ullAvailPhys) : 0;
#else
    std::size_t available = 0;
    if (std::FILE* f = std::fopen("/proc/meminfo", "r"))
    {
        char line[256];
        while (std::fgets(line, sizeof line, f))
        {
            unsigned long long kb = 0;
            if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1)
            {
                available = std::size_t(kb) * 1024;
                break;
            }
        }
        std::fclose(f);
    }
    return available;
#endif
}

}  // namespace anyui::sys
