// Lists', trees' and tabs' rows where rapport's controls are fluent
// (look_options::controls, density): the density's rows beside its 32 px
// (compact 24) controls. A list's rows 32 (a data grid's), a compact list's
// (list::compact) 24 at either density, a file_view's Explorer's 28 (24
// compact); a tree's rows 32 (TreeViewItemMinHeight and its presenter's
// margin; compact 24), a navigator's too; a tab control's tabs 32 (compact
// 24), a sidebar's rows the same. Native (--native): each at its own height,
// as before (a file_view's and a navigator's Explorer's). Under high
// contrast the heights fluent's.
//
// Never-shown windows, at the window's DPI and (static builds) at 125% and
// 150% through the DPI test hook; both densities through the density's
// test hook (--compact: the option's, compact). --transitions: the window
// shown, the display's scale changed for real and put back (CI only).
//   rapport-win32-fluent-rows-test [--native] [--compact] [--transitions]
//
// Built without a Common Controls 6 manifest too (RAPPORT_TEST_NO_MANIFEST,
// win32-fluent-rows-no-manifest): the controls whichever comctl32 the
// backend selects (rapport#41), their rows the same.
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "win32_test_windows.h"
// After windows.h, which they need.
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#if defined(_MSC_VER) && !defined(RAPPORT_TEST_NO_MANIFEST)
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

DWORD windows_build()
{
    using fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto get =
        ntdll
            ? reinterpret_cast<fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
            : nullptr;
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    return get && get(&v) == 0 ? v.dwBuildNumber : 0;
}

bool high_contrast()
{
    HIGHCONTRASTW hc{sizeof hc};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) &&
           (hc.dwFlags & HCF_HIGHCONTRASTON);
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

HWND top_of(const std::string& title)
{
    return test_window(std::wstring(title.begin(), title.end()).c_str());
}

// The window's controls of class `cls`, left to right (each row of the
// layout holds one class).
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

// A density's rows, logical px (win32_band_field.h's band_density).
// `pad`: the room round a taller font's line (TextControlThemePadding's top
// and bottom).
struct density
{
    const char* name;
    float item, file_item, pad;
};
constexpr density standard{"standard", 32, 28, 11};
constexpr density compact{"compact", 24, 24, 5};

class rows_model : public rap::list_model
{
public:
    size_t row_count() const override { return 4; }
    std::string text(size_t row, size_t col) const override
    {
        return (col == 0 ? "Name " : "Note ") + std::to_string(row);
    }
};
rows_model g_model;

int list_row(HWND h)
{
    RECT r{};
    return ListView_GetItemRect(h, 0, &r, LVIR_BOUNDS) ? r.bottom - r.top : -1;
}
int tab_height(HWND h)
{
    // Tab 1, not selected (a selected tab stands out of the row).
    RECT r{};
    return TabCtrl_GetItemRect(h, 1, &r) ? r.bottom - r.top : -1;
}
// A tab control's own height for its font: a fresh one's, given the font.
int native_tab_height(HWND like)
{
    HWND t = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD, 0, 0, 300, 200, GetParent(like),
                             nullptr, nullptr, nullptr);
    if (!t) return -2;
    SendMessageW(t, WM_SETFONT, SendMessageW(like, WM_GETFONT, 0, 0), FALSE);
    for (int i = 0; i < 2; ++i)
    {
        wchar_t text[] = L"Tab";
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = text;
        TabCtrl_InsertItem(t, i, &item);
    }
    const int h = tab_height(t);
    DestroyWindow(t);
    return h;
}

struct controls
{
    HWND list = nullptr, compact_list = nullptr, file_list = nullptr;
    HWND tree = nullptr, navigator = nullptr;
    HWND tabs = nullptr, sidebar = nullptr;
    bool all() const
    {
        return list && compact_list && file_list && tree && navigator && tabs && sidebar;
    }
};
controls controls_of(HWND top)
{
    controls c;
    const std::vector<HWND> lists = all_of(top, WC_LISTVIEWW), trees = all_of(top, WC_TREEVIEWW),
                            tabs = all_of(top, WC_TABCONTROLW), boxes = all_of(top, L"ListBox");
    if (lists.size() == 3) c.list = lists[0], c.compact_list = lists[1], c.file_list = lists[2];
    if (trees.size() == 2) c.tree = trees[0], c.navigator = trees[1];
    if (tabs.size() == 1) c.tabs = tabs[0];
    if (boxes.size() == 1) c.sidebar = boxes[0];
    return c;
}

