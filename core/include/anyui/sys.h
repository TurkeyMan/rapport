// anyui::sys: the few OS services an application needs beside its UI, with
// one API on Linux and Windows. Standard C++ plus the OS; no toolkit, no
// foundation library. Strings are UTF-8 everywhere; paths are
// std::filesystem::path (wide on Windows, converted at the edges here).
//
// Part of libanyui but independent of the backend: callable before a Ui is
// loaded and from any thread. The core uses it to find and load backends.
#pragma once

#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace anyui::sys
{

// ---------------------------------------------------------------- strings, paths

// UTF-8 to path and back (fs::path's own conversions use the ANSI code page
// on Windows).
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string pathToUtf8(const std::filesystem::path&);

// An environment variable, UTF-8. Empty optional if unset.
std::optional<std::string> env(const char* name);

// ---------------------------------------------------------------- files

// fopen for any path (wide on Windows).
std::FILE* fopen(const std::filesystem::path&, const char* mode);

// ---------------------------------------------------------------- locations

// The running executable, absolute.
std::filesystem::path executablePath();

// Per-user directories for `app`, created on first use:
//   config: $XDG_CONFIG_HOME/app   or %APPDATA%\app
//   cache:  $XDG_CACHE_HOME/app    or %LOCALAPPDATA%\app\cache
//   data:   $XDG_DATA_HOME/app     or %LOCALAPPDATA%\app
std::filesystem::path configDir(std::string_view app);
std::filesystem::path cacheDir(std::string_view app);
std::filesystem::path dataDir(std::string_view app);

// ---------------------------------------------------------------- shared libraries

// A dynamically loaded library (dlopen / LoadLibrary). Move-only; unloads
// when destroyed unless released.
class Library
{
public:
    // `name` is a file name searched the platform's way ("libfoo.so.1",
    // "foo-1.dll") or a path.
    static std::expected<Library, std::string> open(const std::filesystem::path& name);

    Library() = default;
    Library(Library&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }
    Library& operator=(Library&& other) noexcept;
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    ~Library();

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
    explicit Library(void* h)
        : handle_(h)
    {
    }
    void* handle_ = nullptr;
};

// Whether `name` would be found by Library::open, judged by the file being
// on the loader's search path. A few stats, no load: use it to decide what
// to offer before paying for the load.
bool libraryInstalled(std::string_view name);

// ---------------------------------------------------------------- threads, memory

// Let the calling thread yield to the UI and other foreground work.
void lowerThreadPriority();

// Memory the system could hand out without swapping; 0 if unknown.
std::size_t availableMemory();

}  // namespace anyui::sys
