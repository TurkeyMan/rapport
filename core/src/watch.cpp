// rap::sys::directory_watch: ReadDirectoryChangesW on Windows, inotify on
// Linux, each on a thread of its own, with the same coalescing of a file's
// events over the settle delay.

#include "rapport/sys.h"

#include <map>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <fcntl.h>

#include <cerrno>
#include <climits>
#include <cstring>
#endif

namespace fs = std::filesystem;

namespace rap::sys
{
namespace
{

using clock = std::chrono::steady_clock;
using kind = file_event::kind;

// A file's events held until it has been quiet for the settle delay, merged
// so its first kind holds [watch_options::settle].
class coalescer
{
public:
    explicit coalescer(std::chrono::milliseconds settle)
        : settle_(settle)
    {
    }

    void add(file_event e)
    {
        const auto now = clock::now();
        if (e.what == kind::renamed)
        {
            // A rename of something still held carries it to the new name.
            if (auto old = held_.find(e.old_path); old != held_.end())
            {
                file_event moved = old->second.event;
                held_.erase(old);
                if (moved.what == kind::added)
                    e = {kind::added, e.path, {}};
                else if (moved.what == kind::renamed)
                    e.old_path = moved.old_path;
            }
        }
        auto it = held_.find(e.path);
        if (it == held_.end())
        {
            fs::path key = e.path;  // (before e moves: arguments' order is unspecified)
            held_.emplace(std::move(key), held{std::move(e), now});
            return;
        }
        file_event& was = it->second.event;
        it->second.last = now;
        switch (e.what)
        {
        case kind::added:
            // Gone and back: changed.
            if (was.what == kind::removed) was = {kind::modified, e.path, {}};
            break;
        case kind::removed:
            if (was.what == kind::added)
                held_.erase(it);  // came and went: nothing
            else if (was.what == kind::renamed)
                was = {kind::removed, was.old_path, {}};
            else
                was = std::move(e);
            break;
        case kind::modified: break;  // added, renamed or modified hold
        case kind::renamed: was = std::move(e); break;
        }
    }

    // The events quiet long enough (all of them with `all`).
    std::vector<file_event> take(bool all)
    {
        std::vector<file_event> out;
        const auto now = clock::now();
        for (auto it = held_.begin(); it != held_.end();)
            if (all || now - it->second.last >= settle_)
            {
                out.push_back(std::move(it->second.event));
                it = held_.erase(it);
            }
            else
                ++it;
        return out;
    }

    // How long until the next held event is due; none if nothing's held.
    std::optional<std::chrono::milliseconds> next_due() const
    {
        if (held_.empty()) return std::nullopt;
        auto earliest = (clock::time_point::max)();
        // ((std::min): windows.h may define min and max as macros.)
        for (auto& [path, h] : held_) earliest = (std::min)(earliest, h.last + settle_);
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - clock::now());
        return (std::max)(wait, std::chrono::milliseconds(0));
    }

private:
    struct held
    {
        file_event event;
        clock::time_point last;
    };
    std::chrono::milliseconds settle_;
    std::map<fs::path, held> held_;
};

// Hands a read's events on: through the coalescer, or at once with no
// settle delay.
struct dispatcher
{
    coalescer pending;
    std::chrono::milliseconds settle;
    std::function<void(watch_batch)> changed;

    void deliver(std::vector<file_event> events, bool overflow)
    {
        if (settle.count() > 0)
        {
            for (file_event& e : events) pending.add(std::move(e));
            events = pending.take(false);
        }
        if (!events.empty() || overflow) changed({std::move(events), overflow});
    }
    void due()
    {
        if (auto events = pending.take(false); !events.empty()) changed({std::move(events), false});
    }
    int timeout_ms() const
    {
        const auto wait = pending.next_due();
        return wait ? int(wait->count()) : -1;
    }
};

}  // namespace

#ifdef _WIN32

struct directory_watch::impl
{
    HANDLE folder = INVALID_HANDLE_VALUE;
    HANDLE stop = nullptr;
    std::thread thread;
    ~impl()
    {
        if (stop) SetEvent(stop);
        if (thread.joinable()) thread.join();
        if (folder != INVALID_HANDLE_VALUE) CloseHandle(folder);
        if (stop) CloseHandle(stop);
    }
};

