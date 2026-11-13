// A path's own icon (icon::for_path) from the shell, with no window: asked
// for, it's pending at once and found in the background, then told on the
// asking thread through the post it was given. The system drive's, a
// program's and a folder's are found, that size and not blank; a path that
// isn't there has none. The program's isn't the drive's (the icon follows
// the path), and the system drive's is the generic drive icon only where
// the shell itself keeps them in the same place (so it holds on any
// machine, a CI runner's included). Asking again, or in another spelling,
// doesn't ask the shell again.
//   rapport-win32-path-icons-test
// And by hand, a path that can't answer (never mapped as a drive):
//   rapport-win32-path-icons-test --slow \\10.255.255.1\share
#include "win32_shell_icons.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool not_blank(const std::vector<uint32_t>& p)
{
    size_t opaque = 0;
    for (const uint32_t c : p) opaque += (c >> 24) != 0;
    return opaque > p.size() / 8;
}

// The shell's own word on where it keeps an icon, as "file,index".
std::wstring location_of(const std::wstring& path)
{
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof info, SHGFI_ICONLOCATION)) return {};
    return std::wstring(info.szDisplayName) + L"," + std::to_wstring(info.iIcon);
}

std::wstring generic_drive_location()
{
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof info;
    if (FAILED(SHGetStockIconInfo(SIID_DRIVEFIXED, SHGSI_ICONLOCATION, &info))) return {};
    return std::wstring(info.szPath) + L"," + std::to_wstring(info.iIcon);
}

// Runs what the finders post to this thread until `done` or `seconds` pass.
template <class F>
bool pump_until(F done, int seconds)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!done())
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, DWORD(left.count()), QS_ALLPOSTMESSAGE);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            if (m.message == WM_APP)
            {
                auto* job = reinterpret_cast<std::function<void()>*>(m.lParam);
                (*job)();
                delete job;
            }
    }
    return true;
}

