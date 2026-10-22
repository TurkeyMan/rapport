#include "rapport/sys.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace fs = std::filesystem;

namespace rap::sys
{

// ---------------------------------------------------------------- strings, paths

fs::path path_from_utf8(std::string_view utf8)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string path_to_utf8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::optional<std::string> env(const char* name)
{
#ifdef _WIN32
    const std::wstring wname = path_from_utf8(name).wstring();
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring value(n, L'\0');
    value.resize(GetEnvironmentVariableW(wname.c_str(), value.data(), n));
    return path_to_utf8(fs::path(value));
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

fs::path executable_path()
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
fs::path known_folder(const KNOWNFOLDERID& id)
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

fs::path config_dir(std::string_view app)
{
#ifdef _WIN32
    return ensure(known_folder(FOLDERID_RoamingAppData) / path_from_utf8(app));
#else
    return ensure(xdg("XDG_CONFIG_HOME", ".config") / path_from_utf8(app));
#endif
}

fs::path cache_dir(std::string_view app)
{
#ifdef _WIN32
    return ensure(known_folder(FOLDERID_LocalAppData) / path_from_utf8(app) / "cache");
#else
    return ensure(xdg("XDG_CACHE_HOME", ".cache") / path_from_utf8(app));
#endif
}

fs::path data_dir(std::string_view app)
{
#ifdef _WIN32
    return ensure(known_folder(FOLDERID_LocalAppData) / path_from_utf8(app));
#else
    return ensure(xdg("XDG_DATA_HOME", ".local/share") / path_from_utf8(app));
#endif
}

// ---------------------------------------------------------------- shared libraries

std::expected<library, std::string> library::open(const fs::path& name)
{
#ifdef _WIN32
    // A bare name searches the application directory and System32 (not the
    // current directory); a path loads exactly that file and lets its
    // dependencies resolve beside it.
    const DWORD flags = name.has_parent_path()
                            ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
                            : LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    if (HMODULE h = LoadLibraryExW(name.c_str(), nullptr, flags))
        return library(reinterpret_cast<void*>(h));
    return std::unexpected(path_to_utf8(name) + ": " +
                           std::system_category().message(int(GetLastError())));
#else
    if (void* h = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL)) return library(h);
    const char* err = dlerror();
    return std::unexpected(std::string(err ? err : "dlopen failed"));
#endif
}

library& library::operator=(library&& other) noexcept
{
    if (this != &other)
    {
        this->~library();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

library::~library()
{
    if (!handle_) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
}

void* library::symbol(const char* name) const
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
std::vector<fs::path> library_search_path()
{
    std::vector<fs::path> dirs;
    dirs.push_back(executable_path().parent_path());
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

bool library_installed(std::string_view name)
{
    static const std::vector<fs::path> dirs = library_search_path();
    const fs::path file = path_from_utf8(name);
    std::error_code ec;
    if (file.has_parent_path()) return fs::exists(file, ec);
    for (const fs::path& d : dirs)
        if (fs::exists(d / file, ec)) return true;
    return false;
}

// ---------------------------------------------------------------- processes

namespace
{

std::string last_error(const char* what)
{
#ifdef _WIN32
    const int code = int(GetLastError());
#else
    const int code = errno;
#endif
    return std::string(what) + ": " + std::system_category().message(code);
}

}  // namespace

#ifdef _WIN32

struct process::impl
{
    HANDLE process = nullptr;  // valid until destruction, so kill() is always safe
    HANDLE job = nullptr;      // the child and everything it starts; null if unavailable
    HANDLE out = nullptr;      // read end of the merged stdout/stderr pipe
    std::optional<int> code;
};

namespace
{

std::wstring widen(std::string_view s)
{
    return path_from_utf8(s).wstring();
}

// Quotes one argument the way the MSVC runtime parses a command line.
void append_arg(std::wstring& cmd, const std::wstring& a)
{
    if (!cmd.empty()) cmd += L' ';
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
    {
        cmd += a;
        return;
    }
    cmd += L'"';
    std::size_t slashes = 0;
    for (wchar_t c : a)
    {
        if (c == L'\\')
        {
            ++slashes;
            continue;
        }
        // Backslashes before a quote double, and the quote is escaped.
        cmd.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        cmd += c;
    }
    cmd.append(slashes * 2, L'\\');  // ...as they do before the closing quote
    cmd += L'"';
}

// The caller's environment with `overrides` applied, as a CreateProcess block.
std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& overrides)
{
    std::vector<std::wstring> names;
    for (auto& [name, value] : overrides) names.push_back(widen(name) + L'=');
    std::wstring block;
    if (wchar_t* env = GetEnvironmentStringsW())
    {
        for (wchar_t* e = env; *e; e += wcslen(e) + 1)
        {
            bool replaced = false;
            for (auto& n : names) replaced |= _wcsnicmp(e, n.c_str(), n.size()) == 0;
            if (!replaced) block.append(e, wcslen(e) + 1);
        }
        FreeEnvironmentStringsW(env);
    }
    for (auto& [name, value] : overrides)
    {
        block += widen(name) + L'=' + widen(value);
        block += L'\0';
    }
    block += L'\0';
    return block;
}

}  // namespace

std::expected<std::unique_ptr<process>, std::string>
process::start(std::string_view program, std::span<const std::string> args, const options& opt)
{
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) return std::unexpected(last_error("pipe"));
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE null =
        CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &inherit, OPEN_EXISTING, 0, nullptr);

    std::wstring cmd;
    append_arg(cmd, widen(program));
    for (auto& a : args) append_arg(cmd, widen(a));
    std::wstring env = environment_block(opt.env);

    // The child inherits only these handles: with plain inheritance, two
    // processes started at once would hold each other's pipe ends open and
    // neither reader would see the end of its output.
    HANDLE handles[2] = {write, null};
    const DWORD handle_count = null != INVALID_HANDLE_VALUE ? 2 : 1;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::byte> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    const bool listed =
        InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                  handle_count * sizeof(HANDLE), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = null;
    si.StartupInfo.hStdOutput = write;
    si.StartupInfo.hStdError = opt.merge_stderr || null == INVALID_HANDLE_VALUE ? write : null;
    si.lpAttributeList = listed ? attrs : nullptr;
    PROCESS_INFORMATION pi{};
    // Suspended until it is in the job, so nothing it starts escapes. With
    // no application name, CreateProcess finds the first word on PATH.
    const BOOL ok = CreateProcessW(
        nullptr, cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
            (listed ? EXTENDED_STARTUPINFO_PRESENT : 0),
        env.data(), opt.dir.empty() ? nullptr : opt.dir.c_str(), &si.StartupInfo, &pi);
    std::string error = ok ? std::string() : last_error(std::string(program).c_str());
    if (listed) DeleteProcThreadAttributeList(attrs);
    CloseHandle(write);
    if (null != INVALID_HANDLE_VALUE) CloseHandle(null);
    if (!ok)
    {
        CloseHandle(read);
        return std::unexpected(error);
    }

    // Killing the job kills git's own children too (TerminateProcess alone
    // leaves them running), and closing it kills whatever is left.
    auto impl = std::make_unique<process::impl>();
    impl->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (impl->job && !(SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation,
                                               &limits, sizeof limits) &&
                       AssignProcessToJobObject(impl->job, pi.hProcess)))
    {
        CloseHandle(impl->job);
        impl->job = nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    impl->process = pi.hProcess;
    impl->out = read;
    return std::unique_ptr<process>(new process(std::move(impl)));
}

std::size_t process::read(char* buf, std::size_t size)
{
    DWORD n = 0;
    return ReadFile(impl_->out, buf, DWORD(std::min<std::size_t>(size, MAXDWORD)), &n, nullptr) ? n
                                                                                                : 0;
}

int process::wait()
{
    if (!impl_->code)
    {
        WaitForSingleObject(impl_->process, INFINITE);
        DWORD code = DWORD(-1);
        GetExitCodeProcess(impl_->process, &code);
        impl_->code = int(code);
    }
    return *impl_->code;
}

void process::kill()
{
    // Both fail harmlessly once everything has exited.
    if (impl_->job)
        TerminateJobObject(impl_->job, 1);
    else
        TerminateProcess(impl_->process, 1);
}

process::~process()
{
    if (!impl_->code)
    {
        kill();
        wait();
    }
    CloseHandle(impl_->out);
    CloseHandle(impl_->process);
    if (impl_->job) CloseHandle(impl_->job);
}

#else

struct process::impl
{
    pid_t pid = -1;
    int out = -1;  // read end of the merged stdout/stderr pipe
    // Guards `reaped`: once waitpid reaps the child its pid may be reused,
    // so kill() must never signal it after that.
    std::mutex mutex;
    bool reaped = false;
    int code = -1;

