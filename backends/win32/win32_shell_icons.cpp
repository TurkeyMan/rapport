#include "win32_shell_icons.h"

#include <windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace rap::win32
{

namespace
{

struct stock_name
{
    const char* name;
    SHSTOCKICONID id;
};
constexpr stock_name stock_names[] = {
    {"drive-harddisk", SIID_DRIVEFIXED},
    {"drive-removable-media", SIID_DRIVEREMOVE},
    {"drive-optical", SIID_DRIVECD},
    {"folder-remote", SIID_DRIVENET},
};

struct known_name
{
    const char* name;
    const KNOWNFOLDERID* id;
};
const known_name known_names[] = {
    {"user-home", &FOLDERID_Profile},
    {"user-desktop", &FOLDERID_Desktop},
    {"folder-download", &FOLDERID_Downloads},
    {"folder-documents", &FOLDERID_Documents},
    {"folder-pictures", &FOLDERID_Pictures},
    {"folder-music", &FOLDERID_Music},
    {"folder-videos", &FOLDERID_Videos},
};

// Shell calls made on each thread, for shell_calls_on_this_thread.
thread_local size_t shell_calls = 0;

// The icon at `index` in `file` (a negative index is a resource id), made at
// `px` from the image nearest that size, as the shell itself does.
HICON extract(const wchar_t* file, int index, int px)
{
    ++shell_calls;
    HICON h = nullptr;
    if (!file || !file[0]) return nullptr;
    if (SHDefExtractIconW(file, index, 0, &h, nullptr, MAKELONG(px, 0)) != S_OK) return nullptr;
    return h;
}

HICON lookup(const std::string& name, int px)
{
    ++shell_calls;
    for (const stock_name& s : stock_names)
        if (name == s.name)
        {
            SHSTOCKICONINFO info{};
            info.cbSize = sizeof info;
            if (FAILED(SHGetStockIconInfo(s.id, SHGSI_ICONLOCATION, &info))) return nullptr;
            return extract(info.szPath, info.iIcon, px);
        }
    for (const known_name& k : known_names)
        if (name == k.name)
        {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (FAILED(SHGetKnownFolderIDList(*k.id, KF_FLAG_DEFAULT, nullptr, &pidl))) return nullptr;
            SHFILEINFOW info{};
            HICON h = nullptr;
            if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &info, sizeof info, SHGFI_PIDL | SHGFI_ICONLOCATION))
                h = extract(info.szDisplayName, info.iIcon, px);
            CoTaskMemFree(pidl);
            return h;
        }
    return nullptr;
}

// An icon's pixels, premultiplied; its mask stands in for alpha when its
// colour has none. Empty unless it's `px` square in colour.
std::vector<uint32_t> pixels_of(HICON h, int px)
{
    ICONINFO ii{};
    if (!GetIconInfo(h, &ii)) return {};
    std::vector<uint32_t> out;
    BITMAP bm{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm) && bm.bmWidth == px && bm.bmHeight == px)
    {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = px;
        bi.bmiHeader.biHeight = -px;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint32_t> colour(size_t(px) * size_t(px)), mask(colour.size());
        HDC dc = GetDC(nullptr);
        const bool got = GetDIBits(dc, ii.hbmColor, 0, UINT(px), colour.data(), &bi, DIB_RGB_COLORS) == px;
        bool has_alpha = false;
        for (const uint32_t c : colour) has_alpha |= (c >> 24) != 0;
        bool masked = has_alpha;
        if (got && !has_alpha && ii.hbmMask)
            masked = GetDIBits(dc, ii.hbmMask, 0, UINT(px), mask.data(), &bi, DIB_RGB_COLORS) == px;
        ReleaseDC(nullptr, dc);
        if (got && masked)
        {
            for (size_t i = 0; i < colour.size(); ++i)
            {
                uint32_t c = colour[i];
                if (!has_alpha) c = (mask[i] & 0xffffff) ? 0 : (c | 0xff000000u);
                const uint32_t a = c >> 24;
                if (a < 255)
                {
                    auto pm = [a](uint32_t v) { return (v * a + 127) / 255; };
                    c = a << 24 | pm((c >> 16) & 0xff) << 16 | pm((c >> 8) & 0xff) << 8 | pm(c & 0xff);
                }
                colour[i] = c;
            }
            out = std::move(colour);
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return out;
}

bool has_shell_icon(const std::string& name)
{
    for (const stock_name& s : stock_names)
        if (name == s.name) return true;
    for (const known_name& k : known_names)
        if (name == k.name) return true;
    return false;
}

struct store
{
    std::mutex m;
    // Map nodes stay put, so a pointer to one's pixels stays good.
    std::map<std::pair<std::string, int>, std::optional<std::vector<uint32_t>>> cache;
    std::map<std::pair<std::string, int>, bool> wanted;  // (name, logical px)
    size_t extractions = 0;
};

store& the_store()
{
    static store s;
    return s;
}

void request_wanted_path_icons(UINT dpi);  // below, with the rest of paths'

}  // namespace