// --slow <path>: a manual check, not run by ctest. Asks for `path`'s icon
// (an unreachable UNC path, say \\10.255.255.1\share) and keeps this thread
// pumping meanwhile: how long asking took, the longest this thread was ever
// kept from its messages, how long the shell took to give up, and what it
// gave.
int slow_check(const std::string& path)
{
    using namespace rap::win32;
    using clock = std::chrono::steady_clock;
    const DWORD self = GetCurrentThreadId();
    MSG m;
    PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);
    set_path_icon_post(
        [self](std::function<void()> f)
        {
            auto* job = new std::function<void()>(std::move(f));
            if (!PostThreadMessageW(self, WM_APP, 0, reinterpret_cast<LPARAM>(job))) delete job;
        });
    bool told = false;
    add_path_icon_listener([&](const std::string&, int) { told = true; });
    const auto t0 = clock::now();
    const path_icon_state first = request_path_icon(path, 16);
    const auto asked = clock::now();
    auto ms = [](clock::duration d) { return long(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()); };
    std::printf("asked for %s: %s, returned in %ld ms\n", path.c_str(),
                first == path_icon_state::pending ? "pending" : "not pending", ms(asked - t0));
    long longest = 0;
    auto last = clock::now();
    while (!told && clock::now() - t0 < std::chrono::minutes(3))
    {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLPOSTMESSAGE);
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            if (m.message == WM_APP)
            {
                auto* job = reinterpret_cast<std::function<void()>*>(m.lParam);
                (*job)();
                delete job;
            }
        const auto now = clock::now();
        longest = (std::max)(longest, ms(now - last));
        last = now;
    }
    const path_icon_state end = request_path_icon(path, 16);
    std::printf("told after %ld ms: %s; this thread's longest wait between messages: %ld ms\n", ms(clock::now() - t0),
                !told ? "never (gave up waiting after 3 minutes)"
                : end == path_icon_state::ready ? "ready (an icon)"
                : end == path_icon_state::none  ? "none (the fallback stays)"
                                                : "still pending",
                longest);
    set_path_icon_post({});
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (argc == 3 && std::string(argv[1]) == "--slow") return slow_check(argv[2]);
    using namespace rap::win32;
    MSG m;
    PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);  // this thread's queue, for the posts
    const DWORD self = GetCurrentThreadId();
    set_path_icon_post(
        [self](std::function<void()> f)
        {
            auto* job = new std::function<void()>(std::move(f));
            if (!PostThreadMessageW(self, WM_APP, 0, reinterpret_cast<LPARAM>(job))) delete job;
        });
    std::map<std::pair<std::string, int>, int> told;
    bool told_elsewhere = false;
    const size_t listener = add_path_icon_listener(
        [&](const std::string& path, int px)
        {
            ++told[{path, px}];
            told_elsewhere |= GetCurrentThreadId() != self;
        });

    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring windir = windows;
    const std::wstring drive = windir.substr(0, 3);  // "C:\"
    // A drive letter nothing has.
    std::wstring free_drive;
    const DWORD drives = GetLogicalDrives();
    for (wchar_t l = L'Z'; l >= L'D' && free_drive.empty(); --l)
        if (!(drives & (1u << (l - L'A')))) free_drive = std::wstring(1, l) + L":\\";

    struct ask
    {
        std::string what, path;
        bool there;
    };
    std::vector<ask> asks = {
        {"the system drive", narrow(drive), true},
        {"explorer.exe", narrow(windir + L"\\explorer.exe"), true},
        {"the Windows folder", narrow(windir), true},
        {"a file that isn't there", narrow(windir + L"\\rapport-no-such-file.xyz"), false},
    };
    if (!free_drive.empty()) asks.push_back({"a drive letter nothing has", narrow(free_drive), false});

    expect(path_icon_lookups() == 0, "the cache starts cold");
    const int sizes[] = {16, 32};
    for (const ask& a : asks)
        for (const int px : sizes)
        {
            const auto t0 = std::chrono::steady_clock::now();
            const path_icon_state st = request_path_icon(a.path, px);
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            const std::string what = a.what + " at " + std::to_string(px);
            expect(st == path_icon_state::pending, what + ": pending when first asked");
            expect(ms < 50, what + ": asking returns at once (" + std::to_string(ms) + " ms)");
            expect(!path_icon_pixels(a.path, px), what + ": no pixels yet");
        }

    const bool all = pump_until(
        [&]
        {
            for (const ask& a : asks)
                for (const int px : sizes)
                    if (request_path_icon(a.path, px) == path_icon_state::pending) return false;
            return true;
        },
        60);
    expect(all, "all found within a minute");

    for (const ask& a : asks)
        for (const int px : sizes)
        {
            const std::string what = a.what + " at " + std::to_string(px);
            const path_icon_state st = request_path_icon(a.path, px);
            const auto* p = path_icon_pixels(a.path, px);
            if (!a.there)
            {
                expect(st == path_icon_state::none && !p, what + ": none");
                continue;
            }
            expect(st == path_icon_state::ready && p, what + ": found");
            if (!p) continue;
            expect(p->size() == size_t(px) * size_t(px), what + ": that size");
            expect(not_blank(*p), what + ": not blank");
        }
    // The last posts: an icon is ready before it's told.
    pump_until([&] { return told.size() >= asks.size() * std::size(sizes); }, 10);
    bool once_each = told.size() == asks.size() * std::size(sizes);
    for (const auto& [_, n] : told) once_each &= n == 1;
    expect(once_each, "each told once");
    expect(!told_elsewhere, "and told on the asking thread");

    const std::string drive_path = narrow(drive), exe = narrow(windir + L"\\explorer.exe");
    for (const int px : sizes)
    {
        const auto* d = path_icon_pixels(drive_path, px);
        const auto* e = path_icon_pixels(exe, px);
        expect(d && e && *d != *e, "explorer.exe's icon isn't the drive's at " + std::to_string(px));
        prepare_shell_icon("drive-harddisk", px);
        const auto* generic = shell_icon_pixels("drive-harddisk", px);
        const std::wstring here = location_of(drive), stock = generic_drive_location();
        std::printf("     the system drive's icon is at %s; the generic drive's at %s\n", narrow(here).c_str(),
                    narrow(stock).c_str());
        if (!here.empty() && _wcsicmp(here.c_str(), stock.c_str()) != 0)
            expect(d && generic && *d != *generic,
                   "the system drive's isn't the generic drive icon at " + std::to_string(px));
        else
            std::printf("     (the shell keeps them in the same place here: not compared)\n");
    }

    const size_t asked = path_icon_lookups();
    expect(asked == asks.size() * std::size(sizes), "the shell asked once per path and size");
    std::string other = drive_path;
    other.pop_back();  // "C:"
    expect(request_path_icon(other, 16) == path_icon_state::ready &&
               request_path_icon(other + "/", 16) == path_icon_state::ready &&
               path_icon_pixels(other, 16) == path_icon_pixels(drive_path, 16),
           "\"C:\", \"C:/\" and \"C:\\\" are the same drive's");
    for (const ask& a : asks) request_path_icon(a.path, 16);
    expect(path_icon_lookups() == asked, "asked again, kept: not asked of the shell again");
    expect(request_path_icon("", 16) == path_icon_state::none && path_icon_lookups() == asked,
           "no path: none, and the shell not asked");

    remove_path_icon_listener(listener);
    set_path_icon_post({});
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