    // The child leads its own process group, so this reaches whatever it
    // started too (git's helpers, hooks, ssh).
    void signal(int sig)
    {
        std::lock_guard lock(mutex);
        if (!reaped) ::kill(-pid, sig);
    }
};

std::expected<std::unique_ptr<process>, std::string>
process::start(std::string_view program, std::span<const std::string> args, const options& opt)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return std::unexpected(last_error("pipe"));

    std::vector<std::string> argv{std::string(program)};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(a.data());
    cargv.push_back(nullptr);

    std::vector<std::string> env;
    for (char** e = environ; *e; ++e)
    {
        std::string_view entry(*e);
        bool replaced = false;
        for (auto& [name, value] : opt.env)
            replaced |=
                entry.size() > name.size() && entry.starts_with(name) && entry[name.size()] == '=';
        if (!replaced) env.emplace_back(entry);
    }
    for (auto& [name, value] : opt.env) env.push_back(name + '=' + value);
    std::vector<char*> cenv;
    for (auto& e : env) cenv.push_back(e.data());
    cenv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    if (opt.merge_stderr)
        posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    else
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (!opt.dir.empty()) posix_spawn_file_actions_addchdir_np(&fa, opt.dir.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, cargv[0], &fa, &attr, cargv.data(), cenv.data());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0)
    {
        close(fds[0]);
        return std::unexpected(argv[0] + ": " + std::system_category().message(rc));
    }
    auto impl = std::make_unique<process::impl>();
    impl->pid = pid;
    impl->out = fds[0];
    return std::unique_ptr<process>(new process(std::move(impl)));
}

