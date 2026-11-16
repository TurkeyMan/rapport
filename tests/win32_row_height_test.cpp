// An app's row height for a list or a tree (list::row_height,
// tree::row_height): rows = round(row_height x dpi/96 x the text size),
// never shorter than the font's line and 2 px, overriding the density's
// rows, a compact list's, a file_view's (Explorer's) and a navigator's (its
// indent kept), in the fluent look and the native (--native). A list or tree
// without one keeps its rows as before: the density's where the controls
// are fluent, the control's own native.
//
// Never-shown windows: each control's rows at the window's DPI and (static
// builds) at 96, 144 and 192 dpi through the DPI test hook, at a 150% text
// size and at a huge one (the font's line then: no clipping) through the
// text size hooks, and back.
//
// --app: the app's row height for all of them (look_options::row_height,
// 20): a plain list, a file_view, a tree, a navigator and a sidebar's rows
// 20 at 96 dpi and 30 at 144 (static builds: the DPI hook), a list's own
// row_height (28) over it; a sidebar at its natural size every row whole.
// --app-large: the app's 64, at 192 dpi and a 225% text size (static
// builds: the hooks): a sidebar's rows capped at a list box's 255 device px,
// and a sidebar at its natural size every row whole.
//   rapport-win32-row-height-test [--native] [--app | --app-large]
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which they need.
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(                                                                                   \
    linker,                                                                                        \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
namespace rap::win32
{
void set_band_density_for_tests(int compact);  // win32_band_field.h
void set_dpi_for_tests(UINT dpi);              // win32_common.h
void set_text_scale_for_tests(float scale);    // win32_common.h
void set_message_font_size_for_tests(float dip);
}  // namespace rap::win32
#endif

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
}
void note(const std::string& what)
{
    std::printf("note %s\n", what.c_str());
    std::fflush(stdout);
}
std::string str(long v)
{
    return std::to_string(v);
}

