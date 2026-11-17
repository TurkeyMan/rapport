// Tab page visibility on Win32 (rap::tabs::set_page_visible,
// layout::page::visible, tabs::hide_single_tab), for a strip (a
// WC_TABCONTROL) and a sidebar (a list box): a hidden page's tab or row
// deleted and the other pages' indices kept, the selection moving to the
// nearest visible page without on_changed, -1 with none visible, a page shown
// again back at its place, set_current on a hidden page ignored, pages hidden
// from the start never shown (a lazy one unbuilt), a sidebar's natural height
// its visible rows', and hide_single_tab: no strip or list with one page
// visible, the page filling the area and the natural size without them.
// And #92: a canvas of a fixed width and height as a tab page's, a sidebar
// page's and a stack page's whole content, measured at that size.
// And #101: a page's icon and title, from the start and at runtime (set,
// swapped, removed; kept while hidden; the same again nothing; out of range
// ignored), strip and sidebar, and a strip in dark mode.
//
// Native reads throughout: TabCtrl_GetItemCount and the items' text,
// LB_GETCOUNT and LB_GETTEXT, the controls' WS_VISIBLE, the window's least
// size (WM_GETMINMAXINFO, which is the content's natural size). Never-shown
// windows.
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which they need.
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
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
unsigned layout_requests_for_tests();  // win32_common.h
}  // namespace rap::win32
#endif

namespace
{

// Relayouts asked for so far (static builds; a plugin build can't count
// them: its checks of none pass trivially, and a check that some were asked
// for is made only where they're counted, laid_out).
#ifdef RAPPORT_TEST_BUILTIN_WIN32
constexpr bool layouts_counted = true;
#else
constexpr bool layouts_counted = false;
#endif
unsigned layouts();
// Some relayout asked for, `n` of them counted; in a plugin build, unknown:
// taken as yes (the check's other conditions still checked).
bool laid_out(unsigned n)
{
    return !layouts_counted || n != 0;
}
unsigned layouts()
{
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    return rap::win32::layout_requests_for_tests();
#else
    return 0;
#endif
}

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
}
std::string str(long v)
{
    return std::to_string(v);
}
// An ASCII title, as the window's.
std::string narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += char(c);
    return s;
}

void pump(int rounds = 10)
{
    for (int round = 0; round < rounds; ++round)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

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
    return f.found;
}
HWND one_of(HWND top, const wchar_t* cls)
{
    const std::vector<HWND> all = top ? all_of(top, cls) : std::vector<HWND>{};
    return all.size() == 1 ? all[0] : nullptr;
}
// The control with this text (a page's label).
HWND with_text(HWND top, const std::wstring& text)
{
    for (HWND h : all_of(top, L"STATIC"))
    {
        wchar_t t[128]{};
        GetWindowTextW(h, t, 128);
        if (text == t) return h;
    }
    return nullptr;
}
bool shown(HWND h)
{
    return h && (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE);
}

// The headers, in order: a strip's tabs or a sidebar's rows, by their text.
std::string headers(HWND h, bool sidebar)
{
    std::string out;
    const int n = sidebar ? int(SendMessageW(h, LB_GETCOUNT, 0, 0)) : TabCtrl_GetItemCount(h);
    for (int i = 0; i < n; ++i)
    {
        wchar_t t[64]{};
        if (sidebar)
            SendMessageW(h, LB_GETTEXT, i, reinterpret_cast<LPARAM>(t));
        else
        {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = t;
            item.cchTextMax = 64;
            TabCtrl_GetItem(h, i, &item);
        }
        for (const wchar_t* c = t; *c; ++c) out += char(*c);
    }
    return out;
}
// The selected header's text.
std::string selected(HWND h, bool sidebar)
{
    const int i = sidebar ? int(SendMessageW(h, LB_GETCURSEL, 0, 0)) : TabCtrl_GetCurSel(h);
    if (i < 0) return "-";
    std::string all = headers(h, sidebar);
    return i < int(all.size()) ? all.substr(size_t(i), 1) : "?";
}