std::size_t process::read(char* buf, std::size_t size)
{
    for (;;)
    {
        const ssize_t n = ::read(impl_->out, buf, size);
        if (n >= 0) return std::size_t(n);
        if (errno != EINTR) return 0;
    }
}

int process::wait()
{
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->reaped) return impl_->code;
    }
    // Wait without reaping, so kill() stays safe meanwhile, then reap.
    siginfo_t info{};
    while (waitid(P_PID, id_t(impl_->pid), &info, WEXITED | WNOWAIT) < 0 && errno == EINTR)
    {
    }
    std::lock_guard lock(impl_->mutex);
    int status = 0;
    while (waitpid(impl_->pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    impl_->reaped = true;
    impl_->code = WIFEXITED(status) ? WEXITSTATUS(status)
                                    : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return impl_->code;
}

void process::kill()
{
    impl_->signal(SIGTERM);
}

process::~process()
{
    // SIGKILL: destruction must not hang on a child that ignores SIGTERM.
    impl_->signal(SIGKILL);
    wait();
    close(impl_->out);
}

#endif

process::process(std::unique_ptr<impl> impl)
    : impl_(std::move(impl))
{
}

std::expected<process::result, std::string>
process::run(std::string_view program, std::span<const std::string> args, const options& opt)
{
    auto p = start(program, args, opt);
    if (!p) return std::unexpected(p.error());
    result r;
    char buf[4096];
    while (std::size_t n = (*p)->read(buf, sizeof buf)) r.output.append(buf, n);
    r.exit_code = (*p)->wait();
    return r;
}

// ---------------------------------------------------------------- mapped files

std::expected<mapped_file, std::string> mapped_file::open(const fs::path& path, mode mode)
{
    mapped_file f;
    f.mode_ = mode;
    const bool rw = mode == mode::read_write;
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | (rw ? GENERIC_WRITE : 0),
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           rw ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::unexpected(last_error(path_to_utf8(path).c_str()));
    f.file_ = reinterpret_cast<std::intptr_t>(h);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) return std::unexpected(last_error("size"));
    f.size_ = std::size_t(size.QuadPart);
#else
    const int fd = ::open(path.c_str(), (rw ? O_RDWR | O_CREAT : O_RDONLY) | O_CLOEXEC, 0644);
    if (fd < 0) return std::unexpected(last_error(path.c_str()));
    f.file_ = fd;
    struct stat st{};
    if (fstat(fd, &st) != 0) return std::unexpected(last_error("size"));
    f.size_ = std::size_t(st.st_size);
#endif
    if (auto m = f.map(); !m) return std::unexpected(m.error());
    return f;
}