// Each control's rows at `dpi`: the density's where `fluent`, else their
// own.
void check_rows(const controls& c, bool fluent, const density& d, UINT dpi, const std::string& at)
{
    auto want = [&](float logical) { return px(logical, dpi); };
    const std::string here = at + ", " + str(long(dpi)) + " dpi";
    if (fluent)
    {
        expect(list_row(c.list) == want(d.item),
               "a list's rows " + str(want(d.item)) + here + ": " + str(list_row(c.list)));
        expect(list_row(c.compact_list) == want(compact.item),
               "a compact list's rows " + str(want(compact.item)) + here + ": " +
                   str(list_row(c.compact_list)));
        expect(list_row(c.file_list) == want(d.file_item), "a file_view's rows " +
                                                               str(want(d.file_item)) + here +
                                                               ": " + str(list_row(c.file_list)));
        const int tree = TreeView_GetItemHeight(c.tree), nav = TreeView_GetItemHeight(c.navigator);
        expect(tree == want(d.item),
               "a tree's rows " + str(want(d.item)) + here + ": " + str(tree));
        expect(nav == want(d.item),
               "a navigator's rows " + str(want(d.item)) + here + ": " + str(nav));
        expect(tab_height(c.tabs) == want(d.item),
               "a tab control's tabs " + str(want(d.item)) + here + ": " + str(tab_height(c.tabs)));
        const int side = int(SendMessageW(c.sidebar, LB_GETITEMHEIGHT, 0, 0));
        expect(side == want(d.item),
               "a sidebar's rows " + str(want(d.item)) + here + ": " + str(side));
    }
    else
    {
        // Native: a list's rows its font's (no rows' image list), a
        // file_view's and a navigator's Explorer's (28, 32), the tabs a tab
        // control's own.
        expect(!ListView_GetImageList(c.list, LVSIL_SMALL) &&
                   !ListView_GetImageList(c.compact_list, LVSIL_SMALL),
               "native lists' rows their font's (no rows' image list)" + here);
        expect(list_row(c.file_list) == want(standard.file_item),
               "a native file_view's rows Explorer's, " + str(want(standard.file_item)) + here +
                   ": " + str(list_row(c.file_list)));
        const int nav = TreeView_GetItemHeight(c.navigator);
        expect(nav == want(standard.item), "a native navigator's rows Explorer's, " +
                                               str(want(standard.item)) + here + ": " + str(nav));
        const int tabs = tab_height(c.tabs), own = native_tab_height(c.tabs);
        expect(tabs == own,
               "native tabs a tab control's own, " + str(own) + here + ": " + str(tabs));
    }
    // The header keeps its own height: not given the rows' image list.
    for (HWND l : {c.list, c.compact_list})
        if (HWND header = ListView_GetHeader(l))
            expect(!SendMessageW(header, HDM_GETIMAGELIST, 0, 0),
                   "a list's header without the rows' image list" + here);
}

rap::node rows_layout()
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::vector<rap::column> cols{{"Name", 160_px}, {"Notes", 160_px}};
    const std::vector<rap::tree_item> items{{"Alpha", "a"}, {"Beta", "b"}, {"Gamma", "c"}};
    return vbox{hbox{list{cols, &g_model}.stretch(), list{cols, &g_model}.compact().stretch(),
                     list{cols, &g_model}.file_view().stretch()}
                    .stretch(),
                hbox{tree{items}.stretch(), tree{items}.navigator().stretch()}.stretch(),
                hbox{tabs{{"One", label{"one"}}, {"Two", label{"two"}}}.stretch(),
                     tabs{{"One", label{"one"}}, {"Two", label{"two"}}}.sidebar().stretch()}
                    .stretch()};
}

// -- the display's scale, for real (CI only) ----------------------------------
//
// As win32-band-fields-transitions (and win32-caption-tabs-transitions)
// change it: the primary display's scale through DisplayConfig's
// undocumented DPI scale (as Settings sets it, in steps relative to the
// recommended one), its mode made larger first where a small screen allows
// only 100%, and both put back exactly as they were, read back to be sure,
// on every path. (The same code as win32_band_fields_test.cpp's.)

