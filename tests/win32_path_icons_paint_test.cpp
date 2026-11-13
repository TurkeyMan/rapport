// Paths' own icons (icon::for_path) on a tree's rows and a list's, in a
// window that's never shown, painted into memory (WM_PRINTCLIENT): painting
// never enters the shell, before the icons are found or after, and once
// they're found both show them in place of their fallbacks. So do the
// controls that keep an image rather than drawing as they paint: a button's
// and a menu button's image list, a label's icon, a toolbar's strip and the
// window's own icons, each made again once the path's icon is found, with
// nothing setting their icons again.
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

namespace rap::win32
{
void set_dpi_for_tests(UINT dpi);  // win32_common.h (which wants the backend's include paths)
}

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

// Image `i` of an image list, drawn over white.
std::vector<uint32_t> list_image(HIMAGELIST list, int i)
{
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

std::vector<uint32_t> tree_image(HWND tree, int i) { return list_image(TreeView_GetImageList(tree, TVSIL_NORMAL), i); }

// An icon handle, drawn over white at its own size.
std::vector<uint32_t> icon_image(HICON icon)
{
    ICONINFO ii{};
    if (!icon || !GetIconInfo(icon, &ii)) return {};
    BITMAP bm{};
    GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    const int cx = bm.bmWidth, cy = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
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
    DrawIconEx(dc, 0, 0, icon, cx, cy, 0, nullptr, DI_NORMAL);
    GdiFlush();
    std::vector<uint32_t> out(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + size_t(cx) * size_t(cy));
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return out;
}

// A child of `top` of class `cls` whose text starts with `text`.
HWND find_child_text(HWND top, const wchar_t* cls, const std::wstring& text)
{
    struct search
    {
        const wchar_t* cls;
        const std::wstring* text;
        HWND found;
    } s{cls, &text, nullptr};
    EnumChildWindows(
        top,
        [](HWND c, LPARAM q) -> BOOL
        {
            auto* s = reinterpret_cast<search*>(q);
            wchar_t name[32] = {}, t[128] = {};
            GetClassNameW(c, name, 32);
            GetWindowTextW(c, t, 128);
            if (!_wcsicmp(name, s->cls) && std::wstring(t).starts_with(*s->text) && !s->found) s->found = c;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&s));
    return s.found;
}

// A static showing an icon (a label's).
HWND find_icon_static(HWND top)
{
    HWND found = nullptr;
    EnumChildWindows(
        top,
        [](HWND c, LPARAM q) -> BOOL
        {
            wchar_t name[32] = {};
            GetClassNameW(c, name, 32);
            if (!_wcsicmp(name, L"Static") && (GetWindowLongW(c, GWL_STYLE) & SS_TYPEMASK) == SS_ICON)
            {
                *reinterpret_cast<HWND*>(q) = c;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// What each control that keeps an image keeps now: its handles, and their
// pixels.
struct retained
{
    HIMAGELIST button_list = nullptr, menu_button_list = nullptr, toolbar_list = nullptr;
    HICON label_icon = nullptr, window_small = nullptr, window_big = nullptr;
    std::vector<uint32_t> button, menu_button, label, toolbar, window_small_px, window_big_px;
};
HIMAGELIST button_list(HWND b)
{
    BUTTON_IMAGELIST bil{};
    SendMessageW(b, BCM_GETIMAGELIST, 0, reinterpret_cast<LPARAM>(&bil));
    return bil.himl;
}
retained read_retained(HWND top, HWND button, HWND menu_button, HWND label, HWND toolbar)
{
    retained r;
    r.button_list = button_list(button);
    r.menu_button_list = button_list(menu_button);
    r.toolbar_list = reinterpret_cast<HIMAGELIST>(SendMessageW(toolbar, TB_GETIMAGELIST, 0, 0));
    r.label_icon = reinterpret_cast<HICON>(SendMessageW(label, STM_GETICON, 0, 0));
    r.window_small = reinterpret_cast<HICON>(SendMessageW(top, WM_GETICON, ICON_SMALL, 0));
    r.window_big = reinterpret_cast<HICON>(SendMessageW(top, WM_GETICON, ICON_BIG, 0));
    r.button = list_image(r.button_list, 0);
    r.menu_button = list_image(r.menu_button_list, 0);
    r.toolbar = list_image(r.toolbar_list, 0);
    r.label = icon_image(r.label_icon);
    r.window_small_px = icon_image(r.window_small);
    r.window_big_px = icon_image(r.window_big);
    return r;
}
bool drawn(const std::vector<uint32_t>& px)
{
    return std::any_of(px.begin(), px.end(), [](uint32_t p) { return (p & 0xFFFFFF) != 0xFFFFFF; });
}

// A button, a menu button, a label and a toolbar button, each with `icon`.
rap::node kept_controls(const rap::icon& icon)
{
    using namespace rap::layout;
    return hbox{button("PathButton").icon(icon), menu_button("PathMenu").icon(icon).on_menu([] { return rap::menu{}; }),
                label("PathLabel").icon(icon), toolbar{tool_button{icon, "PathTool"}}};
}

void pump_for(int ms)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until)
    {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        pump();
    }
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
    // On the controls that keep an image: notepad's own, its fallback a
    // drawn name.
    const std::string notepad = narrow(windir + L"\\notepad.exe");
    const rap::icon kept = rap::icon::for_path(notepad, "document-save");

    // The finders held until the "before" is seen: a local path's icon can
    // be found within the tree's own making, before anything could look.
    hold_path_icon_finders(true);
    using namespace rap::layout;
    const wchar_t* title = L"rapport path icons paint test";
    auto win = (*ui)->create(window{"rapport path icons paint test",
                                    {vbox{tree{{c_drive, system}}.stretch(),
                                          list{{{.title = "Name", .stretch = true}}, &model}.stretch(),
                                          kept_controls(kept)}}}
                                 .size(60_ch, 30_lines));
    win->set_icon(kept);
    // A second window whose controls keep images of a path still pending,
    // gone before it's found: each took its listener with it, and the
    // answer, when it comes, touches nothing of theirs.
    {
        const size_t listeners = rap::win32::path_icon_listener_count();
        const std::string gone_path = narrow(windir + L"\\hh.exe");
        const rap::icon gone = rap::icon::for_path(gone_path, "help-browser");
        auto doomed = (*ui)->create(window{"rapport path icons paint test, gone", {kept_controls(gone)}}.size(40_ch, 6_lines));
        doomed->set_icon(gone);
        pump();
        expect(request_path_icon(gone_path, MulDiv(16, int(GetDpiForWindow(GetDesktopWindow())), 96)) ==
                   path_icon_state::pending,
               "a window's path icon is pending as it goes");
        expect(rap::win32::path_icon_listener_count() > listeners, "its controls listen meanwhile");
        doomed->close();
        doomed.reset();
        pump();
        expect(rap::win32::path_icon_listener_count() == listeners, "gone, they listen no more");
        expect(!FindWindowW(L"rapportWindow", L"rapport path icons paint test, gone"), "and the window's gone");
    }

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

    HWND button_hwnd = find_child_text(top, L"Button", L"PathButton"),
         menu_button_hwnd = find_child_text(top, L"Button", L"PathMenu"), label_icon = find_icon_static(top),
         toolbar_hwnd = find_child(top, TOOLBARCLASSNAMEW);
    expect(button_hwnd && menu_button_hwnd && label_icon && toolbar_hwnd,
           "the button, menu button, label's icon and toolbar are there");
    if (!button_hwnd || !menu_button_hwnd || !label_icon || !toolbar_hwnd) return 1;
    expect(request_path_icon(notepad, px) == path_icon_state::pending, "notepad's is pending");
    calls = shell_calls_on_this_thread();
    for (HWND h : {button_hwnd, menu_button_hwnd, label_icon, toolbar_hwnd}) print_client(h);
    expect(shell_calls_on_this_thread() == calls, "painting the controls that keep images enters the shell nowhere");
    const retained kept_before = read_retained(top, button_hwnd, menu_button_hwnd, label_icon, toolbar_hwnd);
    expect(drawn(kept_before.button) && drawn(kept_before.menu_button) && drawn(kept_before.label) &&
               drawn(kept_before.toolbar) && drawn(kept_before.window_small_px) && drawn(kept_before.window_big_px),
           "each keeps its fallback meanwhile");

    // Found in the background; told here, on this thread.
    hold_path_icon_finders(false);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto all_found = [&]
    {
        for (const std::string& p : {drive, narrow(windir), exe, regedit, notepad})
            if (request_path_icon(p, px) == path_icon_state::pending) return false;
        return request_path_icon(notepad, GetSystemMetrics(SM_CXICON)) != path_icon_state::pending &&
               request_path_icon(notepad, GetSystemMetrics(SM_CXSMICON)) != path_icon_state::pending;
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

    // Nothing set their icons again: each was made again when told.
    calls = shell_calls_on_this_thread();
    for (HWND h : {button_hwnd, menu_button_hwnd, label_icon, toolbar_hwnd}) print_client(h);
    expect(shell_calls_on_this_thread() == calls, "painting them once found enters the shell nowhere");
    expect(path_icon_pixels(notepad, px) != nullptr, "notepad's own icon is there");
    const retained kept_after = read_retained(top, button_hwnd, menu_button_hwnd, label_icon, toolbar_hwnd);
    auto replaced = [](const std::vector<uint32_t>& before, const std::vector<uint32_t>& after)
    { return drawn(after) && before.size() == after.size() && before != after; };
    expect(kept_after.button_list != kept_before.button_list && replaced(kept_before.button, kept_after.button),
           "the button's image list is made again, with notepad's icon");
    expect(kept_after.menu_button_list != kept_before.menu_button_list &&
               replaced(kept_before.menu_button, kept_after.menu_button),
           "the menu button's image list is made again, with notepad's icon");
    expect(kept_after.label_icon != kept_before.label_icon && replaced(kept_before.label, kept_after.label),
           "the label's icon is made again, notepad's");
    expect(replaced(kept_before.toolbar, kept_after.toolbar), "the toolbar's strip is made again, with notepad's icon");
    expect(kept_after.window_small != kept_before.window_small && kept_after.window_big != kept_before.window_big &&
               replaced(kept_before.window_small_px, kept_after.window_small_px) &&
               replaced(kept_before.window_big_px, kept_after.window_big_px),
           "the window's icons are made again, notepad's");

    // A new DPI, through WM_DPICHANGED as Windows sends it (the window taken
    // to be at that DPI, never shown): the label's icon and the toolbar's
    // strip are made at the new size, the fallback while it's being found
    // there, then notepad's, in place.
    hold_path_icon_finders(true);
    const UINT old_dpi = GetDpiForWindow(top), new_dpi = old_dpi == 144 ? 192 : 144;
    const int new_px = MulDiv(16, int(new_dpi), 96);
    rap::win32::set_dpi_for_tests(new_dpi);
    {
        RECT wr{};
        GetWindowRect(top, &wr);
        RECT suggested{wr.left, wr.top, wr.left + MulDiv(wr.right - wr.left, int(new_dpi), int(old_dpi)),
                       wr.top + MulDiv(wr.bottom - wr.top, int(new_dpi), int(old_dpi))};
        SendMessageW(top, WM_DPICHANGED, MAKEWPARAM(new_dpi, new_dpi), reinterpret_cast<LPARAM>(&suggested));
    }
    pump_for(500);  // laid out again at it
    auto icon_px = [](HIMAGELIST l)
    {
        int cx = 0, cy = 0;
        return l && ImageList_GetIconSize(l, &cx, &cy) ? cx : 0;
    };
    const retained dpi_before = read_retained(top, button_hwnd, menu_button_hwnd, label_icon, toolbar_hwnd);
    expect(request_path_icon(notepad, new_px) == path_icon_state::pending, "asked for again at the new size: pending");
    expect(dpi_before.label.size() == size_t(new_px) * size_t(new_px) && drawn(dpi_before.label) &&
               icon_px(dpi_before.toolbar_list) == new_px && drawn(dpi_before.toolbar),
           "the label's icon and the toolbar's strip are made at the new size meanwhile");
    hold_path_icon_finders(false);
    const auto dpi_until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (request_path_icon(notepad, new_px) == path_icon_state::pending && std::chrono::steady_clock::now() < dpi_until)
        pump_for(50);
    pump_for(100);  // told
    calls = shell_calls_on_this_thread();
    for (HWND h : {label_icon, toolbar_hwnd}) print_client(h);
    expect(shell_calls_on_this_thread() == calls, "painting at the new size enters the shell nowhere");
    const retained dpi_after = read_retained(top, button_hwnd, menu_button_hwnd, label_icon, toolbar_hwnd);
    expect(path_icon_pixels(notepad, new_px) != nullptr, "notepad's is found at the new size");
    expect(dpi_after.label_icon != dpi_before.label_icon && replaced(dpi_before.label, dpi_after.label),
           "the label's icon is made again at the new size, notepad's");
    expect(icon_px(dpi_after.toolbar_list) == new_px && replaced(dpi_before.toolbar, dpi_after.toolbar),
           "the toolbar's strip is made again at the new size, with notepad's icon");
    rap::win32::set_dpi_for_tests(0);

    win->close();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