mapped_file::mapped_file(mapped_file&& other) noexcept
{
    *this = std::move(other);
}

mapped_file& mapped_file::operator=(mapped_file&& other) noexcept
{
    if (this != &other)
    {
        this->~mapped_file();
        file_ = std::exchange(other.file_, no_file);
#ifdef _WIN32
        mapping_ = std::exchange(other.mapping_, nullptr);
#endif
        mode_ = other.mode_;
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

mapped_file::~mapped_file()
{
    unmap();
    if (file_ == no_file) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(file_));
#else
    close(int(file_));  // also drops a flock
#endif
    file_ = no_file;
}

// An empty file has no mapping: data() is null.
std::expected<void, std::string> mapped_file::map()
{
    if (size_ == 0) return {};
    const bool rw = mode_ == mode::read_write;
#ifdef _WIN32
    mapping_ = CreateFileMappingW(reinterpret_cast<HANDLE>(file_), nullptr,
                                  rw ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_) return std::unexpected(last_error("map"));
    data_ = static_cast<std::byte*>(
        MapViewOfFile(mapping_, rw ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, 0));
    if (!data_)
    {
        std::string error = last_error("map");
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return std::unexpected(error);
    }
#else
    void* p = mmap(nullptr, size_, PROT_READ | (rw ? PROT_WRITE : 0), MAP_SHARED, int(file_), 0);
    if (p == MAP_FAILED) return std::unexpected(last_error("map"));
    data_ = static_cast<std::byte*>(p);
#endif
    return {};
}

void mapped_file::unmap()
{
    if (!data_) return;
#ifdef _WIN32
    UnmapViewOfFile(data_);
    CloseHandle(mapping_);
    mapping_ = nullptr;
#else
    munmap(data_, size_);
#endif
    data_ = nullptr;
}

std::expected<void, std::string> mapped_file::resize(std::size_t size)
{
    if (file_ == no_file || mode_ != mode::read_write)
        return std::unexpected(std::string("resize: not open for writing"));
    unmap();
#ifdef _WIN32
    LARGE_INTEGER at{};
    at.QuadPart = LONGLONG(size);
    const auto h = reinterpret_cast<HANDLE>(file_);
    if (!SetFilePointerEx(h, at, nullptr, FILE_BEGIN) || !SetEndOfFile(h))
    {
        std::string error = last_error("resize");
        (void)map();  // keep the old mapping usable
        return std::unexpected(error);
    }
#else
    if (ftruncate(int(file_), off_t(size)) != 0)
    {
        std::string error = last_error("resize");
        (void)map();
        return std::unexpected(error);
    }
#endif
    size_ = size;
    return map();
}

std::expected<void, std::string> mapped_file::flush()
{
    if (!data_) return {};
#ifdef _WIN32
    if (!FlushViewOfFile(data_, 0) || !FlushFileBuffers(reinterpret_cast<HANDLE>(file_)))
        return std::unexpected(last_error("flush"));
#else
    if (msync(data_, size_, MS_SYNC) != 0) return std::unexpected(last_error("flush"));
#endif
    return {};
}

#ifdef _WIN32
namespace
{
// Windows locks byte ranges, and a locked range blocks ReadFile/WriteFile
// from other processes, so lock one byte far past any real data instead.
OVERLAPPED lock_range()
{
    OVERLAPPED ov{};
    ov.Offset = 0;
    ov.OffsetHigh = 0x7fffffff;
    return ov;
}
}  // namespace
#endif

bool mapped_file::try_lock()
{
    if (file_ == no_file) return false;
#ifdef _WIN32
    OVERLAPPED ov = lock_range();
    return LockFileEx(reinterpret_cast<HANDLE>(file_),
                      LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov);
#else
    return flock(int(file_), LOCK_EX | LOCK_NB) == 0;
#endif
}

void mapped_file::unlock()
{
    if (file_ == no_file) return;
#ifdef _WIN32
    OVERLAPPED ov = lock_range();
    UnlockFileEx(reinterpret_cast<HANDLE>(file_), 0, 1, 0, &ov);
#else
    flock(int(file_), LOCK_UN);
#endif
}

// ---------------------------------------------------------------- threads, memory

void lower_thread_priority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 10);
#endif
}

std::size_t available_memory()
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

}  // namespace rap::sys