struct dpi_scale_get
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    int32_t min_rel, cur_rel, max_rel;
};
struct dpi_scale_set
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    int32_t rel;
};
struct display_source
{
    LUID adapter{};
    UINT32 id = 0;
    bool known = false;
};
display_source g_source;

bool source_of(const std::wstring& gdi_name, display_source& out)
{
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS ||
        !paths)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr) !=
        ERROR_SUCCESS)
        return false;
    for (UINT32 i = 0; i < paths; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof name;
        name.header.adapterId = p[i].sourceInfo.adapterId;
        name.header.id = p[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS &&
            gdi_name == name.viewGdiDeviceName)
        {
            out = {p[i].sourceInfo.adapterId, p[i].sourceInfo.id, true};
            return true;
        }
    }
    return false;
}
bool get_scale(dpi_scale_get& g)
{
    g = {};
    if (!g_source.known) return false;
    g.header.type = DISPLAYCONFIG_DEVICE_INFO_TYPE(-3);
    g.header.size = sizeof g;
    g.header.adapterId = g_source.adapter;
    g.header.id = g_source.id;
    return DisplayConfigGetDeviceInfo(&g.header) == ERROR_SUCCESS;
}
bool set_scale(int32_t rel)
{
    if (!g_source.known) return false;
    dpi_scale_set s{};
    s.header.type = DISPLAYCONFIG_DEVICE_INFO_TYPE(-4);
    s.header.size = sizeof s;
    s.header.adapterId = g_source.adapter;
    s.header.id = g_source.id;
    s.rel = rel;
    return DisplayConfigSetDeviceInfo(&s.header) == ERROR_SUCCESS;
}

template <class F>
bool pump_until(F&& done, int ms = 10000)
{
    for (int waited = 0; waited < ms; waited += 50)
    {
        if (done()) return true;
        pump(5);
    }
    return done();
}

// The display as it was before the first change, put back to exactly that
// (main undoes it, the destructor if anything returns first); each restore
// read back, bounded, and one that fails a failure, left marked.
struct restore
{
    bool saved = false;
    std::wstring device;
    DEVMODEW mode{};
    int32_t rel = 0;
    bool scale = false, mode_changed = false;  // changed and not yet put back

    ~restore() { undo(); }

    bool save()
    {
        if (saved) return true;
        DISPLAY_DEVICEW dev{sizeof dev};
        bool found = false;
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i)
            if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
            {
                found = true;
                break;
            }
        mode = {};
        mode.dmSize = sizeof mode;
        dpi_scale_get g{};
        if (!found || !source_of(dev.DeviceName, g_source) ||
            !EnumDisplaySettingsW(dev.DeviceName, ENUM_CURRENT_SETTINGS, &mode) || !get_scale(g))
            return false;
        device = dev.DeviceName;
        rel = g.cur_rel;
        saved = true;
        return true;
    }
    bool mode_is_saved() const
    {
        DEVMODEW now{};
        now.dmSize = sizeof now;
        return EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &now) &&
               now.dmPelsWidth == mode.dmPelsWidth && now.dmPelsHeight == mode.dmPelsHeight &&
               now.dmBitsPerPel == mode.dmBitsPerPel &&
               now.dmDisplayFrequency == mode.dmDisplayFrequency;
    }
    bool scale_is_saved() const
    {
        dpi_scale_get now{};
        return get_scale(now) && now.cur_rel == rel;
    }
    template <class F>
    static bool settles(F&& is)
    {
        for (int waited = 0; waited < 5000; waited += 50)
        {
            if (is()) return true;
            pump(5);
        }
        return is();
    }
    void undo()
    {
        // The mode first: the scale saved is the saved mode's.
        if (mode_changed)
        {
            const LONG r = ChangeDisplaySettingsExW(device.c_str(), &mode, nullptr, 0, nullptr);
            const bool ok = r == DISP_CHANGE_SUCCESSFUL && settles([&] { return mode_is_saved(); });
            expect(ok, "the display's mode put back to " + str(long(mode.dmPelsWidth)) + "x" +
                           str(long(mode.dmPelsHeight)) + " (" + str(long(r)) + ")");
            if (ok) mode_changed = false;
        }
        if (scale)
        {
            const bool ok = set_scale(rel) && settles([&] { return scale_is_saved(); });
            expect(ok, "the display's scale put back to step " + str(rel));
            if (ok) scale = false;
        }
    }
};
restore g_restore;

