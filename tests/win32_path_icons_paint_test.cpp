// Paths' own icons (icon::for_path) on a tree's rows and a list's, in a
// window that's never shown, painted into memory (WM_PRINTCLIENT): painting
// never enters the shell, before the icons are found or after, and once
// they're found both show them in place of their fallbacks.
//   rapport-win32-path-icons-paint-test (static builds: it reads the cache)
#include <rapport/rapport.h>

#include "win32_shell_icons.h"  // after rapport: windows.h's min and max

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

RAPPORT_BUILTIN_BACKEND(win32);

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

std::wstring windows_dir()
{
    wchar_t w[MAX_PATH] = {};
    GetWindowsDirectoryW(w, MAX_PATH);
    return w;
}

struct files : rap::list_model
{
    std::vector<std::string> paths;
    size_t row_count() const override { return paths.size(); }
    std::string text(size_t row, size_t) const override { return paths[row]; }
    rap::icon icon(size_t row, size_t column) const override
    {
        return column == 0 ? rap::icon::for_path(paths[row], "application-x-executable") : rap::icon{};
    }
};

HWND find_child(HWND top, const wchar_t* cls)
{
    struct search
    {
        const wchar_t* cls;
        HWND found;
    } s{cls, nullptr};
    EnumChildWindows(
        top,
        [](HWND c, LPARAM q) -> BOOL
        {
            auto* s = reinterpret_cast<search*>(q);
            wchar_t name[32] = {};
            GetClassNameW(c, name, 32);
            if (!_wcsicmp(name, s->cls) && !s->found) s->found = c;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&s));
    return s.found;
}