std::expected<std::unique_ptr<directory_watch>, std::string>
directory_watch::start(const fs::path& folder, const watch_options& options, std::function<void(watch_batch)> changed)
{
    auto i = std::make_unique<impl>();
    i->folder = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (i->folder == INVALID_HANDLE_VALUE)
        return std::unexpected("can't watch " + path_to_utf8(folder) + " (error " + std::to_string(GetLastError()) + ")");
    i->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const fs::path root = fs::absolute(folder);
    i->thread = std::thread(
        [h = i->folder, stop = i->stop, root, recursive = options.recursive,
         d = dispatcher{coalescer(options.settle), options.settle, std::move(changed)}]() mutable
        {
            std::vector<DWORD> words(16 * 1024);  // DWORD-aligned, as the records must be
            std::byte* buffer = reinterpret_cast<std::byte*>(words.data());
            const DWORD buffer_size = DWORD(words.size() * sizeof(DWORD));
            HANDLE io = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            OVERLAPPED ov{};
            bool reading = false;
            for (;;)
            {
                if (!reading)
                {
                    ov = {};
                    ov.hEvent = io;
                    ResetEvent(io);
                    if (!ReadDirectoryChangesW(h, buffer, buffer_size, recursive,
                                               FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                                   FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                                   FILE_NOTIFY_CHANGE_CREATION,
                                               nullptr, &ov, nullptr))
                        break;  // the folder went
                    reading = true;
                }
                const HANDLE waits[] = {stop, io};
                const int timeout = d.timeout_ms();
                const DWORD r = WaitForMultipleObjects(2, waits, FALSE, timeout < 0 ? INFINITE : DWORD(timeout));
                if (r == WAIT_TIMEOUT)
                {
                    d.due();
                    continue;
                }
                if (r != WAIT_OBJECT_0 + 1)
                {
                    CancelIoEx(h, &ov);
                    DWORD ignored = 0;
                    GetOverlappedResult(h, &ov, &ignored, TRUE);
                    break;
                }
                reading = false;
                DWORD bytes = 0;
                if (!GetOverlappedResult(h, &ov, &bytes, FALSE)) break;
                if (bytes == 0)
                {
                    // The system's buffer overflowed: events were lost.
                    d.deliver({}, true);
                    continue;
                }
                std::vector<file_event> events;
                std::optional<fs::path> renamed_from;
                for (auto* n = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer);;)
                {
                    const fs::path path =
                        root / std::wstring(n->FileName, n->FileNameLength / sizeof(wchar_t));
                    switch (n->Action)
                    {
                    case FILE_ACTION_ADDED: events.push_back({kind::added, path, {}}); break;
                    case FILE_ACTION_REMOVED: events.push_back({kind::removed, path, {}}); break;
                    case FILE_ACTION_MODIFIED: events.push_back({kind::modified, path, {}}); break;
                    case FILE_ACTION_RENAMED_OLD_NAME:
                        if (renamed_from) events.push_back({kind::removed, *renamed_from, {}});
                        renamed_from = path;
                        break;
                    case FILE_ACTION_RENAMED_NEW_NAME:
                        // Paired with its old name, or moved in from outside.
                        if (renamed_from)
                            events.push_back({kind::renamed, path, *renamed_from});
                        else
                            events.push_back({kind::added, path, {}});
                        renamed_from.reset();
                        break;
                    }
                    if (!n->NextEntryOffset) break;
                    n = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(reinterpret_cast<const std::byte*>(n) +
                                                                         n->NextEntryOffset);
                }
                // An old name with no new one: moved out.
                if (renamed_from) events.push_back({kind::removed, *renamed_from, {}});
                d.deliver(std::move(events), false);
            }
            CloseHandle(io);
        });
    return std::unique_ptr<directory_watch>(new directory_watch(std::move(i)));
}

#else

struct directory_watch::impl
{
    int fd = -1;
    int stop[2] = {-1, -1};
    std::thread thread;
    ~impl()
    {
        if (stop[1] >= 0)
        {
            const char c = 0;
            (void)!::write(stop[1], &c, 1);
        }
        if (thread.joinable()) thread.join();
        for (int f : {fd, stop[0], stop[1]})
            if (f >= 0) ::close(f);
    }
};