// Every scale the display allows, for real, `check` at each new DPI of
// `top` (125% and 150% where the runner reaches them), then the scale put
// back and `check` again. Which ran is noted.
int scale_transitions(HWND top, const std::function<void(UINT, const std::string&)>& check)
{
    if (!g_restore.save())
    {
        expect(false, "the display's mode and scale read before changing them");
        return 1;
    }
    g_restore.scale = true;  // armed before anything changes
    dpi_scale_get g{};
    if (!get_scale(g))
    {
        expect(false, "the display's scale read");
        return 1;
    }
    note("display scale steps " + str(g.min_rel) + ".." + str(g.max_rel) + ", now " +
         str(g.cur_rel));
    // A small screen (a runner's 1024x768) allows only 100%: its largest
    // mode first, for the scales it allows.
    if (g.max_rel <= g.cur_rel + 1)
    {
        const wchar_t* device = g_restore.device.c_str();
        DEVMODEW best{}, mode{};
        best.dmSize = mode.dmSize = sizeof(DEVMODEW);
        for (DWORD i = 0; EnumDisplaySettingsW(device, i, &mode); ++i)
            if (mode.dmPelsWidth * mode.dmPelsHeight > best.dmPelsWidth * best.dmPelsHeight)
                best = mode;
        if (best.dmPelsWidth && best.dmPelsWidth * best.dmPelsHeight >
                                    g_restore.mode.dmPelsWidth * g_restore.mode.dmPelsHeight)
        {
            g_restore.mode_changed = true;  // marked before it's tried: half made is put back too
            if (ChangeDisplaySettingsExW(device, &best, nullptr, CDS_FULLSCREEN, nullptr) ==
                DISP_CHANGE_SUCCESSFUL)
            {
                note("display mode " + str(long(best.dmPelsWidth)) + "x" +
                     str(long(best.dmPelsHeight)));
                pump(50);
                get_scale(g);
                note("display scale steps " + str(g.min_rel) + ".." + str(g.max_rel) + ", now " +
                     str(g.cur_rel));
            }
        }
    }
    const UINT before = GetDpiForWindow(top);
    std::string ran;
    bool at_125 = false;
    UINT now = before;
    for (int32_t rel = g.min_rel; rel <= g.max_rel; ++rel)
    {
        if (rel == g.cur_rel) continue;
        if (!set_scale(rel))
        {
            expect(false, "the display's scale set to step " + str(rel));
            continue;
        }
        const UINT was = now;
        pump_until([&] { return GetDpiForWindow(top) != was; });
        pump();
        now = GetDpiForWindow(top);
        if (now == was) continue;
        ran += (ran.empty() ? "" : ", ") + str(long(now * 100 / 96)) + "%";
        at_125 = at_125 || now == 120;
        check(now, ", the display's scale, " + str(long(now)) + " dpi");
    }
    note("scales that ran: " + (ran.empty() ? std::string("none") : ran));
    expect(!ran.empty(), "the display allows another scale");
    expect(at_125, "125% among them");
    g_restore.undo();
    expect(pump_until([&] { return GetDpiForWindow(top) == before; }),
           "and back to " + str(long(before)));
    pump();
    check(before, ", the scale back, " + str(long(before)) + " dpi");
    return 0;
}

// One density: the controls in a never-shown window, checked at its DPI and
// (static builds) at 125% and 150% through the DPI test hook, as Windows
// sends WM_DPICHANGED (every control given the UI font at the new DPI).
void run(rap::ui& ui, const density& d, bool fluent)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::string title = std::string("Fluent rows ") + d.name;
    auto win = ui.create(window{title, {rows_layout()}}.size(1000_px, 700_px));
    pump();
    HWND top = top_of(title);
    expect(top != nullptr, "the window, " + std::string(d.name));
    if (!top) return;
    const controls c = controls_of(top);
    expect(c.all(), "its lists, trees, tabs and sidebar");
    if (!c.all()) return;
    const std::string at = std::string(", ") + d.name;
    const UINT dpi = GetDpiForWindow(top);
    check_rows(c, fluent, d, dpi, at);
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    UINT was = dpi;
    for (const UINT to : {120u, 144u, dpi})
    {
        rap::win32::set_dpi_for_tests(to == dpi ? 0 : to);
        RECT wr{};
        GetWindowRect(top, &wr);
        RECT suggested{wr.left, wr.top, wr.left + MulDiv(wr.right - wr.left, int(to), int(was)),
                       wr.top + MulDiv(wr.bottom - wr.top, int(to), int(was))};
        was = to;
        SendMessageW(top, WM_DPICHANGED, MAKEWPARAM(to, to), reinterpret_cast<LPARAM>(&suggested));
        pump();
        check_rows(c, fluent, d, to, at + ", the DPI hook");
    }
