// rap::sys: the few OS services an application needs beside its UI, with
// one API on Linux and Windows. Standard C++ plus the OS; no toolkit, no
// foundation library. Strings are UTF-8 everywhere; paths are
// std::filesystem::path (wide on Windows, converted at the edges here).
//
// Part of librapport but independent of the backend: callable before a Ui is
// loaded and from any thread. The core uses it to find and load backends.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rap::sys
{

// -- strings, paths -----------------------------------------------------------

// UTF-8 to path and back (fs::path's own conversions use the ANSI code page
// on Windows).
std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path&);

// An environment variable, UTF-8. Empty optional if unset.
std::optional<std::string> env(const char* name);

// -- files --------------------------------------------------------------------

// fopen for any path (wide on Windows).
std::FILE* fopen(const std::filesystem::path&, const char* mode);

// -- locations ----------------------------------------------------------------

// The running executable, absolute.
std::filesystem::path executable_path();

// Per-user directories for `app`, created on first use:
//   config: $XDG_CONFIG_HOME/app   or %APPDATA%\app
//   cache:  $XDG_CACHE_HOME/app    or %LOCALAPPDATA%\app\cache
//   data:   $XDG_DATA_HOME/app     or %LOCALAPPDATA%\app
std::filesystem::path config_dir(std::string_view app);
std::filesystem::path cache_dir(std::string_view app);
std::filesystem::path data_dir(std::string_view app);

// -- shared libraries ---------------------------------------------------------

// A dynamically loaded library (dlopen / LoadLibrary). Move-only; unloads
// when destroyed unless released.
class library
{
public:
    // `name` is a file name searched the platform's way ("libfoo.so.1",
    // "foo-1.dll") or a path.
    static std::expected<library, std::string> open(const std::filesystem::path& name);

    library() = default;
    library(library&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }
    library& operator=(library&& other) noexcept;
    library(const library&) = delete;
    library& operator=(const library&) = delete;
    ~library();

    explicit operator bool() const { return handle_ != nullptr; }
    void* symbol(const char* name) const;
    template <class Fn>
    Fn* function(const char* name) const
    {
        return reinterpret_cast<Fn*>(symbol(name));
    }
    // Keep it loaded for the life of the process.
    void release() { handle_ = nullptr; }

private:
    explicit library(void* h)
        : handle_(h)
    {
    }
    void* handle_ = nullptr;
};

// Whether `name` would be found by library::open, judged by the file being
// on the loader's search path. A few stats, no load: use it to decide what
// to offer before paying for the load.
bool library_installed(std::string_view name);

// -- processes ----------------------------------------------------------------

// How Process starts a child.
struct process_options
{
    std::filesystem::path dir;  // working directory; empty: the caller's
    // Set in the child's environment, replacing any inherited value.
    std::vector<std::pair<std::string, std::string>> env;
    // false: stderr goes to the null device, so warnings can't mix into
    // output that is parsed.
    bool merge_stderr = true;
};

// A child process with stdin from the null device and stdout and stderr
// merged into one pipe, or stderr discarded (no console window on
// Windows). read() and wait() belong to one thread; kill() may be called
// from any thread at any time, before or after the process exits, and also
// ends anything the child started (its process group; a job object on
// Windows). Destroying a process that wasn't waited for kills it and reaps
// it.
class process
{
public:
    using options = process_options;

    struct result
    {
        int exit_code = -1;
        std::string output;  // stdout and stderr interleaved
    };

    // `program` is searched on PATH unless it is a path; `args` follow it.
    static std::expected<std::unique_ptr<process>, std::string>
    start(std::string_view program, std::span<const std::string> args, const options& = {});

    // Starts, reads all output and waits.
    static std::expected<result, std::string>
    run(std::string_view program, std::span<const std::string> args, const options& = {});

    process(const process&) = delete;
    process& operator=(const process&) = delete;
    ~process();

    // Blocks for output; 0 once the child closed it (normally on exit).
    std::size_t read(char* buf, std::size_t size);
    // Waits for the exit and returns its code; a POSIX signal death is
    // 128 + signal. Read the output to the end first, or a child blocked on a
    // full pipe never exits. Repeated calls return the same code.
    int wait();
    void kill();

private:
    struct impl;
    explicit process(std::unique_ptr<impl>);
    std::unique_ptr<impl> impl_;
};

// -- mapped files -------------------------------------------------------------

// A file mapped into memory, for caches the OS pages in on demand. Map a
// file read-only, or read-write to grow it in place: resize() may move the
// mapping, so data structures inside it should link by offset, not pointer.
// Pages written through data() reach the file in the OS's own time, or at
// flush(). Move-only.
class mapped_file
{
public:
    enum class mode
    {
        read,
        read_write,  // creates the file if missing
    };

    static std::expected<mapped_file, std::string> open(const std::filesystem::path&, mode);

    mapped_file() = default;
    mapped_file(mapped_file&& other) noexcept;
    mapped_file& operator=(mapped_file&& other) noexcept;
    mapped_file(const mapped_file&) = delete;
    mapped_file& operator=(const mapped_file&) = delete;
    ~mapped_file();

    explicit operator bool() const { return file_ != no_file; }
    std::byte* data() { return data_; }
    const std::byte* data() const { return data_; }
    std::size_t size() const { return size_; }

    // Read-write only: sets the file's length (new bytes are zero) and
    // remaps it. Grow in large steps; each call costs a remap.
    std::expected<void, std::string> resize(std::size_t size);
    // Read-write only: writes changed pages to disk before returning.
    std::expected<void, std::string> flush();

    // An exclusive advisory lock on the whole file, across processes; false
    // if another process holds it, or if the filesystem can't lock (WSL paths
    // seen from Windows, some network shares). Released by unlock() or on
    // close.
    bool try_lock();
    void unlock();

private:
    std::expected<void, std::string> map();
    void unmap();

    static constexpr std::intptr_t no_file = -1;  // INVALID_HANDLE_VALUE, or no fd
    std::intptr_t file_ = no_file;                // HANDLE or fd
#ifdef _WIN32
    void* mapping_ = nullptr;  // the file mapping object's HANDLE
#endif
    mode mode_ = mode::read;
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

// -- threads, memory ----------------------------------------------------------

// Let the calling thread yield to the UI and other foreground work.
void lower_thread_priority();

// Memory the system could hand out without swapping; 0 if unknown.
std::size_t available_memory();

}  // namespace rap::sys