std::expected<std::unique_ptr<directory_watch>, std::string>
directory_watch::start(const fs::path& folder, const watch_options& options, std::function<void(watch_batch)> changed)
{
    auto i = std::make_unique<impl>();
    i->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (i->fd < 0 || pipe2(i->stop, O_CLOEXEC) != 0)
        return std::unexpected(std::string("can't watch: ") + std::strerror(errno));
    constexpr uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVED_FROM |
                              IN_MOVED_TO | IN_DONT_FOLLOW | IN_ONLYDIR;
    auto watches = std::make_shared<std::map<int, fs::path>>();
    // A folder, and with `recursive` the folders in it.
    auto add = [fd = i->fd, watches, recursive = options.recursive](const fs::path& dir)
    {
        std::vector<fs::path> todo{dir};
        while (!todo.empty())
        {
            fs::path d = std::move(todo.back());
            todo.pop_back();
            const int wd = inotify_add_watch(fd, d.c_str(), mask);
            if (wd < 0) continue;
            (*watches)[wd] = d;
            if (!recursive) break;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(d, ec))
                if (entry.is_directory(ec) && !entry.is_symlink(ec)) todo.push_back(entry.path());
        }
        return !watches->empty();
    };
    const fs::path root = fs::absolute(folder);
    if (!add(root)) return std::unexpected("can't watch " + root.string() + ": " + std::strerror(errno));

    i->thread = std::thread(
        [fd = i->fd, stop = i->stop[0], watches, add, recursive = options.recursive,
         d = dispatcher{coalescer(options.settle), options.settle, std::move(changed)}]() mutable
        {
            alignas(inotify_event) char buffer[64 * 1024];
            for (;;)
            {
                pollfd p[2] = {{fd, POLLIN, 0}, {stop, POLLIN, 0}};
                const int r = ::poll(p, 2, d.timeout_ms());
                if (r < 0 && errno == EINTR) continue;
                if (r < 0 || (p[1].revents & POLLIN)) break;
                if (r == 0)
                {
                    d.due();
                    continue;
                }
                const ssize_t n = ::read(fd, buffer, sizeof buffer);
                if (n <= 0) continue;
                std::vector<file_event> events;
                std::map<uint32_t, fs::path> moved_from;  // by cookie, until its pair
                bool overflow = false;
                for (char* at = buffer; at < buffer + n;)
                {
                    const auto* e = reinterpret_cast<const inotify_event*>(at);
                    at += sizeof(inotify_event) + e->len;
                    if (e->mask & IN_Q_OVERFLOW)
                    {
                        overflow = true;
                        continue;
                    }
                    if (e->mask & IN_IGNORED)
                    {
                        watches->erase(e->wd);
                        continue;
                    }
                    auto dir = watches->find(e->wd);
                    if (dir == watches->end() || e->len == 0) continue;
                    const fs::path path = dir->second / e->name;
                    if (e->mask & IN_CREATE)
                    {
                        events.push_back({kind::added, path, {}});
                        if (recursive && (e->mask & IN_ISDIR)) add(path);
                    }
                    else if (e->mask & IN_DELETE)
                        events.push_back({kind::removed, path, {}});
                    else if (e->mask & IN_MOVED_FROM)
                        moved_from[e->cookie] = path;
                    else if (e->mask & IN_MOVED_TO)
                    {
                        // Paired with its old name, or moved in from outside.
                        if (auto from = moved_from.find(e->cookie); from != moved_from.end())
                        {
                            events.push_back({kind::renamed, path, from->second});
                            moved_from.erase(from);
                        }
                        else
                            events.push_back({kind::added, path, {}});
                        if (recursive && (e->mask & IN_ISDIR)) add(path);
                    }
                    else
                        events.push_back({kind::modified, path, {}});
                }
                // An old name with no new one: moved out.
                for (auto& [cookie, path] : moved_from) events.push_back({kind::removed, path, {}});
                d.deliver(std::move(events), overflow);
            }
        });
    return std::unique_ptr<directory_watch>(new directory_watch(std::move(i)));
}

#endif

directory_watch::directory_watch(std::unique_ptr<impl> i)
    : impl_(std::move(i))
{
}

directory_watch::~directory_watch() = default;

}  // namespace rap::sys