#endif
}

// A tree view's own row height for its font: a fresh one's, given the font.
int native_tree_height(HWND like)
{
    HWND t = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD, 0, 0, 300, 200, GetParent(like),
                             nullptr, nullptr, nullptr);
    if (!t) return -2;
    SendMessageW(t, WM_SETFONT, SendMessageW(like, WM_GETFONT, 0, 0), FALSE);
    const int h = TreeView_GetItemHeight(t);
    DestroyWindow(t);
    return h;
}

// A list, a tree and tabs in the toolbar band's row (a window with a base
// surface and document tabs: its first row), each the band's density's rows
// (fluent or native: the band's row is the band's); out of the band (the
// document tabs hidden: no band) the app's again: the density's where
// fluent, native their own (the row height set while in the band undone);
// and back in the band, the band's.
void run_band(rap::ui& ui, const density& d, bool fluent)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::string title = std::string("Fluent rows band ") + d.name;
    const std::vector<rap::column> cols{{"Name", 160_px}, {"Notes", 160_px}};
    const std::vector<rap::tree_item> items{{"Alpha", "a"}, {"Beta", "b"}};
    rap::document_tabs* doc_tabs = nullptr;
    auto win = ui.create(
        window{title,
               {vbox{hbox{list{cols, &g_model}.stretch(), tree{items}.stretch(),
                          tabs{{"One", label{"one"}}, {"Two", label{"two"}}}.stretch()},
                     label{"content"}.stretch()}}}
            .size(1100_px, 400_px)
            .margins(false)
            .base_surface()
            .tabs(document_tabs{{{"a", "Alpha", rap::icon::named("folder"), {}}}}.bind(doc_tabs)));
    pump();
    HWND top = top_of(title);
    expect(top != nullptr && doc_tabs != nullptr, "the band's window, " + std::string(d.name));
    if (!top || !doc_tabs) return;
    const std::vector<HWND> lists = all_of(top, WC_LISTVIEWW), trees = all_of(top, WC_TREEVIEWW),
                            tabs_h = all_of(top, WC_TABCONTROLW);
    // The document tabs are a tab control too: the row's is the one with
    // two tabs and a page.
    HWND page_tabs = nullptr;
    for (HWND t : tabs_h)
        if (TabCtrl_GetItemCount(t) == 2) page_tabs = t;
    expect(lists.size() == 1 && trees.size() == 1 && page_tabs, "the band's list, tree and tabs");
    if (lists.size() != 1 || trees.size() != 1 || !page_tabs) return;
    HWND l = lists[0], t = trees[0];
    const UINT dpi = GetDpiForWindow(top);
    const int want = px(d.item, dpi);
    auto in_band = [&](const std::string& at)
    {
        expect(list_row(l) == want,
               "a list in the band's row, its rows " + str(want) + at + ": " + str(list_row(l)));
        expect(TreeView_GetItemHeight(t) == want, "a tree in the band's row, its rows " +
                                                      str(want) + at + ": " +
                                                      str(TreeView_GetItemHeight(t)));
        expect(tab_height(page_tabs) == want,
               "tabs in the band's row, " + str(want) + at + ": " + str(tab_height(page_tabs)));
    };
    const std::string at = std::string(", ") + d.name;
    in_band(at);
    doc_tabs->set_visible(false);
    pump();
    const std::string out = ", out of the band" + at;
    if (fluent)
        in_band(out);
    else
    {
        expect(!ListView_GetImageList(l, LVSIL_SMALL),
               "a native list out of the band, no rows' image list");
        const int tree = TreeView_GetItemHeight(t), tree_own = native_tree_height(t);
        expect(tree == tree_own,
               "a native tree out of the band, its own rows, " + str(tree_own) + ": " + str(tree));
        const int tabs = tab_height(page_tabs), own = native_tab_height(page_tabs);
        expect(tabs == own,
               "native tabs out of the band, their own, " + str(own) + ": " + str(tabs));
    }
    doc_tabs->set_visible(true);
    pump();
    in_band(", back in the band" + at);
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

