// tree_item::separator_before on Win32: a divider-coloured line along the
// top of the item's row, from where its icon (else its label) starts; none
// over the first row or right under a parent; none once turned off. The
// arrow keys go item to item as ever. Printed (WM_PRINTCLIENT), not captured.
//   rapport-win32-tree-separator-test
#include <rapport/rapport.h>
#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

rap::tree_item item(std::string id, bool line, std::vector<rap::tree_item> children = {})
{
    rap::tree_item t;
    t.label = id;
    t.id = std::move(id);
    t.separator_before = line;
    t.children = std::move(children);
    t.expanded = !t.children.empty();
    return t;
}

HWND find_tree()
{
    HWND found = nullptr;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND top, LPARAM p) -> BOOL
        {
            EnumChildWindows(
                top,
                [](HWND c, LPARAM q) -> BOOL
                {
                    wchar_t name[32] = {};
                    GetClassNameW(c, name, 32);
                    if (!_wcsicmp(name, WC_TREEVIEWW)) *reinterpret_cast<HWND*>(q) = c;
                    return TRUE;
                },
                p);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

HTREEITEM item_named(HWND tree, const wchar_t* label)
{
    for (HTREEITEM h = TreeView_GetRoot(tree); h; h = TreeView_GetNextVisible(tree, h))
    {
        wchar_t text[64] = {};
        TVITEMW it{};
        it.mask = TVIF_TEXT;
        it.hItem = h;
        it.pszText = text;
        it.cchTextMax = 64;
        // Wide by name: this test isn't built UNICODE, so TreeView_GetItem
        // would ask for (and write) ANSI text.
        if (SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&it)) && !wcscmp(text, label)) return h;
    }
    return nullptr;
}