void pump()
{
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// What `h` paints of its client area, into memory: its pixels, `w` wide.
struct picture
{
    int w = 0;
    std::vector<uint32_t> pixels;
    // Its top-left corner, `cw` by `ch`: where a list's first rows' icons are.
    std::vector<uint32_t> corner(int cw, int ch) const
    {
        std::vector<uint32_t> out;
        const int ht = w ? int(pixels.size()) / w : 0;
        for (int y = 0; y < (std::min)(ch, ht); ++y)
            for (int x = 0; x < (std::min)(cw, w); ++x) out.push_back(pixels[size_t(y) * size_t(w) + size_t(x)]);
        return out;
    }
};
picture print_client(HWND h)
{
    RECT r{};
    GetClientRect(h, &r);
    const int w = r.right, ht = r.bottom;
    if (w <= 0 || ht <= 0) return {};
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -ht;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    SendMessageW(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
    GdiFlush();
    picture out{w, std::vector<uint32_t>(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + size_t(w) * size_t(ht))};
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return out;
}

// Image `i` of the tree's image list, drawn over white.
std::vector<uint32_t> tree_image(HWND tree, int i)
{
    HIMAGELIST list = TreeView_GetImageList(tree, TVSIL_NORMAL);
    int cx = 0, cy = 0;
    if (!list || !ImageList_GetIconSize(list, &cx, &cy) || ImageList_GetImageCount(list) <= i) return {};
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cx;
    bi.bmiHeader.biHeight = -cy;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT r{0, 0, cx, cy};
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    ImageList_Draw(list, i, dc, 0, 0, ILD_NORMAL);
    GdiFlush();
    std::vector<uint32_t> out(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + size_t(cx) * size_t(cy));
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return out;
}

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

}  // namespace

int main(int argc, char** argv)
{
    using namespace rap::win32;
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::printf("FAIL %s\n", ui.error().c_str());
        return 1;
    }
    expect(path_icon_lookups() == 0, "the cache starts cold");

    const std::wstring windir = windows_dir();
    const std::string drive = narrow(windir.substr(0, 3)), exe = narrow(windir + L"\\explorer.exe"),
                      regedit = narrow(windir + L"\\regedit.exe");
    rap::tree_item c_drive, system;
    c_drive.label = c_drive.id = drive;
    c_drive.icon = rap::icon::for_path(drive, "drive-harddisk");
    system.label = system.id = narrow(windir);
    system.icon = rap::icon::for_path(narrow(windir), "folder");
    files model;
    model.paths = {exe, regedit};

    // The finders held until the "before" is seen: a local path's icon can
    // be found within the tree's own making, before anything could look.
    hold_path_icon_finders(true);
    using namespace rap::layout;
    const wchar_t* title = L"rapport path icons paint test";
    auto win = (*ui)->create(window{"rapport path icons paint test",
                                    {vbox{tree{{c_drive, system}}.stretch(),
                                          list{{{.title = "Name", .stretch = true}}, &model}.stretch()}}}
                                 .size(60_ch, 24_lines));
    // Never shown: no show(), and nothing here makes it visible.
    HWND top = FindWindowW(L"rapportWindow", title);
    expect(top != nullptr && !IsWindowVisible(top), "the window is there, not shown");
    if (!top) return 1;
    HWND tree_hwnd = find_child(top, WC_TREEVIEWW), list_hwnd = find_child(top, WC_LISTVIEWW);
    expect(tree_hwnd && list_hwnd, "the tree and the list are there");
    if (!tree_hwnd || !list_hwnd) return 1;
    pump();  // its layout settled
    RECT tr{}, lr{};
    GetClientRect(tree_hwnd, &tr);
    GetClientRect(list_hwnd, &lr);
    expect(tr.right > 0 && tr.bottom > 0 && lr.right > 0 && lr.bottom > 0, "both laid out");
    const int px = MulDiv(16, int(GetDpiForWindow(top)), 96);

    // Painted before they're found: the fallbacks, and nothing of the shell.
    size_t calls = shell_calls_on_this_thread();
    const picture list_before = print_client(list_hwnd);
    print_client(tree_hwnd);
    expect(shell_calls_on_this_thread() == calls, "painting before they're found enters the shell nowhere");
    expect(request_path_icon(exe, px) == path_icon_state::pending &&
               request_path_icon(drive, px) == path_icon_state::pending,
           "the tree asked for its rows' icons as it was made, the list as it painted: pending");
    const std::vector<uint32_t> tree_before = tree_image(tree_hwnd, 1);

    // Found in the background; told here, on this thread.
    hold_path_icon_finders(false);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto all_found = [&]
    {
        for (const std::string& p : {drive, narrow(windir), exe, regedit})
            if (request_path_icon(p, px) == path_icon_state::pending) return false;
        return true;
    };
    while (!all_found() && std::chrono::steady_clock::now() < until)
    {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        pump();
    }
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
    pump();  // the last ones told
    expect(all_found(), "all found");
    expect(path_icon_pixels(exe, px) && path_icon_pixels(drive, px), "explorer.exe's and the drive's are there");

    calls = shell_calls_on_this_thread();
    const picture list_after = print_client(list_hwnd);
    print_client(tree_hwnd);
    expect(shell_calls_on_this_thread() == calls, "painting after they're found enters the shell nowhere");
    // The first rows' icons, at the left (the list may have been laid out
    // again meanwhile, at another width).
    const int corner = MulDiv(40, int(GetDpiForWindow(top)), 96);
    const std::vector<uint32_t> icons_before = list_before.corner(corner, corner * 2),
                                icons_after = list_after.corner(corner, corner * 2);
    expect(!icons_before.empty() && icons_before.size() == icons_after.size() && icons_before != icons_after,
           "the list's rows show their own icons now, in place");
    const std::vector<uint32_t> tree_after = tree_image(tree_hwnd, 1);
    if (_wcsicmp(location_of(windir.substr(0, 3)).c_str(), generic_drive_location().c_str()) != 0)
        expect(!tree_before.empty() && tree_before != tree_after,
               "the tree's drive row shows the drive's own icon now, not the generic one, in place");
    else
        std::printf("     (the shell keeps the drive's icon where the generic one is: not compared)\n");

    win->close();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