const std::vector<uint32_t>* shell_icon_pixels(const std::string& name, int px)
{
    store& s = the_store();
    std::lock_guard lock(s.m);
    const auto it = s.cache.find({name, px});
    return it != s.cache.end() && it->second ? &*it->second : nullptr;
}

void prepare_shell_icon(const std::string& name, int px)
{
    if (px <= 0 || !has_shell_icon(name)) return;
    store& s = the_store();
    {
        std::lock_guard lock(s.m);
        if (s.cache.contains({name, px})) return;
        ++s.extractions;
    }
    // The shell unlocked: it can take a while.
    std::optional<std::vector<uint32_t>> pixels;
    if (HICON h = lookup(name, px))
    {
        if (std::vector<uint32_t> p = pixels_of(h, px); !p.empty()) pixels = std::move(p);
        DestroyIcon(h);  // ours is the copy
    }
    std::lock_guard lock(s.m);
    s.cache.emplace(std::pair{name, px}, std::move(pixels));
}

void want_shell_icon(const std::string& name, int logical, UINT dpi)
{
    if (!has_shell_icon(name)) return;
    {
        store& s = the_store();
        std::lock_guard lock(s.m);
        s.wanted[{name, logical}] = true;
    }
    prepare_shell_icon(name, MulDiv(logical, int(dpi), 96));
}

void prepare_wanted_shell_icons(UINT dpi)
{
    std::vector<std::pair<std::string, int>> wanted;
    {
        store& s = the_store();
        std::lock_guard lock(s.m);
        for (const auto& [key, _] : s.wanted) wanted.push_back(key);
    }
    for (const auto& [name, logical] : wanted) prepare_shell_icon(name, MulDiv(logical, int(dpi), 96));
    request_wanted_path_icons(dpi);  // found in the background; their names' meanwhile, above
}

size_t shell_icon_extractions()
{
    store& s = the_store();
    std::lock_guard lock(s.m);
    return s.extractions;
}

// -- a path's own icon --------------------------------------------------------