void pump(int rounds = 20)
{
    for (int round = 0; round < rounds; ++round)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

std::string env(const char* name)
{
    char v[256]{};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    return n > 0 && n < sizeof v ? std::string(v, n) : std::string();
}

int px(float logical, UINT dpi)
{
    return int(std::lround(logical * float(dpi) / 96.f));
}

// The window's controls of class `cls`, left to right.
std::vector<HWND> all_of(HWND top, const wchar_t* cls)
{
    struct find
    {
        const wchar_t* cls;
        std::vector<HWND> found;
    } f{cls, {}};
    EnumChildWindows(
        top,
        [](HWND h, LPARAM lp) -> BOOL
        {
            auto* f = reinterpret_cast<find*>(lp);
            wchar_t name[64]{};
            GetClassNameW(h, name, 64);
            if (!_wcsicmp(name, f->cls)) f->found.push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&f));
    std::sort(f.found.begin(), f.found.end(),
              [](HWND a, HWND b)
              {
                  RECT ra{}, rb{};
                  GetWindowRect(a, &ra);
                  GetWindowRect(b, &rb);
                  return ra.left < rb.left;
              });
    return f.found;
}

class rows_model : public rap::list_model
{
public:
    size_t row_count() const override { return 3; }
    std::string text(size_t row, size_t col) const override
    {
        return (col == 0 ? "Name " : "Note ") + std::to_string(row);
    }
};
rows_model g_model;

int list_row(HWND h)
{
    RECT r{};
    return ListView_GetItemRect(h, 0, &r, LVIR_BOUNDS) ? int(r.bottom - r.top) : -1;
}
int tree_row(HWND h)
{
    return int(TreeView_GetItemHeight(h));
}
// The line of the font control `h` has, px.
int font_line(HWND h)
{
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return int(tm.tmHeight);
}

// The row an app's 20 px asks for at `dpi` and text size `scale`, for
// control `h`: its font's line and 2 px at least; a tree's even, rounded
// up; a list's never under the list view's own least, its font's line and
// 4 (a huge text size: taller than asked, never clipped).
int want(HWND h, UINT dpi, float scale, bool tree, float logical = 20)
{
    const int row = std::max(int(std::lround(logical * float(dpi) / 96.f * scale)), font_line(h) + px(2, dpi));
    return tree ? row + (row & 1) : std::max(row, font_line(h) + 4);
}

struct controls
{
    HWND list = nullptr, compact_list = nullptr, file_list = nullptr, plain_list = nullptr;
    HWND tree = nullptr, navigator = nullptr, plain_tree = nullptr;
    bool all() const { return list && compact_list && file_list && plain_list && tree && navigator && plain_tree; }
};

// The app's rows at `dpi` and text size `scale`; the plain ones as they'd be
// without one (`plain_list`, `plain_tree`: what they were, or -1 not to
// check).
void check_rows(const controls& c, UINT dpi, float scale, int plain_list, int plain_tree, const std::string& at)
{
    const struct
    {
        const char* what;
        HWND h;
        bool tree;
    } fixed[] = {{"a list", c.list, false},
                 {"a compact list", c.compact_list, false},
                 {"a file_view", c.file_list, false},
                 {"a tree", c.tree, true},
                 {"a navigator", c.navigator, true}};
    for (const auto& f : fixed)
    {
        const int now = f.tree ? tree_row(f.h) : list_row(f.h);
        const int w = want(f.h, dpi, scale, f.tree);
        expect(now == w, std::string(f.what) + "'s rows the app's, " + str(w) + at + ": " + str(now));
    }
    if (plain_list >= 0)
        expect(list_row(c.plain_list) == plain_list,
               "a list without one, its rows as before, " + str(plain_list) + at + ": " + str(list_row(c.plain_list)));
    if (plain_tree >= 0)
        expect(tree_row(c.plain_tree) == plain_tree,
               "a tree without one, its rows as before, " + str(plain_tree) + at + ": " + str(tree_row(c.plain_tree)));
}

#ifdef RAPPORT_TEST_BUILTIN_WIN32
// To `to` dpi through the DPI hook, as Windows sends WM_DPICHANGED.
void to_dpi(HWND top, UINT from, UINT to, UINT own)
{
    rap::win32::set_dpi_for_tests(to == own ? 0 : to);
    RECT wr{};
    GetWindowRect(top, &wr);
    RECT suggested{wr.left, wr.top, wr.left + MulDiv(wr.right - wr.left, int(to), int(from)),
                   wr.top + MulDiv(wr.bottom - wr.top, int(to), int(from))};
    SendMessageW(top, WM_DPICHANGED, MAKEWPARAM(to, to), reinterpret_cast<LPARAM>(&suggested));
    pump();
}
// A text size through the text size hooks, as Windows announces it.
void text_size(HWND top, float scale, float message_dip)
{
    rap::win32::set_text_scale_for_tests(scale);
    rap::win32::set_message_font_size_for_tests(message_dip);
    SendMessageW(top, WM_SETTINGCHANGE, SPI_SETNONCLIENTMETRICS, reinterpret_cast<LPARAM>(L"WindowMetrics"));
    pump();
}
#endif

// --app: the app's 20 (look_options::row_height) for every list, tree and
// sidebar, a list's own 28 over it, at `dpi`.
void check_app(HWND top, UINT dpi, const std::string& at)
{
    const std::vector<HWND> lists = all_of(top, WC_LISTVIEWW), trees = all_of(top, WC_TREEVIEWW),
                            boxes = all_of(top, L"ListBox");
    expect(lists.size() == 3 && trees.size() == 2 && boxes.size() == 1,
           "a list with its own row height, a plain list, a file_view, a tree, a navigator and a sidebar");
    if (lists.size() != 3 || trees.size() != 2 || boxes.size() != 1) return;
    const int own = list_row(lists[0]), own_want = want(lists[0], dpi, 1.f, false, 28);
    expect(own == own_want, "a list's own row_height over the app's, " + str(own_want) + at + ": " + str(own));
    const struct
    {
        const char* what;
        HWND h;
        int now;
        bool tree;
    } rows[] = {{"a plain list", lists[1], list_row(lists[1]), false},
                {"a file_view", lists[2], list_row(lists[2]), false},
                {"a tree", trees[0], tree_row(trees[0]), true},
                {"a navigator", trees[1], tree_row(trees[1]), true},
                {"a sidebar", boxes[0], int(SendMessageW(boxes[0], LB_GETITEMHEIGHT, 0, 0)), false}};
    for (const auto& r : rows)
    {
        // A list box's rows are what's set: the clamp alone, no list view's
        // least.
        const int w = r.h == boxes[0] ? std::max(px(20, dpi), font_line(r.h) + px(2, dpi)) : want(r.h, dpi, 1.f, r.tree);
        expect(r.now == w, std::string(r.what) + "'s rows the app's, " + str(w) + at + ": " + str(r.now));
    }
}

// A sidebar of four pages in a window of its natural size: every row whole.
void sidebar_fits(rap::ui& ui)
{
    using namespace rap::layout;
    auto win = ui.create(window{"Row height sidebar",
                                {tabs{{"General", label{"one"}},
                                      {"Appearance", label{"two"}},
                                      {"Fonts", label{"three"}},
                                      {"Advanced", label{"four"}}}
                                     .sidebar()}});
    pump();
    HWND top = FindWindowW(nullptr, L"Row height sidebar");
    const std::vector<HWND> boxes = top ? all_of(top, L"ListBox") : std::vector<HWND>{};
    expect(boxes.size() == 1, "a sidebar in a window of its natural size");
    if (boxes.size() != 1) return;
    RECT client{};
    GetClientRect(boxes[0], &client);
    for (int i = 0; i < 4; ++i)
    {
        RECT r{};
        SendMessageW(boxes[0], LB_GETITEMRECT, i, reinterpret_cast<LPARAM>(&r));
        expect(r.top >= client.top && r.bottom <= client.bottom,
               "its row " + str(i) + " whole, " + str(r.top) + ".." + str(r.bottom) + " in 0.." + str(client.bottom));
    }
}

// --app-large: the app's 64 at 192 dpi and 225% text (288 asked for): a
// sidebar's rows 255, a list box's most, and at its natural size whole.
int run_app_large(rap::ui& ui)
{
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    rap::win32::set_dpi_for_tests(192);
    rap::win32::set_text_scale_for_tests(2.25f);
    rap::win32::set_message_font_size_for_tests(27.f);
    using namespace rap::layout;
    auto win = ui.create(window{"Row height large",
                                {tabs{{"General", label{"one"}},
                                      {"Appearance", label{"two"}},
                                      {"Fonts", label{"three"}}}
                                     .sidebar()}});
    pump();
    HWND top = FindWindowW(nullptr, L"Row height large");
    const std::vector<HWND> boxes = top ? all_of(top, L"ListBox") : std::vector<HWND>{};
    expect(boxes.size() == 1, "a sidebar in a window of its natural size, 192 dpi, 225% text");
    if (boxes.size() == 1)
    {
        const int row = int(SendMessageW(boxes[0], LB_GETITEMHEIGHT, 0, 0));
        expect(row == 255, "its rows a list box's most, 255 (288 asked for): " + str(row));
        RECT client{};
        GetClientRect(boxes[0], &client);
        for (int i = 0; i < 3; ++i)
        {
            RECT r{};
            SendMessageW(boxes[0], LB_GETITEMRECT, i, reinterpret_cast<LPARAM>(&r));
            expect(r.top >= client.top && r.bottom <= client.bottom,
                   "its row " + str(i) + " whole, " + str(r.top) + ".." + str(r.bottom) + " in 0.." +
                       str(client.bottom));
        }
    }
    rap::win32::set_dpi_for_tests(0);
    rap::win32::set_text_scale_for_tests(1.f);
    rap::win32::set_message_font_size_for_tests(0);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
#else
    (void)ui;
    note("skipped: the DPI and text size hooks are static builds'");
    return 77;
#endif
}

int run_app(rap::ui& ui)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::vector<rap::column> cols{{"Name", 120_px}, {"Notes", 120_px}};
    const std::vector<rap::tree_item> items{{"Alpha", "a"}, {"Beta", "b"}};
    auto win = ui.create(
        window{"Row height app",
               {hbox{list{cols, &g_model}.row_height(28).stretch(), list{cols, &g_model}.stretch(),
                     list{cols, &g_model}.file_view().stretch(), tree{items}.stretch(),
                     tree{items}.navigator().stretch(),
                     tabs{{"One", label{"one"}}, {"Two", label{"two"}}}.sidebar().stretch()}}}
            .size(1300_px, 500_px));
    pump();
    HWND top = FindWindowW(nullptr, L"Row height app");
    expect(top != nullptr, "the window, never shown");
    if (!top) return 1;
    const UINT dpi = GetDpiForWindow(top);
    check_app(top, dpi, ", " + str(long(dpi)) + " dpi");
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    UINT was = dpi;
    for (const UINT to : {96u, 144u})
    {
        to_dpi(top, was, to, dpi);
        was = to;
        check_app(top, to, ", " + str(long(to)) + " dpi (the DPI hook)");
    }
    to_dpi(top, was, dpi, dpi);
#endif
    sidebar_fits(ui);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    bool native = false, app = false, app_large = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--native") native = true;
        if (std::string(argv[i]) == "--app") app = true;
        if (std::string(argv[i]) == "--app-large") app_large = true;
    }
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    rap::win32::set_text_scale_for_tests(1.f);
    rap::win32::set_band_density_for_tests(0);