// The window's least client size: its content's natural size and margins.
SIZE least(HWND top)
{
    MINMAXINFO mm{};
    SendMessageW(top, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&mm));
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&frame, DWORD(GetWindowLongPtrW(top, GWL_STYLE)), FALSE,
                             DWORD(GetWindowLongPtrW(top, GWL_EXSTYLE)), GetDpiForWindow(top));
    return {mm.ptMinTrackSize.x - (frame.right - frame.left), mm.ptMinTrackSize.y - (frame.bottom - frame.top)};
}
RECT rect_in(HWND top, HWND h)
{
    RECT r{};
    GetWindowRect(h, &r);
    MapWindowPoints(nullptr, top, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

class rows_model : public rap::list_model
{
public:
    size_t row_count() const override { return 2; }
    std::string text(size_t row, size_t) const override { return "Row " + std::to_string(row); }
};
rows_model g_model;

// Hiding and showing pages, strip or sidebar.
void visibility(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "sidebar: " : "strip: ";
    const std::wstring title = sidebar ? L"Visibility sidebar" : L"Visibility strip";
    rap::tabs* t = nullptr;
    rap::label* lazy_label = nullptr;
    int changes = 0;
    auto win = ui.create(window{narrow(title),
                                {tabs{{"A", label{"page a"}},
                                      {"B", label{"page b"}},
                                      {"C", label{"page c"}},
                                      {"D", lazy{[&] { return rap::node(label{"page d"}.bind(lazy_label)); }}}}
                                     .sidebar(sidebar)
                                     .on_changed([&](int) { ++changes; })
                                     .bind(t)}});
    pump();
    HWND top = FindWindowW(nullptr, title.c_str());
    HWND h = top ? one_of(top, sidebar ? L"ListBox" : WC_TABCONTROLW) : nullptr;
    expect(t && h, kind + "the window, never shown, and its " + (sidebar ? "list" : "tab control"));
    if (!t || !h) return;
    expect(headers(h, sidebar) == "ABCD" && t->current() == 0, kind + "ABCD, A current");

    // A middle page hidden: its header gone, the others' indices kept.
    t->set_page_visible(1, false);
    pump();
    expect(headers(h, sidebar) == "ACD", kind + "B hidden: headers ACD, " + headers(h, sidebar));
    expect(!t->page_visible(1) && t->page_visible(0) && t->page_visible(2), kind + "page 1 hidden, 0 and 2 not");
    expect(!shown(with_text(top, L"page b")), kind + "B's page not shown");
    t->set_current(2);
    pump();
    expect(t->current() == 2 && selected(h, sidebar) == "C", kind + "set_current(2): page 2 is C, selected");
    expect(shown(with_text(top, L"page c")) && !shown(with_text(top, L"page a")), kind + "C's page shown, A's not");
    // set_current on a hidden page: nothing.
    t->set_current(1);
    pump();
    expect(t->current() == 2 && selected(h, sidebar) == "C", kind + "set_current(hidden 1): still 2");

    // The current page hidden: the nearest visible after it (the lazy D,
    // built now), without on_changed.
    t->set_page_visible(2, false);
    pump();
    expect(t->current() == 3 && selected(h, sidebar) == "D" && headers(h, sidebar) == "AD",
           kind + "C (current) hidden: D current and selected, headers AD, " + str(t->current()));
    expect(lazy_label != nullptr && shown(with_text(top, L"page d")), kind + "the lazy D built and shown");
    expect(!shown(with_text(top, L"page c")), kind + "C's page no longer shown");
    // The last page hidden while current: the nearest before it.
    t->set_page_visible(3, false);
    pump();
    expect(t->current() == 0 && selected(h, sidebar) == "A", kind + "D (current, last) hidden: A current");
    // Every page hidden: -1, nothing selected, no page shown.
    t->set_page_visible(0, false);
    pump();
    expect(t->current() == -1 && headers(h, sidebar).empty(), kind + "every page hidden: current -1, no headers, " +
                                                                    str(t->current()));
    expect(!shown(with_text(top, L"page a")) && !shown(with_text(top, L"page d")), kind + "no page shown");
    t->set_current(0);
    expect(t->current() == -1, kind + "set_current(hidden 0) with none visible: still -1");
    // Shown again: current again (it was -1); another shown keeps it, at its
    // place among the headers.
    t->set_page_visible(2, true);
    pump();
    expect(t->current() == 2 && selected(h, sidebar) == "C" && shown(with_text(top, L"page c")),
           kind + "C shown again with none current: C current and showing");
    t->set_page_visible(3, true);
    t->set_page_visible(1, true);
    pump();
    expect(headers(h, sidebar) == "BCD", kind + "B and D shown again: headers BCD, " + headers(h, sidebar));
    expect(t->current() == 2 && selected(h, sidebar) == "C", kind + "current stays C");
    t->set_page_visible(0, true);
    pump();
    expect(headers(h, sidebar) == "ABCD" && t->current() == 2 && selected(h, sidebar) == "C",
           kind + "A shown again: first among ABCD, C still current");
    // The same again: nothing.
    t->set_page_visible(0, true);
    expect(headers(h, sidebar) == "ABCD" && t->current() == 2, kind + "A shown again, twice: nothing");
    expect(changes == 0, kind + "no on_changed from any of it, " + str(changes));
    t->set_page_visible(9, false);
    expect(!t->page_visible(9) && !t->page_visible(-1) && headers(h, sidebar) == "ABCD",
           kind + "pages out of range: not visible, nothing changed");
}

std::string seq(const std::vector<int>& v)
{
    std::string s;
    for (int i : v) s += (s.empty() ? "" : ",") + std::to_string(i);
    return "[" + s + "]";
}

// #98: set_current is the app's own: no on_changed, the page made current
// natively, a lazy one built and shown; to the current page, nothing. A lazy
// page made current by hiding the current one: built and shown, no
// on_changed. A user's selection, made on the native control as a click's
// reaches rapport: on_changed once, with the page. A strip's by
// TabCtrl_SetCurFocus (it selects and sends TCN_SELCHANGE, as a click
// does), a sidebar's by LB_SETCURSEL and the LBN_SELCHANGE the list box
// sends its parent.
void set_current_quiet(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "sidebar, set_current (#98): " : "strip, set_current (#98): ";
    const std::wstring title = sidebar ? L"Quiet sidebar" : L"Quiet strip";
    rap::tabs* t = nullptr;
    rap::label* built[4] = {};
    std::vector<int> changes;
    auto lazy_page = [&built](int i, const char* text)
    { return lazy{[&built, i, text] { return rap::node(label{text}.bind(built[i])); }}; };
    auto win = ui.create(window{narrow(title),
                                {tabs{{"A", label{"page a"}},
                                      {"B", lazy_page(1, "page b")},
                                      {"C", lazy_page(2, "page c")},
                                      {"D", lazy_page(3, "page d")}}
                                     .sidebar(sidebar)
                                     .on_changed([&](int i) { changes.push_back(i); })
                                     .bind(t)}});
    pump();
    HWND top = FindWindowW(nullptr, title.c_str());
    HWND h = top ? one_of(top, sidebar ? L"ListBox" : WC_TABCONTROLW) : nullptr;
    expect(t && h, kind + "the window, never shown, and its " + (sidebar ? "list" : "tab control"));
    if (!t || !h) return;
    // The page shown natively: its label's control visible, every other
    // page's not (an unbuilt page has none).
    const wchar_t* texts[] = {L"page a", L"page b", L"page c", L"page d"};
    auto showing = [&](int page)
    {
        for (int i = 0; i < 4; ++i)
            if (shown(with_text(top, texts[i])) != (i == page)) return false;
        return true;
    };
    expect(t->current() == 0 && selected(h, sidebar) == "A" && showing(0) && !built[1] && !built[2] && !built[3],
           kind + "A current and shown, the lazy B, C and D unbuilt");
    changes.clear();

    // To the lazy B: no on_changed; current and selected; built, shown.
    t->set_current(1);
    pump();
    expect(changes.empty(), kind + "set_current(1): no on_changed, " + seq(changes));
    expect(t->current() == 1 && selected(h, sidebar) == "B",
           kind + "set_current(1): B current and selected, " + str(t->current()) + ", " + selected(h, sidebar));
    expect(built[1] && showing(1), kind + "set_current(1): the lazy B built and shown");
    // The current page again: nothing.
    t->set_current(1);
    pump();
    expect(changes.empty(), kind + "set_current(1) again: no on_changed, " + seq(changes));
    expect(t->current() == 1 && selected(h, sidebar) == "B" && showing(1) && !built[2] && !built[3],
           kind + "set_current(1) again: nothing changed");
    // B (current) hidden: the lazy C, the nearest after it, built and shown,
    // no on_changed.
    t->set_page_visible(1, false);
    pump();
    expect(t->current() == 2 && selected(h, sidebar) == "C",
           kind + "B (current) hidden: C current and selected, " + str(t->current()) + ", " + selected(h, sidebar));
    expect(built[2] && showing(2), kind + "B (current) hidden: the lazy C built and shown");
    expect(changes.empty(), kind + "B (current) hidden: no on_changed, " + seq(changes));

    // A user's selection of the lazy D (its header the third of ACD): on
    // the native control, on_changed once, with 3; built and shown.
    const int item = int(headers(h, sidebar).find('D'));
    expect(item == 2, kind + "D's header the third, " + str(item));
    if (sidebar)
    {
        SendMessageW(h, LB_SETCURSEL, WPARAM(item), 0);
        SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), LBN_SELCHANGE), LPARAM(h));
    }
    else
        TabCtrl_SetCurFocus(h, item);
    pump();
    expect(changes == std::vector<int>{3}, kind + "a user's selection of D: on_changed once, with 3, " + seq(changes));
    expect(t->current() == 3 && selected(h, sidebar) == "D" && built[3] && showing(3),
           kind + "a user's selection of D: D current, built and shown, " + str(t->current()));
    // set_current to the current page: nothing.
    changes.clear();
    t->set_current(3);
    pump();
    expect(changes.empty() && t->current() == 3 && selected(h, sidebar) == "D" && showing(3),
           kind + "set_current(3), current already: nothing, " + seq(changes));
    // Back to A, the app's: no on_changed.
    t->set_current(0);
    pump();
    expect(changes.empty() && t->current() == 0 && selected(h, sidebar) == "A" && showing(0),
           kind + "set_current(0): A current and shown, no on_changed, " + seq(changes));
}