namespace
{

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// One spelling per path: "C:", "C:/" and "C:\" are the same drive's.
std::string path_key(std::string p)
{
    for (char& c : p)
        if (c == '/') c = '\\';
    if (p.size() == 2 && p[1] == ':') p += '\\';
    return p;
}

// A shell bitmap's pixels, premultiplied: it may come either way, so one
// with a colour brighter than its alpha is taken as straight. Empty unless
// it's `px` square.
std::vector<uint32_t> pixels_of_bitmap(HBITMAP b, int px)
{
    BITMAP bm{};
    if (!GetObjectW(b, sizeof bm, &bm) || bm.bmWidth != px || std::abs(bm.bmHeight) != px) return {};
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint32_t> out(size_t(px) * size_t(px));
    HDC dc = GetDC(nullptr);
    const bool got = GetDIBits(dc, b, 0, UINT(px), out.data(), &bi, DIB_RGB_COLORS) == px;
    ReleaseDC(nullptr, dc);
    if (!got) return {};
    bool has_alpha = false, straight = false;
    for (const uint32_t c : out)
    {
        const uint32_t a = c >> 24;
        has_alpha |= a != 0;
        straight |= ((c >> 16) & 0xff) > a || ((c >> 8) & 0xff) > a || (c & 0xff) > a;
    }
    for (uint32_t& c : out)
    {
        if (!has_alpha)
        {
            c |= 0xff000000u;
            continue;
        }
        const uint32_t a = c >> 24;
        if (straight && a < 255)
        {
            auto pm = [a](uint32_t v) { return (v * a + 127) / 255; };
            c = a << 24 | pm((c >> 16) & 0xff) << 16 | pm((c >> 8) & 0xff) << 8 | pm(c & 0xff);
        }
    }
    return out;
}

// One icon lookup by type (it touches no path), alone, before any finder
// asks for a path's. Without it, finders making the process's first
// lookups at once crashed it (an access violation in 2 of 3 fresh runs of
// rapport-win32-path-icons-test); with it, 40 of 40 passed. The cause is
// believed to be the shell setting up its icon cache on the first lookup,
// which isn't safe from several threads at once: believed, not proven. So
// keep it even if a run passes without it: the race is a matter of timing.
// The backend makes it on the UI thread as it sets the post
// (set_path_icon_post), before any finder can start; the call_once is for
// the module used without a backend (the windowless test).
void warm_shell()
{
    static std::once_flag warmed;
    std::call_once(warmed,
                   []
                   {
                       ++shell_calls;
                       SHFILEINFOW w{};
                       SHGetFileInfoW(L"x.txt", FILE_ATTRIBUTE_NORMAL, &w, sizeof w,
                                      SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES);
                   });
}

// What the shell shows for `path` at `px`: from where it keeps the icon (its
// file and index, read at that size), else, for one an icon handler makes,
// the shell item's own image at that size. Reads the path: not on the UI
// thread.
std::optional<std::vector<uint32_t>> find_path_icon(const std::wstring& path, int px)
{
    warm_shell();
    ++shell_calls;
    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof info, SHGFI_ICONLOCATION) && info.szDisplayName[0])
    {
        if (HICON h = extract(info.szDisplayName, info.iIcon, px))
        {
            std::vector<uint32_t> p = pixels_of(h, px);
            DestroyIcon(h);
            if (!p.empty()) return p;
        }
    }
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return std::nullopt;
    HBITMAP b = nullptr;
    std::optional<std::vector<uint32_t>> out;
    if (SUCCEEDED(factory->GetImage(SIZE{px, px}, SIIGBF_ICONONLY, &b)) && b)
    {
        if (std::vector<uint32_t> p = pixels_of_bitmap(b, px); !p.empty()) out = std::move(p);
        DeleteObject(b);
    }
    factory->Release();
    return out;
}

struct path_entry
{
    path_icon_state state = path_icon_state::pending;
    std::vector<uint32_t> pixels;  // set once, when ready; never changed after
};

using path_size = std::pair<std::string, int>;

struct path_store
{
    std::mutex m;
    std::condition_variable work;
    // Map nodes stay put, so a pointer to one's pixels stays good.
    std::map<path_size, path_entry> cache;
    std::deque<path_size> queue;
    // A few finders, so one path that hangs (a network drive gone) doesn't
    // hold up the others. Each lives as long as the process.
    int finders = 0, idle = 0;
    std::function<void(std::function<void()>)> post;
    std::map<size_t, std::function<void(const std::string&, int)>> listeners;
    size_t next_listener = 1;
    std::map<path_size, bool> wanted;  // (key, logical px)
    size_t lookups = 0;
};
constexpr int max_finders = 4;

// Never destroyed: a finder stuck in a network drive at exit still has it.
path_store& the_path_store()
{
    static path_store* s = new path_store;
    return *s;
}

