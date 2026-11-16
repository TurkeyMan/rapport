// A setter given the value it has already does nothing (#88): no native
// call (no message that changes a control: WM_SETTEXT, BM_SETCHECK,
// CB_SETCURSEL, TTM_ADDTOOL and the like), no relayout (static builds: the
// backend's layout pass counter, through its test hook), no change
// notification. A changed value still changes it, and relayouts where it did
// (a label's text, a splitter's sizes).
//
// A never-shown window with one of each control; each setter set to a
// value, then the same value again, the second watched.
//   rapport-win32-setters-noop-test
#include <rapport/rapport.h>
#include "win32_test_no_dialogs.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which they need.
#include <commctrl.h>

#include <cstdio>
#include <functional>
#include <map>
#include <set>
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

int fails = 0;
void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
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

// The messages that change a control (or its tooltip): one sent to any of
// this thread's windows is a native call a setter made.
const std::set<UINT>& writes()
{
    static const std::set<UINT> w{WM_SETTEXT,       WM_ENABLE,           BM_SETCHECK,       BM_SETSTATE,
                                  BM_SETIMAGE,      EM_SETREADONLY,      EM_SETCUEBANNER,   CB_SETCURSEL,
                                  CB_RESETCONTENT,  CB_ADDSTRING,        TBM_SETPOS,        TBM_SETRANGEMIN,
                                  TBM_SETRANGEMAX,  PBM_SETPOS,          PBM_SETRANGE32,    DTM_SETSYSTEMTIME,
                                  TCM_SETCURSEL,    LB_SETCURSEL,        TVM_EXPAND,        TVM_SELECTITEM,
                                  TVM_SETITEMW,     LVM_SETITEMSTATE,    LVM_SETCOLUMNWIDTH, LVM_SETCOLUMNORDERARRAY,
                                  HDM_SETITEMW,     TB_CHECKBUTTON,      TB_ENABLEBUTTON,   TTM_ADDTOOLW,
                                  TTM_DELTOOLW,     LB_SETITEMHEIGHT,    WM_SETREDRAW};
    return w;
}
std::map<UINT, int> g_written;  // write messages seen, by message
std::set<HWND> g_watched;

LRESULT CALLBACK watch_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (writes().count(msg)) ++g_written[msg];
    return DefSubclassProc(h, msg, wp, lp);
}
void watch(HWND h)
{
    if (g_watched.insert(h).second) SetWindowSubclass(h, watch_proc, 0x88, 0);
}
// Every window of this thread watched (the window, its controls and the
// tooltip windows), as they are now.
void watch_all()
{
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND top, LPARAM) -> BOOL
        {
            watch(top);
            EnumChildWindows(
                top, [](HWND c, LPARAM) -> BOOL
                {
                    watch(c);
                    return TRUE;
                },
                0);
            return TRUE;
        },
        0);
}

std::string written()
{
    std::string out;
    for (const auto& [m, n] : g_written) out += " 0x" + std::to_string(m) + "x" + std::to_string(n);
    return out;
}

unsigned layouts()
{
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    return rap::win32::layout_requests_for_tests();
#else
    return 0;
#endif
}

int g_changed = 0;  // change notifications, any widget

// `set` to a value (the first call, which may change it), then again: the
// second call sends no write message, asks no relayout and notifies nothing.
void same_twice(const std::string& what, const std::function<void()>& set)
{
    set();
    pump();
    watch_all();  // (windows the first call made: a tooltip window)
    g_written.clear();
    const unsigned was = layouts();
    const int changed = g_changed;
    set();
    const unsigned now = layouts();
    expect(g_written.empty(), what + ": the same value again, no native call" + written());
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    expect(now == was, what + ": the same value again, no relayout (" + std::to_string(now - was) + ")");
#else
    (void)now;
    (void)was;
#endif
    expect(g_changed == changed, what + ": the same value again, no change notified");
    pump();
}

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

}  // namespace