// A sidebar of four pages in a window of its own natural size (none asked
// for): every row whole in the list, none clipped.
void sidebar_fits(rap::ui& ui, const density& d, const std::string& at)
{
    using namespace rap::layout;
    const std::string title = std::string("Fluent rows sidebar ") + d.name;
    auto win = ui.create(window{title,
                                {tabs{{"General", label{"one"}},
                                      {"Appearance", label{"two"}},
                                      {"Fonts", label{"three"}},
                                      {"Advanced", label{"four"}}}
                                     .sidebar()}});
    pump();
    HWND top = top_of(title);
    const std::vector<HWND> boxes = top ? all_of(top, L"ListBox") : std::vector<HWND>{};
    expect(boxes.size() == 1, "a sidebar in a window of its natural size" + at);
    if (boxes.size() != 1) return;
    RECT client{};
    GetClientRect(boxes[0], &client);
    for (int i = 0; i < 4; ++i)
    {
        RECT r{};
        SendMessageW(boxes[0], LB_GETITEMRECT, i, reinterpret_cast<LPARAM>(&r));
        expect(r.top >= client.top && r.bottom <= client.bottom,
               "its row " + str(i) + " whole, " + str(r.top) + ".." + str(r.bottom) + " in 0.." +
                   str(client.bottom) + at);
    }
}

#ifdef RAPPORT_TEST_BUILTIN_WIN32
// A large text size (150%, the message font 18 DIPs: the text size test
// hooks, the setting untouched, WM_SETTINGCHANGE as Windows sends it) while
// the window's open: each row the density's or, taller, its font's line
// with the density's room round it, no text clipped (a tree's, tabs' and a
// list box's set height is kept however tall the font is); a sidebar made
// at it whole at its natural size. Then 100% again, and the density's rows.
void large_text(rap::ui& ui, const density& d)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::string title = std::string("Fluent rows large text ") + d.name;
    auto win = ui.create(window{title, {rows_layout()}}.size(1000_px, 700_px));
    pump();
    HWND top = top_of(title);
    const controls c = top ? controls_of(top) : controls{};
    expect(c.all(), "the large text window's controls, " + std::string(d.name));
    if (!c.all()) return;
    auto settings_changed = [&]
    {
        SendMessageW(top, WM_SETTINGCHANGE, SPI_SETNONCLIENTMETRICS,
                     reinterpret_cast<LPARAM>(L"WindowMetrics"));
        pump();
    };
    rap::win32::set_text_scale_for_tests(1.5f);
    rap::win32::set_message_font_size_for_tests(18.f);
    settings_changed();
    const UINT dpi = GetDpiForWindow(top);
    const std::string at = std::string(", text 150%, ") + d.name;
    auto row_for = [&](HWND h, float item, float pad)
    { return std::max(px(item, dpi), font_line(h) + px(pad, dpi)); };
    // A tree view's rows even: an odd one rounded up.
    auto even = [](int h) { return h + (h & 1); };
    struct row
    {
        const char* what;
        HWND h;
        int now, want;
    };
    const int line = font_line(c.tree);
    expect(line + px(d.pad, dpi) > px(d.item, dpi),
           "the font's line with its room taller than the density's row, " + str(line) + " + " +
               str(px(d.pad, dpi)) + at);
    const row rows[] = {
        {"a list's rows", c.list, list_row(c.list), row_for(c.list, d.item, d.pad)},
        {"a compact list's rows", c.compact_list, list_row(c.compact_list),
         row_for(c.compact_list, compact.item, compact.pad)},
        {"a file_view's rows", c.file_list, list_row(c.file_list),
         row_for(c.file_list, d.file_item, d.pad)},
        {"a tree's rows", c.tree, TreeView_GetItemHeight(c.tree),
         even(row_for(c.tree, d.item, d.pad))},
        {"a navigator's rows", c.navigator, TreeView_GetItemHeight(c.navigator),
         even(row_for(c.navigator, d.item, d.pad))},
        {"a tab control's tabs", c.tabs, tab_height(c.tabs), row_for(c.tabs, d.item, d.pad)},
        {"a sidebar's rows", c.sidebar, int(SendMessageW(c.sidebar, LB_GETITEMHEIGHT, 0, 0)),
         row_for(c.sidebar, d.item, d.pad)},
    };
    for (const row& r : rows)
        expect(r.now == r.want && r.now >= font_line(r.h),
               std::string(r.what) + " " + str(r.want) + ", its font's line " +
                   str(font_line(r.h)) + " whole" + at + ": " + str(r.now));
    sidebar_fits(ui, d, at);
    rap::win32::set_text_scale_for_tests(1.f);
    rap::win32::set_message_font_size_for_tests(12.f);
    settings_changed();
    check_rows(c, true, d, dpi, ", text 100% again, " + std::string(d.name));
}
#endif

