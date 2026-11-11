// A tree item's drive icon from Windows, and again at the new size after the
// window moves to 150% (WM_DPICHANGED).
//   rapport-win32-shell-icons-dpi-test
#include <rapport/rapport.h>
#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <string>
#include <vector>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}

HWND find_child(const wchar_t* cls)
{
    struct search
    {
        const wchar_t* cls;
        HWND found;
    } s{cls, nullptr};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND top, LPARAM p) -> BOOL
        {
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
                p);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&s));
    return s.found;
}

// The image list's size, and how many of image 1's pixels show (image 0 is
// the tree's blank).
std::pair<int, size_t> image(HWND tree)
{
    HIMAGELIST list = TreeView_GetImageList(tree, TVSIL_NORMAL);
    int cx = 0, cy = 0;
    if (!list || !ImageList_GetIconSize(list, &cx, &cy) || ImageList_GetImageCount(list) < 2) return {0, 0};
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
    // Drawn over white: what changes is the icon.
    RECT r{0, 0, cx, cy};
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    ImageList_Draw(list, 1, dc, 0, 0, ILD_NORMAL);
    GdiFlush();
    size_t shown = 0;
    for (int i = 0; i < cx * cy; ++i) shown += (static_cast<uint32_t*>(bits)[i] & 0xffffff) != 0xffffff;
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return {cx, shown};
}

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::printf("FAIL %s\n", ui.error().c_str());
        return 1;
    }
    rap::tree_item drive;
    drive.label = drive.id = "C:";
    drive.icon = rap::icon::named("drive-harddisk");
    using namespace rap::layout;
    auto win = (*ui)->create(window{"t", {tree{{drive}}}});
    win->show();
    HWND tree_hwnd = find_child(WC_TREEVIEWW);
    expect(tree_hwnd != nullptr, "the tree is there");
    if (!tree_hwnd) return 1;
    HWND top = GetAncestor(tree_hwnd, GA_ROOT);
    const UINT dpi = GetDpiForWindow(top);

    const auto [size, shown] = image(tree_hwnd);
    expect(size == MulDiv(16, int(dpi), 96), "the icon is the small-icon size for the window's DPI");
    expect(shown > size_t(size * size) / 8, "and shows the drive");

    // A DPI change. One sent here can't change GetDpiForWindow, which the
    // controls take their scale from (on a real move to another monitor the
    // window has the new DPI by the time this comes), so what's checked is
    // that the icons are made again, at the window's DPI, and still drawn;
    // the lookup at other sizes is win32_shell_icons_test's.
    HIMAGELIST before = TreeView_GetImageList(tree_hwnd, TVSIL_NORMAL);
    const UINT to = dpi == 144 ? 120 : 144;
    RECT r{};
    GetWindowRect(top, &r);
    SendMessageW(top, WM_DPICHANGED, MAKEWPARAM(to, to), reinterpret_cast<LPARAM>(&r));
    expect(TreeView_GetImageList(tree_hwnd, TVSIL_NORMAL) != before, "WM_DPICHANGED makes the icons again");
    const auto [after, shown_after] = image(tree_hwnd);
    expect(after == MulDiv(16, int(GetDpiForWindow(top)), 96), "at the window's DPI");
    expect(shown_after > size_t(after * after) / 8, "and still the drive");

    win->close();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