// #98, re-entered: a lazy page's builder that calls the tabs back while the
// switch that built it is still being handled. The app's set_current to B,
// whose builder calls set_current(2): no on_changed, C current and shown. B
// (current) hidden, the lazy C made current, whose builder hides D: no
// on_changed, C current and shown, D's header gone. A user's switch to a
// lazy page: built, shown, on_changed once with it; to B whose builder calls
// set_current(2): C current and shown, on_changed once, with 2. A user's
// switch as in set_current_quiet: TabCtrl_SetCurFocus, or LB_SETCURSEL and
// LBN_SELCHANGE.
void reentrant(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    using hook = std::function<void(rap::tabs*)>;
    // The tabs, their native control and window, the pages built so far,
    // the on_changed calls, and the checks' prefix.
    struct run
    {
        rap::tabs* t;
        HWND top;
        HWND h;
        rap::label** built;
        std::vector<int>& changes;
        std::string kind;
    };
    const std::string side = sidebar ? "sidebar" : "strip";
    // The page shown natively: its label's control visible, every other
    // page's not (an unbuilt page has none).
    auto showing = [](const run& r, int page)
    {
        const wchar_t* texts[] = {L"page a", L"page b", L"page c", L"page d"};
        for (int i = 0; i < 4; ++i)
            if (shown(with_text(r.top, texts[i])) != (i == page)) return false;
        return true;
    };
    // A user's selection of the item `item`, as a click's reaches rapport.
    auto user_select = [sidebar](const run& r, int item)
    {
        if (sidebar)
        {
            SendMessageW(r.h, LB_SETCURSEL, WPARAM(item), 0);
            SendMessageW(GetParent(r.h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(r.h), LBN_SELCHANGE), LPARAM(r.h));
        }
        else
            TabCtrl_SetCurFocus(r.h, item);
        pump();
    };
    // A window of A and the lazy B, C and D, page i's builder calling
    // then[i] first; `body` run on it.
    auto scenario = [&](const std::string& name, std::array<hook, 4> then, const std::function<void(const run&)>& body)
    {
        const std::string kind = side + ", " + name + " (#98): ";
        const std::string title = "Reentrant " + side + " " + name;
        rap::tabs* t = nullptr;
        rap::label* built[4] = {};
        std::vector<int> changes;
        auto lazy_page = [&](int i, const char* text)
        {
            return lazy{[&, i, text]
                        {
                            if (then[size_t(i)]) then[size_t(i)](t);
                            return rap::node(label{text}.bind(built[i]));
                        }};
        };
        auto win = ui.create(window{title,
                                    {tabs{{"A", label{"page a"}},
                                          {"B", lazy_page(1, "page b")},
                                          {"C", lazy_page(2, "page c")},
                                          {"D", lazy_page(3, "page d")}}
                                         .sidebar(sidebar)
                                         .on_changed([&](int i) { changes.push_back(i); })
                                         .bind(t)}});
        pump();
        const std::wstring wtitle(title.begin(), title.end());
        HWND top = FindWindowW(nullptr, wtitle.c_str());
        HWND h = top ? one_of(top, sidebar ? L"ListBox" : WC_TABCONTROLW) : nullptr;
        expect(t && h, kind + "the window, never shown, and its " + (sidebar ? "list" : "tab control"));
        if (!t || !h) return;
        changes.clear();
        body(run{t, top, h, built, changes, kind});
    };
    const hook to_c = [](rap::tabs* t) { t->set_current(2); };
    const hook hide_d = [](rap::tabs* t) { t->set_page_visible(3, false); };

    scenario("set_current re-entered", {nullptr, to_c, nullptr, nullptr},
             [&](const run& r)
             {
                 r.t->set_current(1);
                 pump();
                 expect(r.changes.empty(), r.kind + "no on_changed, " + seq(r.changes));
                 expect(r.t->current() == 2 && selected(r.h, sidebar) == "C",
                        r.kind + "C current and selected, " + str(r.t->current()) + ", " + selected(r.h, sidebar));
                 expect(showing(r, 2), r.kind + "C shown");
                 expect(r.built[1] != nullptr, r.kind + "B built");
             });
    scenario("set_page_visible re-entered", {nullptr, nullptr, hide_d, nullptr},
             [&](const run& r)
             {
                 r.t->set_current(1);
                 pump();
                 r.changes.clear();
                 r.t->set_page_visible(1, false);
                 pump();
                 expect(r.changes.empty(), r.kind + "no on_changed, " + seq(r.changes));
                 expect(r.t->current() == 2 && selected(r.h, sidebar) == "C" && showing(r, 2),
                        r.kind + "C current, selected and shown, " + str(r.t->current()) + ", " +
                            selected(r.h, sidebar));
                 expect(!r.t->page_visible(3) && headers(r.h, sidebar) == "AC",
                        r.kind + "D hidden, its header gone: " + headers(r.h, sidebar));
             });
    scenario("a user's switch to a lazy page", {},
             [&](const run& r)
             {
                 user_select(r, 1);
                 expect(r.changes == std::vector<int>{1}, r.kind + "on_changed once, with 1, " + seq(r.changes));
                 expect(r.t->current() == 1 && selected(r.h, sidebar) == "B" && r.built[1] && showing(r, 1),
                        r.kind + "B current, built and shown, " + str(r.t->current()));
             });
    scenario("a user's switch re-entered", {nullptr, to_c, nullptr, nullptr},
             [&](const run& r)
             {
                 user_select(r, 1);
                 expect(r.changes == std::vector<int>{2}, r.kind + "on_changed once, with 2, " + seq(r.changes));
                 expect(r.t->current() == 2 && selected(r.h, sidebar) == "C" && showing(r, 2),
                        r.kind + "C current, selected and shown, " + str(r.t->current()) + ", " +
                            selected(r.h, sidebar));
                 expect(r.built[1] != nullptr, r.kind + "B built");
             });
    // Every page hidden, then the lazy B shown: B made current and built,
    // and its builder shows C and switches to it. The page shown is the one
    // current after the build: C, selected too; no on_changed.
    const hook show_c = [](rap::tabs* t)
    {
        t->set_page_visible(2, true);
        t->set_current(2);
    };
    scenario("shown first, re-entered", {nullptr, show_c, nullptr, nullptr},
             [&](const run& r)
             {
                 for (int i : {3, 2, 1, 0}) r.t->set_page_visible(i, false);
                 pump();
                 expect(r.t->current() == -1 && !r.built[1], r.kind + "every page hidden: none, B unbuilt");
                 r.t->set_page_visible(1, true);
                 pump();
                 expect(r.changes.empty(), r.kind + "no on_changed, " + seq(r.changes));
                 expect(r.t->current() == 2 && selected(r.h, sidebar) == "C",
                        r.kind + "C current and selected, " + str(r.t->current()) + ", " + selected(r.h, sidebar));
                 expect(showing(r, 2), r.kind + "C shown");
                 expect(r.built[1] != nullptr, r.kind + "B built");
             });
    // A user's switch to the lazy B, whose builder hides every page: none
    // current, and no on_changed (never -1).
    const hook hide_all = [](rap::tabs* t)
    {
        for (int i : {0, 2, 3, 1}) t->set_page_visible(i, false);
    };
    scenario("a user's switch, every page hidden", {nullptr, hide_all, nullptr, nullptr},
             [&](const run& r)
             {
                 user_select(r, 1);
                 expect(r.changes.empty(), r.kind + "no on_changed, " + seq(r.changes));
                 expect(r.t->current() == -1 && selected(r.h, sidebar) == "-" && showing(r, -1),
                        r.kind + "none current, none selected, no page shown, " + str(r.t->current()));
             });
    // Shown first with every page hidden, its builder hiding every page
    // again: none current, nothing shown, no on_changed.
    scenario("shown first, its builder hiding every page", {nullptr, hide_all, nullptr, nullptr},
             [&](const run& r)
             {
                 for (int i : {3, 2, 1, 0}) r.t->set_page_visible(i, false);
                 pump();
                 r.t->set_page_visible(1, true);  // B's builder hides every page again
                 pump();
                 expect(r.changes.empty(), r.kind + "no on_changed, " + seq(r.changes));
                 expect(r.t->current() == -1 && selected(r.h, sidebar) == "-" && showing(r, -1),
                        r.kind + "none current, none selected, no page shown, " + str(r.t->current()));
             });
    // A user's switch to the lazy B, whose builder switches back to A, the
    // page current before: A current, selected and shown, no on_changed.
    const hook to_a = [](rap::tabs* t) { t->set_current(0); };
    scenario("a user's switch back where it was", {nullptr, to_a, nullptr, nullptr},
             [&](const run& r)
             {
                 user_select(r, 1);
                 expect(r.changes.empty(), r.kind + "no on_changed, " + seq(r.changes));
                 expect(r.t->current() == 0 && selected(r.h, sidebar) == "A" && showing(r, 0),
                        r.kind + "A current, selected and shown, " + str(r.t->current()) + ", " +
                            selected(r.h, sidebar));
                 expect(r.built[1] != nullptr, r.kind + "B built");
             });
}