// The tree as it paints, printed into a bitmap.
struct printed
{
    explicit printed(HWND tree)
    {
        RECT r{};
        GetClientRect(tree, &r);
        w = r.right;
        h = r.bottom;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        old = SelectObject(dc, bmp);
        SendMessageW(tree, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
        GdiFlush();
    }
    ~printed()
    {
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    COLORREF at(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return CLR_INVALID;
        const uint32_t p = bits[size_t(y) * size_t(w) + size_t(x)];
        return RGB((p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff);
    }
    int w = 0, h = 0;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* bits = nullptr;
};

// Where a line for `h` starts (its icon, else its label), and how much of
// its row's top edge from there is the line's colour.
struct line_check
{
    bool found = false;  // the item's in the tree, showing
    bool measured = false;  // and its row and label rects came back
    int left = 0;
    double covered = 0;
    bool before_left = false;  // the colour just before where it starts
    RECT row{}, text{};
};

// The line's colour, theme_color::divider: in light, COLOR_3DLIGHT
// (COLOR_WINDOWTEXT in high contrast); in dark, rapport's own dark palette's,
// which the test can't name, so set from b's line (and checked against its
// row's background) by main.
COLORREF g_ink = CLR_INVALID;

// As rapport decides it: RAPPORT_THEME=dark|light first, else the desktop's.
bool light_theme()
{
    if (const char* forced = std::getenv("RAPPORT_THEME"))
    {
        if (!std::strcmp(forced, "dark")) return false;
        if (!std::strcmp(forced, "light")) return true;
    }
    DWORD light = 1, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light != 0;
}

bool high_contrast()
{
    HIGHCONTRASTW hc{sizeof hc};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

line_check line_of(HWND tree, const printed& p, HTREEITEM h, bool has_icon)
{
    line_check c;
    c.found = h != nullptr;
    if (!c.found) return c;
    RECT& row = c.row;
    RECT& text = c.text;
    c.measured = TreeView_GetItemRect(tree, h, &row, FALSE) && TreeView_GetItemRect(tree, h, &text, TRUE);
    if (!c.measured) return c;
    const COLORREF ink = g_ink;
    const UINT dpi = GetDpiForWindow(tree);
    int cx = 0, cy = 0;
    HIMAGELIST icons = TreeView_GetImageList(tree, TVSIL_NORMAL);
    if (has_icon && icons) ImageList_GetIconSize(icons, &cx, &cy);
    c.left = has_icon ? text.left - cx - MulDiv(3, int(dpi), 96) : text.left;
    int on = 0, span = 0;
    for (int x = c.left; x < row.right; ++x, ++span) on += p.at(x, row.top) == ink;
    c.covered = span ? double(on) / span : 0;
    c.before_left = p.at(c.left - 2, row.top) == ink;
    return c;
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
    rap::tree_item disk = item("d", true);
    disk.icon = rap::icon::named("drive-harddisk");
    rap::tree* t = nullptr;
    using namespace rap::layout;
    using rap::operator""_ch;
    using rap::operator""_lines;
    auto win = (*ui)->create(window{
        "t", {tree{{item("a", false), item("b", true), item("c", true, {item("c1", true), item("c2", true)}), disk}}
                  .bind(t)}}
                             .size(40_ch, 20_lines));
    win->show();
    HWND tree = find_tree();
    expect(tree != nullptr, "the tree is there");
    if (!tree) return 1;

    if (high_contrast())
        g_ink = GetSysColor(COLOR_WINDOWTEXT);
    else if (light_theme())
        g_ink = GetSysColor(COLOR_3DLIGHT);
    else
    {
        printed p(tree);
        RECT row{}, text{};
        const HTREEITEM b_item = item_named(tree, L"b");
        if (b_item && TreeView_GetItemRect(tree, b_item, &row, FALSE) && TreeView_GetItemRect(tree, b_item, &text, TRUE))
        {
            g_ink = p.at(text.left, row.top);
            expect(g_ink != p.at(text.left, row.top + 2), "dark: b's line stands out from its row");
        }
    }
    {
        printed p(tree);
        auto has = [&](const wchar_t* name, bool icon = false)
        { return line_of(tree, p, item_named(tree, name), icon); };
        const line_check a = has(L"a"), b = has(L"b"), c = has(L"c"), c1 = has(L"c1"), c2 = has(L"c2"),
                         d = has(L"d", true);
        // Where everything was, for when a check below fails.
        for (const auto& [name, l] : {std::pair{"a", a}, {"b", b}, {"c", c}, {"c1", c1}, {"c2", c2}, {"d", d}})
            std::printf("     %s: found %d measured %d row (%ld,%ld)-(%ld,%ld) text (%ld,%ld)-(%ld,%ld) left %d covered %.2f\n",
                        name, l.found, l.measured, l.row.left, l.row.top, l.row.right, l.row.bottom, l.text.left,
                        l.text.top, l.text.right, l.text.bottom, l.left, l.covered);
        const HTREEITEM c_item = item_named(tree, L"c");
        expect(c_item && (TreeView_GetItemState(tree, c_item, TVIS_EXPANDED) & TVIS_EXPANDED), "c is open");
        bool all = true;
        for (const line_check& l : {a, b, c, c1, c2, d}) all &= l.found && l.measured;
        expect(all, "every item is showing, its rects measured");
        expect(a.covered == 0, "no line over the first row");
        expect(b.covered > 0.95 && !b.before_left, "a line over b, from its label to the row's end");
        expect(c.covered > 0.95, "a line over c");
        expect(c1.covered == 0, "none over c1, right under its parent");
        expect(c2.covered > 0.95 && !c2.before_left, "a line over c2, from c2's own label (its level's inset)");
        expect(c2.left > b.left, "c2's line starts further in than b's");
        expect(d.covered > 0.95 && !d.before_left, "a line over d, from its icon");
    }

    t->set_separator_before("b", false);
    t->set_separator_before("a", true);
    UpdateWindow(tree);
    {
        printed p(tree);
        expect(line_of(tree, p, item_named(tree, L"b"), false).covered == 0, "b's line turned off");
        expect(line_of(tree, p, item_named(tree, L"a"), false).covered == 0, "a's turned on, still none: first row");
        expect(line_of(tree, p, item_named(tree, L"c"), false).covered > 0.95, "c's unchanged");
    }

    // The keys, item to item.
    t->select("a");
    for (const char* want : {"b", "c", "c1", "c2", "d"})
    {
        SendMessageW(tree, WM_KEYDOWN, VK_DOWN, 0);
        SendMessageW(tree, WM_KEYUP, VK_DOWN, 0);
        expect(t->selected() == std::optional<std::string>(want), std::string("Down onto ") + want);
    }

    win->close();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