#endif
    rap::load_options options{.backend = "win32"};
    if (native) options.look.controls = rap::control_style::native;
    if (app) options.look.row_height = 20;
    if (app_large) options.look.row_height = 64;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 2;
    if (app) return run_app(**ui);
    if (app_large) return run_app_large(**ui);
    std::printf("%s\n", native ? "native" : "the platform's look");

    using namespace rap::layout;
    using namespace rap::literals;
    const std::vector<rap::column> cols{{"Name", 120_px}, {"Notes", 120_px}};
    const std::vector<rap::tree_item> items{{"Alpha", "a"}, {"Beta", "b"}};
    const std::string title = "Row height";
    auto win = (*ui)->create(
        window{title,
               {vbox{hbox{list{cols, &g_model}.row_height(20).stretch(),
                          list{cols, &g_model}.compact().row_height(20).stretch(),
                          list{cols, &g_model}.file_view().row_height(20).stretch(),
                          list{cols, &g_model}.stretch()}
                         .stretch(),
                     hbox{tree{items}.row_height(20).stretch(), tree{items}.navigator().row_height(20).stretch(),
                          tree{items}.stretch()}
                         .stretch()}}}
            .size(1100_px, 600_px));
    pump();
    HWND top = FindWindowW(nullptr, L"Row height");
    expect(top != nullptr, "the window, never shown");
    if (!top) return 1;
    const std::vector<HWND> lists = all_of(top, WC_LISTVIEWW), trees = all_of(top, WC_TREEVIEWW);
    controls c;
    if (lists.size() == 4) c = {lists[0], lists[1], lists[2], lists[3], nullptr, nullptr, nullptr};
    if (trees.size() == 3)
    {
        c.tree = trees[0];
        c.navigator = trees[1];
        c.plain_tree = trees[2];
    }
    expect(c.all(), "its four lists and three trees");
    if (!c.all()) return 1;
    const UINT dpi = GetDpiForWindow(top);
    // The plain ones as they are: a fluent list the density's 32 px (standard),
    // a native one the list view's own; their rows then must stay so.
    const int plain_list = list_row(c.plain_list), plain_tree = tree_row(c.plain_tree);
    if (!native)
        note("a plain list's rows " + str(plain_list) + ", a plain tree's " + str(plain_tree) + " at " +
             str(long(dpi)) + " dpi");
    check_rows(c, dpi, 1.f, plain_list, plain_tree, ", " + str(long(dpi)) + " dpi");
    // The navigator's indent kept (Explorer's 8 where the controls are fluent
    // on Windows 11).
    note("a navigator's indent " + str(long(TreeView_GetIndent(c.navigator))));
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    UINT was = dpi;
    for (const UINT to : {96u, 144u, 192u})
    {
        to_dpi(top, was, to, dpi);
        was = to;
        // At exactly 96, 144 and 192: 20, 30 and 40 (the font's line well under).
        check_rows(c, to, 1.f, -1, -1, ", " + str(long(to)) + " dpi (the DPI hook)");
        expect(list_row(c.list) == px(20, to), "a list's rows " + str(px(20, to)) + " at " + str(long(to)) +
                                                   " dpi: " + str(list_row(c.list)));
    }
    to_dpi(top, was, 96, dpi);
    // 150% text: 30 at 96 dpi.
    text_size(top, 1.5f, 18.f);
    check_rows(c, 96, 1.5f, -1, -1, ", 150% text, 96 dpi");
    expect(list_row(c.list) == 30, "a list's rows 30 at 150% text: " + str(list_row(c.list)));
    // A huge text size: the font's line and 2 px, the text never clipped.
    text_size(top, 2.25f, 60.f);
    check_rows(c, 96, 2.25f, -1, -1, ", a huge text size, 96 dpi");
    expect(list_row(c.list) >= font_line(c.list) + 2,
           "a list's rows the font's line and 2 px at least: " + str(list_row(c.list)) + " for a line of " +
               str(font_line(c.list)));
    text_size(top, 1.f, 12.f);
    to_dpi(top, 96, dpi, dpi);
    check_rows(c, dpi, 1.f, plain_list, plain_tree, ", " + str(long(dpi)) + " dpi again");
    // The density changed (compact): the app's rows stay its.
    rap::win32::set_band_density_for_tests(1);
    SendMessageW(top, WM_SETTINGCHANGE, SPI_SETNONCLIENTMETRICS, reinterpret_cast<LPARAM>(L"WindowMetrics"));
    pump();
    check_rows(c, dpi, 1.f, -1, -1, ", the compact density");
    rap::win32::set_band_density_for_tests(-1);
#endif
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