// Hidden from the start: no header, never shown, a lazy page unbuilt.
void hidden_from_start(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "sidebar, from the start: " : "strip, from the start: ";
    const std::wstring title = sidebar ? L"Start sidebar" : L"Start strip";
    rap::tabs* t = nullptr;
    rap::label* lazy_label = nullptr;
    auto win = ui.create(
        window{narrow(title),
               {tabs{page{"A", lazy{[&] { return rap::node(label{"page a"}.bind(lazy_label)); }}}.visible(false),
                     page{"B", label{"page b"}}.visible(false),
                     {"C", label{"page c"}},
                     {"D", label{"page d"}}}
                    .sidebar(sidebar)
                    .bind(t)}});
    pump();
    HWND top = FindWindowW(nullptr, title.c_str());
    HWND h = top ? one_of(top, sidebar ? L"ListBox" : WC_TABCONTROLW) : nullptr;
    expect(t && h, kind + "the window and its control");
    if (!t || !h) return;
    expect(headers(h, sidebar) == "CD", kind + "headers CD, " + headers(h, sidebar));
    expect(t->current() == 2 && selected(h, sidebar) == "C", kind + "C current, " + str(t->current()));
    expect(lazy_label == nullptr, kind + "the lazy A unbuilt");
    expect(!shown(with_text(top, L"page b")) && shown(with_text(top, L"page c")), kind + "B never shown, C shown");
    t->set_page_visible(0, true);
    pump();
    expect(headers(h, sidebar) == "ACD" && t->current() == 2 && lazy_label == nullptr,
           kind + "A shown: first header, C stays current, A still unbuilt");
    t->set_current(0);
    pump();
    expect(t->current() == 0 && lazy_label != nullptr && shown(with_text(top, L"page a")),
           kind + "A made current: built and shown");
}

// A sidebar's natural height: its visible rows'.
void sidebar_rows(rap::ui& ui)
{
    using namespace rap::layout;
    rap::tabs* t = nullptr;
    auto win = ui.create(window{"Sidebar rows",
                                {tabs{{"One", label{"1"}},
                                      {"Two", label{"2"}},
                                      {"Three", label{"3"}},
                                      {"Four", label{"4"}},
                                      {"Five", label{"5"}},
                                      {"Six", label{"6"}},
                                      {"Seven", label{"7"}},
                                      {"Eight", label{"8"}}}
                                     .sidebar()
                                     .bind(t)}});
    pump();
    HWND top = FindWindowW(nullptr, L"Sidebar rows");
    HWND box = top ? one_of(top, L"ListBox") : nullptr;
    expect(t && box, "sidebar rows: the window and its list");
    if (!t || !box) return;
    const long row = long(SendMessageW(box, LB_GETITEMHEIGHT, 0, 0));
    const SIZE all = least(top);
    for (int i : {2, 4, 6}) t->set_page_visible(i, false);
    pump();
    const SIZE fewer = least(top);
    expect(fewer.cy == all.cy - 3 * row, "sidebar rows: three of eight hidden, the natural height three rows less: " +
                                             str(all.cy) + " to " + str(fewer.cy) + ", rows " + str(row));
    for (int i : {2, 4, 6}) t->set_page_visible(i, true);
    pump();
    expect(least(top).cy == all.cy, "sidebar rows: shown again, as tall as before: " + str(least(top).cy));
}

// hide_single_tab: three pages, two hidden from the start.
void single_tab(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::string kind = sidebar ? "sidebar, hide_single_tab: " : "strip, hide_single_tab: ";
    const std::wstring title = sidebar ? L"Single sidebar" : L"Single strip";
    const std::vector<rap::column> cols{{"Name", 120_px}};
    rap::tabs* t = nullptr;
    auto win = ui.create(window{narrow(title),
                                {tabs{page{"A", list{cols, &g_model}}.visible(false),
                                      {"B", list{cols, &g_model}},
                                      page{"C", list{cols, &g_model}}.visible(false)}
                                     .sidebar(sidebar)
                                     .hide_single_tab()
                                     .bind(t)}});
    pump();
    HWND top = FindWindowW(nullptr, title.c_str());
    HWND h = top ? one_of(top, sidebar ? L"ListBox" : WC_TABCONTROLW) : nullptr;
    const std::vector<HWND> lists = top ? all_of(top, WC_LISTVIEWW) : std::vector<HWND>{};
    expect(t && h && lists.size() == 3, kind + "the window, its control and three lists");
    if (!t || !h || lists.size() != 3) return;
    // The pages' lists in page order: the one showing is B's.
    auto showing = [&]
    {
        HWND found = nullptr;
        int n = 0;
        for (HWND l : lists)
            if (shown(l)) found = l, ++n;
        return n == 1 ? found : nullptr;
    };
    RECT client{};
    GetClientRect(top, &client);
    HWND b = showing();
    expect(!shown(h) && b != nullptr, kind + "one page visible: no " + (sidebar ? "list" : "strip") + ", B's page shown");
    if (!b) return;
    // The page fills the area: the window's margins all round it, nothing
    // above it or beside it.
    RECT r = rect_in(top, b);
    expect(r.left == r.top && client.right - r.right == r.left && client.bottom - r.bottom == r.top,
           kind + "the page fills the area: " + str(r.left) + "," + str(r.top) + " " + str(r.right) + "," +
               str(r.bottom) + " in " + str(client.right) + "x" + str(client.bottom));
    const SIZE without = least(top);

    // A second page shown: the strip back.
    t->set_page_visible(2, true);
    pump();
    expect(shown(h) && headers(h, sidebar) == "BC", kind + "C shown: the " + (sidebar ? "list" : "strip") +
                                                         " back, headers BC");
    expect(t->current() == 1 && showing() == b, kind + "B still current and showing");
    r = rect_in(top, b);
    expect(sidebar ? r.left > r.top + 20 : r.top > r.left + 10,
           kind + "the page beside the " + (sidebar ? "list" : "strip") + ": " + str(r.left) + "," + str(r.top));
    const SIZE with = least(top);
    if (sidebar)
        expect(with.cx > without.cx + 50, kind + "natural width with the list " + str(with.cx) + ", without " +
                                              str(without.cx));
    else
        expect(with.cy > without.cy + 10, kind + "natural height with the strip " + str(with.cy) + ", without " +
                                              str(without.cy));

    // Hidden again: gone again.
    t->set_page_visible(2, false);
    pump();
    expect(!shown(h) && showing() == b, kind + "C hidden again: no " + (sidebar ? "list" : "strip"));
    const SIZE again = least(top);
    expect(again.cx == without.cx && again.cy == without.cy,
           kind + "natural size without it again: " + str(again.cx) + "x" + str(again.cy));
    // Every page hidden: still none; one shown: still none.
    t->set_page_visible(1, false);
    pump();
    expect(!shown(h) && t->current() == -1 && !showing(), kind + "every page hidden: no strip, no page");
    t->set_page_visible(0, true);
    pump();
    expect(!shown(h) && t->current() == 0 && showing() == lists[0], kind + "A shown alone: A current, no strip");
}

