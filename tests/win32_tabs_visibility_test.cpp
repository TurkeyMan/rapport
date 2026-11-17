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

#include <array>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(                                                                                   \
    linker,                                                                                        \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
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
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