int main(int argc, char** argv)
{
    test::no_dialogs();  // a failure to the console, never a dialog on the desktop
    rap::load_options options{.backend = "win32"};
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 2;
    using namespace rap::layout;
    using namespace rap::literals;
    rap::label* lbl = nullptr;
    rap::button* btn = nullptr;
    rap::check* chk = nullptr;
    rap::entry* ent = nullptr;
    rap::drop_down* dd = nullptr;
    rap::combo* cmb = nullptr;
    rap::spin* spn = nullptr;
    rap::slider* sld = nullptr;
    rap::progress_bar* prg = nullptr;
    rap::date_picker* dp = nullptr;
    rap::color_button* clr = nullptr;
    rap::menu_button* mb = nullptr;
    rap::tool_button* tb = nullptr;
    rap::tool_button* strip_tb = nullptr;
    rap::text_view* tv = nullptr;
    rap::list* lst = nullptr;
    rap::tree* tr = nullptr;
    rap::tabs* tbs = nullptr;
    rap::splitter* spl = nullptr;
    const std::vector<rap::column> cols{{"Name", 120_px}, {"Notes", 120_px}};
    const std::vector<rap::tree_item> items{{"Alpha", "a", {{"Child", "c"}}}, {"Beta", "b"}};
    auto win = (*ui)->create(
        window{"Setters no-op",
               {vbox{toolbar{tool_button{rap::icon::named("go-up"), "Up"}.toggle(false).bind(strip_tb)},
                     hbox{label{"Label"}.bind(lbl), button{"Button"}.bind(btn), check{"Check"}.bind(chk),
                          entry{"Entry"}.bind(ent), drop_down{{"One", "Two"}, 0}.bind(dd),
                          combo{{"One", "Two"}, "One"}.bind(cmb)},
                     hbox{spin(0, 10, 1).bind(spn), slider(0, 10, 1).bind(sld), progress_bar{}.bind(prg),
                          date_picker{{2026, 1, 1}}.bind(dp), color_button{rap::color{10, 20, 30}}.bind(clr),
                          menu_button{"Menu"}.bind(mb),
                          tool_button{rap::icon::named("go-down"), "Down"}.toggle(true).bind(tb)},
                     splitter(orientation::horizontal, list{cols, &g_model}.bind(lst).stretch(),
                              tree{items}.bind(tr).stretch(),
                              tabs{{"One", label{"one"}}, {"Two", label{"two"}}}.bind(tbs).stretch())
                         .bind(spl)
                         .stretch(),
                     text_view{"Text"}.bind(tv).stretch()}}}
            .size(1200_px, 700_px));
    pump();
    HWND top = FindWindowW(nullptr, L"Setters no-op");
    expect(top && !IsWindowVisible(top), "the window, never shown");
    expect(lbl && btn && chk && ent && dd && cmb && spn && sld && prg && dp && clr && mb && tb && strip_tb && tv &&
               lst && tr && tbs && spl,
           "each control bound");
    if (!top || !(lbl && btn && chk && ent && dd && cmb && spn && sld && prg && dp && clr && mb && tb && strip_tb &&
                  tv && lst && tr && tbs && spl))
        return 1;
    // Their change notifications counted: a setter's must fire none.
    auto bump = [] { ++g_changed; };
    ent->on_changed([&](const std::string&) { bump(); });
    dd->on_changed([&](int) { bump(); });
    cmb->on_changed([&](const std::string&) { bump(); });
    spn->on_changed([&](double) { bump(); });
    sld->on_changed([&](double) { bump(); });
    dp->on_changed([&](rap::date) { bump(); });
    clr->on_changed([&](rap::color) { bump(); });
    chk->on_toggled([&](bool) { bump(); });
    tb->on_toggled([&](bool) { bump(); });
    lst->on_selection_changed([&] { bump(); });
    tr->on_selection_changed([&] { bump(); });
    tbs->on_changed([&](int) { bump(); });
    tv->on_changed([&] { bump(); });
    watch_all();

    same_twice("label set_text", [&] { lbl->set_text("Another label"); });
    same_twice("button set_text", [&] { btn->set_text("Another button"); });
    same_twice("button set_icon", [&] { btn->set_icon(rap::icon::named("go-next")); });
    same_twice("check set_checked", [&] { chk->set_checked(true); });
    same_twice("check set_state", [&] { chk->set_state(rap::check_state::indeterminate); });
    same_twice("entry set_text", [&] { ent->set_text("Typed"); });
    same_twice("entry set_placeholder", [&] { ent->set_placeholder("Search"); });
    same_twice("entry set_read_only", [&] { ent->set_read_only(true); });
    same_twice("drop_down set_items", [&] { dd->set_items({"One", "Two", "Three"}); });
    same_twice("drop_down set_current", [&] { dd->set_current(1); });
    same_twice("combo set_items", [&] { cmb->set_items({"One", "Two", "Three"}); });
    same_twice("combo set_text", [&] { cmb->set_text("Two"); });
    same_twice("spin set_value", [&] { spn->set_value(4); });
    same_twice("spin set_range", [&] { spn->set_range(0, 20); });
    same_twice("slider set_value", [&] { sld->set_value(4); });
    same_twice("slider set_range", [&] { sld->set_range(0, 20); });
    same_twice("progress_bar set_fraction", [&] { prg->set_fraction(0.5f); });
    same_twice("date_picker set_date", [&] { dp->set_date({2026, 5, 6}); });
    same_twice("color_button set_color", [&] { clr->set_color({40, 50, 60}); });
    same_twice("menu_button set_text", [&] { mb->set_text("Another menu"); });
    same_twice("menu_button set_pushed", [&] { mb->set_pushed(true); });
    same_twice("tool_button set_checked", [&] { tb->set_checked(false); });
    same_twice("tool_button set_icon", [&] { tb->set_icon(rap::icon::named("go-up")); });
    same_twice("a toolbar's tool_button set_enabled", [&] { strip_tb->set_enabled(false); });
    same_twice("a toolbar's tool_button set_tooltip", [&] { strip_tb->set_tooltip("Upward"); });
    same_twice("text_view set_text", [&] { tv->set_text("Other text"); });
    same_twice("list select", [&] { lst->select({1, 2}); });
    same_twice("list set_current", [&] { lst->set_current(2); });
    same_twice("list set_sort_indicator", [&] { lst->set_sort_indicator(1, rap::sort_order::descending); });
    same_twice("list set_column_widths", [&] { lst->set_column_widths({150, 90}); });
    same_twice("tree select", [&] { tr->select("b"); });
    same_twice("tree set_expanded", [&] { tr->set_expanded("a", true); });
    same_twice("tabs set_current", [&] { tbs->set_current(1); });
    same_twice("splitter set_sizes", [&] { spl->set_sizes({300, 300, 300}); });
    same_twice("a widget set_enabled", [&] { btn->set_enabled(false); });
    same_twice("a widget set_enabled again", [&] { btn->set_enabled(true); });
    same_twice("a widget set_tooltip", [&] { btn->set_tooltip("A tip"); });
    same_twice("a widget set_tooltip none", [&] { btn->set_tooltip(""); });
    same_twice("a widget set_visible", [&] { chk->set_visible(false); });
    same_twice("window set_title", [&] { win->set_title("Setters no-op"); });

#ifdef RAPPORT_TEST_BUILTIN_WIN32
    // A changed value still relayouts where it did.
    {
        const unsigned was = layouts();
        lbl->set_text("A label changed");
        expect(layouts() != was, "a label's changed text relayouts");
        pump();
        const unsigned was2 = layouts();
        spl->set_sizes({200, 400, 300});
        expect(layouts() != was2, "a splitter's changed sizes relayout");
        pump();
        g_written.clear();
        btn->set_text("Changed again");
        expect(g_written.count(WM_SETTEXT) == 1, "a button's changed text set" + written());
    }
#endif
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