// #92: a canvas 300x200 as a page's whole content: the container measures
// to include it (it was 64x64, the canvas's default).
void fixed_canvas(rap::ui& ui, int kind)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const char* names[] = {"a tab page", "a sidebar page", "a stack page"};
    const std::wstring titles[] = {L"Canvas tab", L"Canvas sidebar", L"Canvas stack"};
    const std::wstring& title = titles[kind];
    const rap::node canvas_node = canvas{}.width(300_px).height(200_px);
    rap::node root = kind == 2 ? rap::node(stack{{"one", canvas_node}})
                               : rap::node(tabs{{"One", canvas_node}, {"Two", label{"two"}}}.sidebar(kind == 1));
    auto win = ui.create(window{narrow(title), {root}});
    pump();
    HWND top = FindWindowW(nullptr, title.c_str());
    expect(top != nullptr, std::string("#92, ") + names[kind] + ": the window");
    if (!top) return;
    const UINT dpi = GetDpiForWindow(top);
    const long w = MulDiv(300, int(dpi), 96), h = MulDiv(200, int(dpi), 96);
    const SIZE n = least(top);
    expect(n.cx >= w && n.cy >= h, std::string("#92, ") + names[kind] + ": measured to include the canvas " +
                                       str(w) + "x" + str(h) + ": " + str(n.cx) + "x" + str(n.cy));
}

// -- page icons and titles (#101) ---------------------------------------------