// On the UI thread, through the backend's post.
void tell_listeners(const std::string& key, int px)
{
    path_store& s = the_path_store();
    std::vector<size_t> ids;
    {
        std::lock_guard lock(s.m);
        for (const auto& [id, _] : s.listeners) ids.push_back(id);
    }
    // One by one, looked up again: a listener may remove another (or itself).
    for (const size_t id : ids)
    {
        std::function<void(const std::string&, int)> f;
        {
            std::lock_guard lock(s.m);
            const auto it = s.listeners.find(id);
            if (it == s.listeners.end()) continue;
            f = it->second;
        }
        f(key, px);
    }
}

void find_path_icons()
{
    // Apartment threaded, as the shell wants; for the process's life (it
    // never returns).
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    path_store& s = the_path_store();
    std::unique_lock lock(s.m);
    for (;;)
    {
        ++s.idle;
        s.work.wait(lock, [&] { return !s.queue.empty(); });
        --s.idle;
        const path_size job = std::move(s.queue.front());
        s.queue.pop_front();
        ++s.lookups;
        lock.unlock();
        std::optional<std::vector<uint32_t>> found = find_path_icon(widen(job.first), job.second);
        lock.lock();
        path_entry& e = s.cache[job];
        if (found)
        {
            e.pixels = std::move(*found);
            e.state = path_icon_state::ready;
        }
        else
            e.state = path_icon_state::none;
        if (const auto post = s.post)
        {
            lock.unlock();
            post([job] { tell_listeners(job.first, job.second); });
            lock.lock();
        }
    }
}

}  // namespace

path_icon_state request_path_icon(const std::string& path, int px)
{
    if (path.empty() || px <= 0) return path_icon_state::none;
    path_store& s = the_path_store();
    path_size key{path_key(path), px};
    std::lock_guard lock(s.m);
    if (const auto it = s.cache.find(key); it != s.cache.end()) return it->second.state;
    s.cache.emplace(key, path_entry{});
    s.queue.push_back(std::move(key));
    if (s.idle == 0 && s.finders < max_finders)
    {
        ++s.finders;
        std::thread(find_path_icons).detach();
    }
    else
        s.work.notify_one();
    return path_icon_state::pending;
}

std::string path_icon_key(const std::string& path)
{
    return path_key(path);
}

const std::vector<uint32_t>* path_icon_pixels(const std::string& path, int px)
{
    if (path.empty()) return nullptr;
    path_store& s = the_path_store();
    std::lock_guard lock(s.m);
    const auto it = s.cache.find({path_key(path), px});
    return it != s.cache.end() && it->second.state == path_icon_state::ready ? &it->second.pixels : nullptr;
}

void set_path_icon_post(std::function<void(std::function<void()>)> post)
{
    // Here, on the UI thread before any finder can start (see warm_shell).
    if (post) warm_shell();
    path_store& s = the_path_store();
    std::lock_guard lock(s.m);
    s.post = std::move(post);
}

size_t add_path_icon_listener(std::function<void(const std::string& path, int px)> f)
{
    path_store& s = the_path_store();
    std::lock_guard lock(s.m);
    const size_t id = s.next_listener++;
    s.listeners.emplace(id, std::move(f));
    return id;
}

void remove_path_icon_listener(size_t id)
{
    path_store& s = the_path_store();
    std::lock_guard lock(s.m);
    s.listeners.erase(id);
}

void want_path_icon(const std::string& path, int logical, UINT dpi)
{
    if (path.empty()) return;
    {
        path_store& s = the_path_store();
        std::lock_guard lock(s.m);
        s.wanted[{path_key(path), logical}] = true;
    }
    request_path_icon(path, MulDiv(logical, int(dpi), 96));
}

namespace
{

void request_wanted_path_icons(UINT dpi)
{
    std::vector<path_size> wanted;
    {
        path_store& s = the_path_store();
        std::lock_guard lock(s.m);
        for (const auto& [key, _] : s.wanted) wanted.push_back(key);
    }
    for (const auto& [path, logical] : wanted) request_path_icon(path, MulDiv(logical, int(dpi), 96));
}

}  // namespace

size_t path_icon_lookups()
{
    path_store& s = the_path_store();
    std::lock_guard lock(s.m);
    return s.lookups;
}

size_t shell_calls_on_this_thread()
{
    return shell_calls;
}

}  // namespace rap::win32