// --transitions: the window shown, taking each scale the display allows.
int transitions(rap::ui& ui, const density& d, bool fluent)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::string title = std::string("Fluent rows transitions ") + d.name;
    auto win = ui.create(window{title, {rows_layout()}}.size(1000_px, 700_px));
    win->show();
    pump();
    HWND top = top_of(title);
    expect(top != nullptr, "the window, shown");
    if (!top) return 1;
    const controls c = controls_of(top);
    expect(c.all(), "its lists, trees, tabs and sidebar");
    if (!c.all()) return 1;
    const std::string at = std::string(", ") + d.name;
    check_rows(c, fluent, d, GetDpiForWindow(top), at + ", before");
    return scale_transitions(top, [&](UINT dpi, const std::string& here)
                             { check_rows(c, fluent, d, dpi, at + here); });
}

}  // namespace

int main(int argc, char** argv)
{
    bool native = false, compact_run = env("RAPPORT_DENSITY") == "compact", transitions_run = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--native") native = true;
        if (std::string(argv[i]) == "--compact") compact_run = true;
        if (std::string(argv[i]) == "--transitions") transitions_run = true;
    }
    // --transitions changes the display's scale: on a CI runner only, and
    // checked before any window is made.
    if (transitions_run && (env("GITHUB_ACTIONS") != "true" || env("RAPPORT_TEST_SET_DPI") != "1"))
    {
        note("skipped the scale changes: they change the display's scale, so only on a CI runner "
             "(GITHUB_ACTIONS=true, RAPPORT_TEST_SET_DPI=1)");
        return 77;
    }
    const DWORD build = windows_build();
    if (build < 22000)
    {
        note("skipped: native before Windows 11 (build 22000), file views and navigators too");
        return 77;
    }
    rap::load_options options{.backend = "win32"};
    options.look.color_scheme = rap::color_scheme::dark;
    if (native) options.look.controls = rap::control_style::native;
    if (compact_run) options.look.density = rap::density::compact;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 2;
    const bool fluent = !native;
    std::printf("build %lu, %s, %s, %s\n", build,
                high_contrast() ? "high contrast" : "no high contrast",
                fluent ? "fluent" : "native", compact_run ? "compact" : "standard");
    if (transitions_run)
    {
        const int r = transitions(**ui, compact_run ? compact : standard, fluent);
        g_restore.undo();
        std::printf(fails ? "%d failed\n" : "all passed\n", fails);
        return fails ? 1 : r;
    }
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    if (!compact_run && !native)
    {
        // Both, through the density's test hook.
        rap::win32::set_band_density_for_tests(0);
        run(**ui, standard, fluent);
        run_band(**ui, standard, fluent);
        sidebar_fits(**ui, standard, ", standard");
        large_text(**ui, standard);
        rap::win32::set_band_density_for_tests(1);
        run(**ui, compact, fluent);
        run_band(**ui, compact, fluent);
        sidebar_fits(**ui, compact, ", compact");
        large_text(**ui, compact);
        rap::win32::set_band_density_for_tests(-1);
    }
    else
    {
        const density& d = compact_run ? compact : standard;
        run(**ui, d, fluent);
        run_band(**ui, d, fluent);
        sidebar_fits(**ui, d, std::string(", ") + d.name);
        if (fluent) large_text(**ui, d);
    }
#else
    run(**ui, compact_run ? compact : standard, fluent);
    run_band(**ui, compact_run ? compact : standard, fluent);
    sidebar_fits(**ui, compact_run ? compact : standard, "");
#endif
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