// The messages that change a tab control or a list box: one sent to it is a
// native write.
std::map<UINT, int> g_writes;
LRESULT CALLBACK count_writes(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    switch (msg)
    {
    case TCM_SETITEMW:
    case TCM_INSERTITEMW:
    case TCM_DELETEITEM:
    case TCM_SETIMAGELIST:
    case TCM_SETCURSEL:
    case LB_DELETESTRING:
    case LB_INSERTSTRING:
    case LB_SETCURSEL:
    case LB_SETTOPINDEX:
    case LB_SETITEMHEIGHT:
    case WM_SETREDRAW: ++g_writes[msg]; break;
    default: break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}
std::string writes()
{
    std::string out;
    for (const auto& [m, n] : g_writes) out += " 0x" + std::to_string(m) + "x" + std::to_string(n);
    return out.empty() ? " none" : out;
}

// An icon of one colour, filling its square; or of the theme's text colour.
rap::icon solid(rap::color c)
{
    return rap::icon::drawn([c](rap::painter& p, rap::size_f s)
                            { p.rect({0, 0, s.w, s.h}, 0, rap::paint{c}, std::nullopt); });
}
rap::icon inked()
{
    return rap::icon::drawn([](rap::painter& p, rap::size_f s)
                            { p.rect({0, 0, s.w, s.h}, 0, rap::paint{rap::theme_color::window_text}, std::nullopt); });
}
const rap::color red{255, 0, 0}, blue{0, 0, 255};

// What's drawn in a square of pixels: red, blue, near black, near white,
// and anything unlike the background.
struct tally
{
    int red = 0, blue = 0, dark = 0, light = 0, any = 0;
    std::string str() const
    {
        return "red " + std::to_string(red) + " blue " + std::to_string(blue) + " dark " + std::to_string(dark) +
               " light " + std::to_string(light) + " any " + std::to_string(any);
    }
};

// A 32-bit DIB `w` x `h` selected into a memory DC, filled with `bg`.
struct dib
{
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* bits = nullptr;
    int w = 0, h = 0;
    dib(int width, int height, uint32_t bg)
        : w(width)
        , h(height)
    {
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* p = nullptr;
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
        bits = static_cast<uint32_t*>(p);
        old = SelectObject(dc, bmp);
        for (int i = 0; bits && i < w * h; ++i) bits[i] = bg;
    }
    ~dib()
    {
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    tally count(RECT r, uint32_t bg) const
    {
        GdiFlush();
        tally t;
        for (int y = std::max(0L, r.top); bits && y < std::min(long(h), r.bottom); ++y)
            for (int x = std::max(0L, r.left); x < std::min(long(w), r.right); ++x)
            {
                const uint32_t p = bits[y * w + x] & 0xFFFFFF;
                const int R = (p >> 16) & 0xFF, G = (p >> 8) & 0xFF, B = p & 0xFF;
                if (R > 180 && G < 80 && B < 80) ++t.red;
                if (B > 180 && R < 80 && G < 80) ++t.blue;
                if (R < 60 && G < 60 && B < 60) ++t.dark;
                if (R > 200 && G > 200 && B > 200) ++t.light;
                if (p != (bg & 0xFFFFFF)) ++t.any;
            }
        return t;
    }
};

const uint32_t grey = 0x808080;

// A strip's tab's image, as its image list has it, drawn on grey; none for
// a tab without one (TCIF_IMAGE -1). `size`: the list's icon size.
tally strip_image(HWND h, int item, int* size = nullptr)
{
    TCITEMW it{};
    it.mask = TCIF_IMAGE;
    HIMAGELIST list = TabCtrl_GetImageList(h);
    if (!TabCtrl_GetItem(h, item, &it) || it.iImage < 0 || !list) return {};
    int w = 0, hh = 0;
    ImageList_GetIconSize(list, &w, &hh);
    if (size) *size = w;
    dib d(w, hh, grey);
    ImageList_Draw(list, it.iImage, d.dc, 0, 0, ILD_TRANSPARENT);
    return d.count({0, 0, w, hh}, grey);
}
// A sidebar's row as its owner drawing paints it (WM_DRAWITEM to its
// parent, unselected): what's in its icon's square, 8 px in, 16 square.
tally side_image(HWND list, int item, UINT dpi)
{
    RECT r{};
    if (SendMessageW(list, LB_GETITEMRECT, WPARAM(item), reinterpret_cast<LPARAM>(&r)) == LB_ERR) return {};
    const int w = r.right - r.left, hh = r.bottom - r.top;
    dib d(w, hh, 0xFF00FF);
    DRAWITEMSTRUCT di{};
    di.CtlType = ODT_LISTBOX;
    di.CtlID = UINT(GetDlgCtrlID(list));
    di.itemID = UINT(item);
    di.itemAction = ODA_DRAWENTIRE;
    di.itemState = 0;
    di.hwndItem = list;
    di.hDC = d.dc;
    di.rcItem = {0, 0, w, hh};
    SendMessageW(GetParent(list), WM_DRAWITEM, WPARAM(di.CtlID), reinterpret_cast<LPARAM>(&di));
    GdiFlush();
    const uint32_t bg = d.bits ? d.bits[w - 1] : 0;  // the row's background, at its far end
    const int at = MulDiv(8, int(dpi), 96), size = MulDiv(16, int(dpi), 96);
    return d.count({at, (hh - size) / 2, at + size, (hh + size) / 2}, bg);
}
// What the tab control paints (WM_PRINTCLIENT: its own drawing, or
// rapport's in dark mode) within tab `item`.
tally strip_painted(HWND h, int item)
{
    RECT c{}, r{};
    GetClientRect(h, &c);
    if (!TabCtrl_GetItemRect(h, item, &r) || c.right <= 0 || c.bottom <= 0) return {};
    dib d(c.right, c.bottom, 0xFF00FF);
    SendMessageW(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(d.dc), PRF_CLIENT);
    return d.count(r, 0xFF00FF);
}
// A header's title: a strip's tab's text, or a sidebar's row's string.
std::string title_of(HWND h, bool sidebar, int item)
{
    wchar_t t[128]{};
    if (sidebar)
    {
        if (SendMessageW(h, LB_GETTEXTLEN, WPARAM(item), 0) >= 128) return "?";
        SendMessageW(h, LB_GETTEXT, WPARAM(item), reinterpret_cast<LPARAM>(t));
    }
    else
    {
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = t;
        it.cchTextMax = 128;
        SendMessageW(h, TCM_GETITEMW, WPARAM(item), reinterpret_cast<LPARAM>(&it));
    }
    return narrow(t);
}
int selected_item(HWND h, bool sidebar)
{
    return sidebar ? int(SendMessageW(h, LB_GETCURSEL, 0, 0)) : TabCtrl_GetCurSel(h);
}
long width_of(HWND h)
{
    RECT r{};
    GetWindowRect(h, &r);
    return r.right - r.left;
}
// A tab's width (a strip's), or the list's (a sidebar's).
long header_width(HWND h, bool sidebar, int item)
{
    RECT r{};
    if (sidebar) return width_of(h);
    return TabCtrl_GetItemRect(h, item, &r) ? r.right - r.left : -1;
}

// #101: a page's icon and title, from the start and at runtime. Pages A
// (named go-up), B (drawn red), C and D (none). Read natively: a strip's
// tab's TCIF_IMAGE and its image in the control's image list (TCM_GETIMAGELIST),
// what the control paints (WM_PRINTCLIENT); a sidebar's rows as its owner
// drawing paints them (WM_DRAWITEM), LB_GETTEXT. Writes counted on the
// control (TCM_SETITEM and the like; LB_DELETESTRING and the like), and
// relayouts (static builds).
void page_icons(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "sidebar, icons (#101): " : "strip, icons (#101): ";
    const std::wstring title = sidebar ? L"Icons sidebar" : L"Icons strip";
    rap::tabs* t = nullptr;
    int changes = 0;
    auto win = ui.create(window{narrow(title),
                                {tabs{page{"A", label{"page a"}}.icon(rap::icon::named("go-up")),
                                      page{"B", label{"page b"}}.icon(solid(red)),
                                      {"C", label{"page c"}},
                                      {"D", label{"page d"}}}
                                     .sidebar(sidebar)
                                     .on_changed([&](int) { ++changes; })
                                     .bind(t)}});
    pump();
    HWND top = FindWindowW(nullptr, title.c_str());
    HWND h = top ? one_of(top, sidebar ? L"ListBox" : WC_TABCONTROLW) : nullptr;
    expect(t && h, kind + "the window, never shown, and its " + (sidebar ? "list" : "tab control"));
    if (!t || !h) return;
    const UINT dpi = GetDpiForWindow(top);
    auto look = [&](int item) { return sidebar ? side_image(h, item, dpi) : strip_image(h, item); };
    SetWindowSubclass(h, count_writes, 0x101, 0);

    // From the start: A's named icon, B's drawn red one, C none.
    tally a = look(0), b = look(1), c = look(2);
    expect(a.any > 0, kind + "A's named icon from the start: " + a.str());
    expect(b.red > 0 && b.blue == 0, kind + "B's drawn icon from the start, red: " + b.str());
    expect(c.any == 0, kind + "C has none: " + c.str());
    if (!sidebar)
    {
        int size = 0;
        strip_image(h, 1, &size);
        expect(size == MulDiv(16, int(dpi), 96), kind + "the image list's icons at the DPI's small-icon size, " +
                                                     str(size));
        const tally painted = strip_painted(h, 1);
        expect(painted.red > 0, kind + "B's tab painted with its icon: " + painted.str());
    }

    // Set at runtime: C's, blue.
    t->set_page_icon(2, solid(blue));
    pump();
    c = look(2);
    expect(c.blue > 0 && c.red == 0, kind + "C given a blue icon: " + c.str());

    // Two drawn icons swapped (B's red for blue): no relayout, the same width.
    unsigned before = layouts();
    long wide = header_width(h, sidebar, 1);
    t->set_page_icon(1, solid(blue));
    pump();
    b = look(1);
    expect(b.blue > 0 && b.red == 0, kind + "B's icon swapped for a blue one: " + b.str());
    expect(layouts() == before, kind + "a drawn icon swapped: no relayout, " + str(long(layouts() - before)));
    expect(header_width(h, sidebar, 1) == wide,
           kind + "a drawn icon swapped: the same width, " + str(wide) + " to " + str(header_width(h, sidebar, 1)));

    // Removed: A's.
    t->set_page_icon(0, rap::icon{});
    pump();
    a = look(0);
    expect(a.any == 0, kind + "A's icon removed: " + a.str());
    if (!sidebar)
    {
        TCITEMW it{};
        it.mask = TCIF_IMAGE;
        TabCtrl_GetItem(h, 0, &it);
        expect(it.iImage == -1, kind + "A's tab without an image, " + str(it.iImage));
    }

    // The title: a short one; then a long one, which widens a sidebar (and
    // lays it out) but not a strip's measure (nothing laid out).
    before = layouts();
    t->set_page_title(3, "Dee");
    pump();
    expect(title_of(h, sidebar, 3) == "Dee", kind + "D's title now Dee: " + title_of(h, sidebar, 3));
    expect(layouts() == before, kind + "a short title: no relayout, " + str(long(layouts() - before)));
    const std::string long_title = "A much longer title for the fourth page";
    const long list_before = width_of(h);
    t->set_current(3);  // its own header's title changed: still selected
    pump();
    before = layouts();
    t->set_page_title(3, long_title);
    pump();
    const unsigned retitled = layouts() - before;
    const long list_after = width_of(h);
    expect(t->current() == 3 && selected_item(h, sidebar) == 3,
           kind + "D (current) retitled: still current and selected, " + str(selected_item(h, sidebar)));
    t->set_current(0);
    pump();
    expect(title_of(h, sidebar, 3) == long_title, kind + "D's long title: " + title_of(h, sidebar, 3));
    if (sidebar)
        expect(laid_out(retitled) && list_after > list_before,
               kind + "a long title: laid out, the list wider, " + str(list_before) + " to " + str(list_after));
    else
        expect(retitled == 0, kind + "a long title: the strip's measure the same, no relayout, " + str(long(retitled)));
    expect(t->current() == 0 && selected(h, sidebar) == "A", kind + "A still current and selected");

    // The same title, or the same named icon, again: no native write, no
    // relayout (nothing to paint either: page_icons_shown).
    t->set_page_icon(2, rap::icon::named("go-down"));
    pump();
    c = look(2);
    expect(c.any > 0 && c.blue == 0, kind + "C's icon now a named one: " + c.str());
    g_writes.clear();
    before = layouts();
    t->set_page_title(3, long_title);
    t->set_page_icon(2, rap::icon::named("go-down"));
    pump();
    expect(g_writes.empty() && layouts() == before, kind + "the same title and named icon again: no write," +
                                                        writes() + ", no relayout, " + str(long(layouts() - before)));
    // A drawn one can't be compared: B's made again, nothing written to the
    // control, nothing laid out.
    g_writes.clear();
    before = layouts();
    t->set_page_icon(1, solid(blue));
    pump();
    expect(g_writes.empty() && layouts() == before, kind + "a drawn icon again: no write," + writes() +
                                                        ", no relayout, " + str(long(layouts() - before)));

    // Hidden: a title and an icon set meanwhile kept, shown with the page.
    // (C hidden first: B's header's place is then D's, which stays, so a
    // title written to a hidden page's place would show there.)
    t->set_page_visible(2, false);  // C's named icon, kept through hiding
    t->set_page_visible(1, false);
    pump();
    t->set_page_title(1, "Bee");
    t->set_page_icon(1, solid(red));
    pump();
    expect(title_of(h, sidebar, 1) == long_title, kind + "B and C hidden: D's the second header, " +
                                                      title_of(h, sidebar, 1));
    t->set_page_visible(1, true);
    t->set_page_visible(2, true);
    pump();
    expect(title_of(h, sidebar, 1) == "Bee", kind + "B shown again with the title set while hidden: " +
                                                 title_of(h, sidebar, 1));
    expect(title_of(h, sidebar, 0) == "A" && title_of(h, sidebar, 2) == "C" && title_of(h, sidebar, 3) == long_title,
           kind + "the others' titles untouched by it: " + title_of(h, sidebar, 0) + ", " + title_of(h, sidebar, 2) +
               ", " + title_of(h, sidebar, 3));
    b = look(1);
    expect(b.red > 0 && b.blue == 0, kind + "B shown again with the icon set while hidden, red: " + b.str());
    c = look(2);
    expect(c.any > 0 && c.blue == 0, kind + "C shown again with its icon: " + c.str());

    // Out of range: nothing.
    g_writes.clear();
    before = layouts();
    t->set_page_icon(4, solid(red));
    t->set_page_icon(-1, solid(red));
    t->set_page_title(4, "nothing");
    t->set_page_title(-1, "nothing");
    pump();
    expect(g_writes.empty() && layouts() == before && title_of(h, sidebar, 0) == "A" && look(0).any == 0,
           kind + "pages out of range: nothing written or laid out," + writes());
    expect(changes == 0, kind + "no on_changed from any of it, " + str(changes));

    // A sidebar's icons' column: the last icon gone, the list narrower by
    // it (16 and the gap, 8), the long title the widest.
    if (sidebar)
    {
        const long with = width_of(h);
        before = layouts();
        t->set_page_icon(1, rap::icon{});
        t->set_page_icon(2, rap::icon{});
        pump();
        const long without = width_of(h);
        expect(laid_out(layouts() - before) && with - without == MulDiv(16, int(dpi), 96) + MulDiv(8, int(dpi), 96),
               kind + "every icon removed: the list narrower by the icons' column, " + str(with) + " to " +
                   str(without));
    }
    RemoveWindowSubclass(h, count_writes, 0x101);
}

// A sidebar's row as drawn (WM_DRAWITEM, unselected): the leftmost column
// from `from` on with anything unlike the row's background (-1: none): the
// title's start, past the icon's square.
int row_ink_x(HWND list, int item, int from)
{
    RECT r{};
    if (SendMessageW(list, LB_GETITEMRECT, WPARAM(item), reinterpret_cast<LPARAM>(&r)) == LB_ERR) return -2;
    const int w = r.right - r.left, hh = r.bottom - r.top;
    dib d(w, hh, 0xFF00FF);
    DRAWITEMSTRUCT di{};
    di.CtlType = ODT_LISTBOX;
    di.CtlID = UINT(GetDlgCtrlID(list));
    di.itemID = UINT(item);
    di.itemAction = ODA_DRAWENTIRE;
    di.hwndItem = list;
    di.hDC = d.dc;
    di.rcItem = {0, 0, w, hh};
    SendMessageW(GetParent(list), WM_DRAWITEM, WPARAM(di.CtlID), reinterpret_cast<LPARAM>(&di));
    GdiFlush();
    if (!d.bits) return -2;
    const uint32_t bg = d.bits[w - 1] & 0xFFFFFF;
    for (int x = from; x < w; ++x)
        for (int y = 0; y < hh; ++y)
            if ((d.bits[y * w + x] & 0xFFFFFF) != bg) return x;
    return -1;
}

// A path's own icon (icon::for_path) with no fallback, on a path that has
// none, pending and once found to be none: it still takes the icon's place.
// A strip's tab keeps its image (an image list entry) and its width; a
// sidebar keeps its icons' column: the list's width and the titles' start
// (its own row's and another's) as with a named icon, at once and after
// the lookup's had time.
void path_icon_slot(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "sidebar, a path's icon pending: " : "strip, a path's icon pending: ";
    const std::wstring title = sidebar ? L"Path slot sidebar" : L"Path slot strip";
    rap::tabs* t = nullptr;
    auto win = ui.create(window{narrow(title),
                                {tabs{{"A", label{"page a"}},
                                      page{"Build status", label{"page b"}}.icon(rap::icon::named("go-up")),
                                      {"C", label{"page c"}}}
                                     .sidebar(sidebar)
                                     .bind(t)}});
    pump();
    HWND top = FindWindowW(nullptr, title.c_str());
    HWND h = top ? one_of(top, sidebar ? L"ListBox" : WC_TABCONTROLW) : nullptr;
    expect(t && h, kind + "the window and its control");
    if (!t || !h) return;
    const UINT dpi = GetDpiForWindow(top);
    const int past_icon = MulDiv(8 + 16, int(dpi), 96);
    struct shape
    {
        long width;
        int image, own_x, other_x;
    };
    auto now = [&]
    {
        shape s{header_width(h, sidebar, 1), -1, -1, -1};
        if (sidebar)
        {
            s.own_x = row_ink_x(h, 1, past_icon);
            s.other_x = row_ink_x(h, 2, 0);
        }
        else
        {
            TCITEMW it{};
            it.mask = TCIF_IMAGE;
            TabCtrl_GetItem(h, 1, &it);
            s.image = it.iImage;
        }
        return s;
    };
    auto say = [](const shape& s)
    {
        return "width " + str(s.width) + ", image " + str(s.image) + ", title at " + str(s.own_x) + ", another's at " +
               str(s.other_x);
    };
    const shape named = now();
    t->set_page_icon(1, rap::icon::for_path("C:\\rapport-no-such-path-101\\nothing-here"));
    const shape at_once = now();
    pump(40);
    const shape later = now();
    for (const auto& [when, s] : {std::pair{"at once", at_once}, std::pair{"after the lookup", later}})
        expect(s.width == named.width && s.image == named.image && s.own_x == named.own_x &&
                   s.other_x == named.other_x && (sidebar || s.image >= 0),
               kind + "a named icon swapped for a path's with none, " + when + ": as with the named one, " +
                   say(named) + "; now " + say(s));
}

// #101 in dark mode: a strip's tabs painted by rapport (draw_dark_tabs),
// each icon with its title; and a drawn icon in the theme's text colour
// made again in the new theme's.
void page_icons_dark(rap::ui& ui)
{
    using namespace rap::layout;
    const std::string kind = "strip, icons, dark (#101): ";
    rap::tabs* t = nullptr;
    auto win = ui.create(window{"Icons dark",
                                {tabs{page{"A", label{"page a"}}.icon(inked()),
                                      page{"B", label{"page b"}}.icon(solid(red)),
                                      {"C", label{"page c"}}}
                                     .bind(t)}});
    pump();
    HWND top = FindWindowW(nullptr, L"Icons dark");
    HWND h = top ? one_of(top, WC_TABCONTROLW) : nullptr;
    expect(t && h, kind + "the window and its tab control");
    if (!t || !h) return;
    ui.set_color_scheme(rap::color_scheme::light);
    pump();
    const tally light = strip_image(h, 0);
    ui.set_color_scheme(rap::color_scheme::dark);
    pump();
    const tally dark = strip_image(h, 0);
    expect(light.dark > 0 && light.light == 0 && dark.light > 0 && dark.dark == 0,
           kind + "A's icon, in the text colour, made again for the dark theme: light " + light.str() + "; dark " +
               dark.str());
    const tally painted = strip_painted(h, 1);
    expect(painted.red > 0, kind + "B's tab painted with its icon: " + painted.str());
    ui.set_color_scheme(rap::color_scheme::system);
    pump();
}

}  // namespace

// --shown: on a shown window (RAPPORT_NO_ACTIVATE: it takes no focus), a
// page shown (eager, larger than the others) and the strip back
// (hide_single_tab, a second page shown) grow its least size at once
// (relayout_at_once measures anew, as relayout does). Only in this mode,
// which its own ctest runs.
void shown_window(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::string kind = sidebar ? "shown, sidebar: " : "shown, strip: ";
    const std::wstring title = sidebar ? L"Tabs shown sidebar" : L"Tabs shown strip";
    rap::tabs* t = nullptr;
    rap::tabs* single = nullptr;
    auto win = ui.create(
        window{narrow(title),
               {vbox{tabs{{"A", label{"a"}}, page{"B", canvas{}.width(500_px).height(360_px)}.visible(false),
                          {"C", label{"c"}}}
                         .sidebar(sidebar)
                         .bind(t),
                     tabs{{"X", label{"x"}}, page{"Y", label{"y"}}.visible(false)}
                         .sidebar(sidebar)
                         .hide_single_tab()
                         .bind(single)}}});
    win->show();
    pump();
    HWND top = FindWindowW(nullptr, title.c_str());
    expect(top && t && single, kind + "the window, shown");
    if (!top || !t || !single) return;
    const SIZE before = least(top);
    t->set_page_visible(1, true);  // eager: nothing to build, so no relayout() of its own
    pump();
    const SIZE after = least(top);
    expect(after.cx >= before.cx + 200 && after.cy >= before.cy + 150,
           kind + "the larger page shown: the least size grows, " + str(before.cx) + "x" + str(before.cy) + " to " +
               str(after.cx) + "x" + str(after.cy));
    t->set_page_visible(1, false);
    pump();
    const SIZE one = least(top);
    single->set_page_visible(1, true);  // a second page: the strip (or the list) back
    pump();
    const SIZE two = least(top);
    // (A sidebar's list beside the page: wider, or, the window's width
    // already the first tabs' wider sidebar, taller for its rows.)
    expect(sidebar ? (two.cx > one.cx || two.cy > one.cy) : two.cy > one.cy,
           kind + "hide_single_tab, a second page shown: the least size takes the strip, " + str(one.cx) + "x" +
               str(one.cy) + " to " + str(two.cx) + "x" + str(two.cy));
}

// #101, shown (where a window keeps what's left to paint): the same title,
// or the same named icon, again: nothing to paint; a drawn icon again (it
// can't be compared): its tab or row only, nothing laid out.
void page_icons_shown(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "shown, sidebar, icons (#101): " : "shown, strip, icons (#101): ";
    const std::wstring title = sidebar ? L"Icons shown sidebar" : L"Icons shown strip";
    rap::tabs* t = nullptr;
    auto win = ui.create(window{narrow(title),
                                {tabs{page{"A", label{"page a"}}.icon(rap::icon::named("go-up")),
                                      page{"B", label{"page b"}}.icon(solid(red)),
                                      {"C", label{"page c"}}}
                                     .sidebar(sidebar)
                                     .bind(t)}});
    win->show();
    pump();
    HWND top = FindWindowW(nullptr, title.c_str());
    HWND h = top ? one_of(top, sidebar ? L"ListBox" : WC_TABCONTROLW) : nullptr;
    expect(t && h, kind + "the window, shown, and its " + (sidebar ? "list" : "tab control"));
    if (!t || !h) return;
    UpdateWindow(top);
    const unsigned before = layouts();
    ValidateRect(h, nullptr);
    t->set_page_title(2, "C");
    t->set_page_icon(0, rap::icon::named("go-up"));
    RECT dirty{};
    const bool to_paint = GetUpdateRect(h, &dirty, FALSE) != FALSE;
    expect(!to_paint && layouts() == before, kind + "the same title and named icon again: nothing to paint, " +
                                                 (to_paint ? "some" : "none") + ", no relayout, " +
                                                 str(long(layouts() - before)));
    ValidateRect(h, nullptr);
    t->set_page_icon(1, solid(blue));
    const bool repainted = GetUpdateRect(h, &dirty, FALSE) != FALSE;
    RECT own{};
    if (sidebar)
        SendMessageW(h, LB_GETITEMRECT, 1, reinterpret_cast<LPARAM>(&own));
    else
        TabCtrl_GetItemRect(h, 1, &own);
    expect(repainted && dirty.left >= own.left && dirty.top >= own.top && dirty.right <= own.right &&
               dirty.bottom <= own.bottom && layouts() == before,
           kind + "a drawn icon swapped: only its " + (sidebar ? "row" : "tab") + " to paint, " + str(dirty.left) +
               "," + str(dirty.top) + " " + str(dirty.right) + "," + str(dirty.bottom) + " in " + str(own.left) + "," +
               str(own.top) + " " + str(own.right) + "," + str(own.bottom) + ", no relayout, " +
               str(long(layouts() - before)));
    pump();
}

// #102, shown: a sidebar of short titles (its list at its least width,
// which the icons' column doesn't change): the first icon, and the last
// removed, move every row's title, so every row is left to paint, not only
// the page's; with the page hidden too (its row gone, the others' still
// moving).
void icon_column_repaint(rap::ui& ui)
{
    using namespace rap::layout;
    const std::string kind = "shown, sidebar, the icons' column (#102): ";
    rap::tabs* t = nullptr;
    auto win = ui.create(window{"Icon column sidebar",
                                {tabs{{"A", label{"page a"}}, {"B", label{"page b"}}, {"C", label{"page c"}}}
                                     .sidebar()
                                     .bind(t)}});
    win->show();
    pump();
    HWND top = FindWindowW(nullptr, L"Icon column sidebar");
    HWND h = top ? one_of(top, L"ListBox") : nullptr;
    expect(t && h, kind + "the window, shown, and its list");
    if (!t || !h) return;
    // After `change`: what's left to paint covers rows `first` and `last`
    // (the others than the page's: the update rect is their union's at least).
    auto check = [&](const std::string& what, int first, int last, const std::function<void()>& change)
    {
        pump();
        UpdateWindow(top);
        const long w = width_of(h);
        ValidateRect(h, nullptr);
        change();
        RECT dirty{}, a{}, b{};
        const bool any = GetUpdateRect(h, &dirty, FALSE) != FALSE;
        SendMessageW(h, LB_GETITEMRECT, WPARAM(first), reinterpret_cast<LPARAM>(&a));
        SendMessageW(h, LB_GETITEMRECT, WPARAM(last), reinterpret_cast<LPARAM>(&b));
        expect(any && dirty.top <= a.top && dirty.bottom >= b.bottom && dirty.left <= a.left && dirty.right >= a.right,
               kind + what + ": rows " + str(first) + " to " + str(last) + " left to paint, " + str(dirty.left) + "," +
                   str(dirty.top) + " " + str(dirty.right) + "," + str(dirty.bottom) + " over " + str(a.top) + " to " +
                   str(b.bottom));
        expect(width_of(h) == w, kind + what + ": the list's width the same (its least), " + str(w) + " to " +
                                     str(width_of(h)));
    };
    check("B's, the first icon", 0, 2, [&] { t->set_page_icon(1, solid(red)); });
    check("B's, the last icon, removed", 0, 2, [&] { t->set_page_icon(1, rap::icon{}); });
    t->set_page_visible(1, false);
    check("hidden B's, the first icon", 0, 1, [&] { t->set_page_icon(1, solid(red)); });
    check("hidden B's, the last icon, removed", 0, 1, [&] { t->set_page_icon(1, rap::icon{}); });
    pump();
}

int main(int argc, char** argv)
{
    rap::load_options options{.backend = "win32"};
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui)
    {
        std::printf("no win32 backend: %s\n", ui.error().c_str());
        return 77;
    }
    if (argc > 1 && std::string(argv[1]) == "--shown")
    {
        for (const bool sidebar : {false, true}) shown_window(**ui, sidebar);
        for (const bool sidebar : {false, true}) page_icons_shown(**ui, sidebar);
        icon_column_repaint(**ui);
        std::printf(fails ? "%d failed\n" : "all passed\n", fails);
        return fails ? 1 : 0;
    }
    for (const bool sidebar : {false, true}) visibility(**ui, sidebar);
    for (const bool sidebar : {false, true}) hidden_from_start(**ui, sidebar);
    for (const bool sidebar : {false, true}) set_current_quiet(**ui, sidebar);
    for (const bool sidebar : {false, true}) reentrant(**ui, sidebar);
    sidebar_rows(**ui);
    for (const bool sidebar : {false, true}) single_tab(**ui, sidebar);
    for (int kind = 0; kind < 3; ++kind) fixed_canvas(**ui, kind);
    for (const bool sidebar : {false, true}) page_icons(**ui, sidebar);
    page_icons_dark(**ui);
    for (const bool sidebar : {false, true}) path_icon_slot(**ui, sidebar);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
