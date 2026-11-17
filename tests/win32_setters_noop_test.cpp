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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
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
void note(const std::string& what)
{
    std::printf("note %s\n", what.c_str());
    std::fflush(stdout);
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

// The first of `top`'s descendants of class `cls` (and with text `text`, if
// given).
HWND find(HWND top, const wchar_t* cls, const wchar_t* text = nullptr)
{
    struct q
    {
        const wchar_t* cls;
        const wchar_t* text;
        HWND found = nullptr;
    } f{cls, text};
    EnumChildWindows(
        top,
        [](HWND h, LPARAM lp) -> BOOL
        {
            auto* f = reinterpret_cast<q*>(lp);
            wchar_t c[64]{};
            GetClassNameW(h, c, 64);
            if (_wcsicmp(c, f->cls)) return TRUE;
            if (f->text)
            {
                wchar_t t[64]{};
                GetWindowTextW(h, t, 64);
                if (wcscmp(t, f->text)) return TRUE;
            }
            f->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}
std::wstring text_of(HWND h)
{
    const int n = GetWindowTextLengthW(h);
    std::wstring w(size_t(n) + 1, L'\0');
    GetWindowTextW(h, w.data(), n + 1);
    w.resize(size_t(n));
    return w;
}
int width_of(HWND h)
{
    RECT r{};
    GetWindowRect(h, &r);
    return int(r.right - r.left);
}

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
    size_t rows = 4;  // changed underneath for the reload check, unannounced
    size_t row_count() const override { return rows; }
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
    rap::slider* sld_shown = nullptr;
    rap::widget* box_w = nullptr;
    rap::button* inner = nullptr;
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
                          tool_button{rap::icon::named("go-down"), "Down"}.toggle(true).bind(tb),
                          slider(0, 10, 0).show_value(0).bind(sld_shown),
                          hbox{button{"Inner"}.bind(inner)}.handle(box_w)},
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
    // The same model again isn't a no-op: it's a reload (#88's exception).
    // Its row count changed underneath, unannounced: set again, the list has
    // the new count.
    {
        HWND lv = FindWindowExW(top, nullptr, WC_LISTVIEWW, nullptr);
        expect(lv != nullptr, "the list's list view");
        if (lv)
        {
            const int before = ListView_GetItemCount(lv);
            g_model.rows = 6;
            lst->set_model(&g_model);
            pump();
            const int after = ListView_GetItemCount(lv);
            expect(before == 4 && after == 6, "list set_model with the same model: a reload, its rows read anew, " +
                                                  std::to_string(before) + " then " + std::to_string(after));
            g_model.rows = 4;
            lst->set_model(&g_model);
            pump();
        }
    }
    same_twice("tree select", [&] { tr->select("b"); });
    same_twice("tree set_expanded", [&] { tr->set_expanded("a", true); });
    same_twice("tabs set_current", [&] { tbs->set_current(1); });
    spl->set_sizes({300, 300, 300});
    pump();
    // The sizes its panes have, laid out (a stretch pane takes the rest):
    // those again, nothing done.
    {
        const std::vector<float> placed = spl->sizes();
        same_twice("splitter set_sizes", [&] { spl->set_sizes(placed); });
    }
    same_twice("a widget set_enabled", [&] { btn->set_enabled(false); });
    same_twice("a widget set_enabled again", [&] { btn->set_enabled(true); });
    same_twice("a widget set_tooltip", [&] { btn->set_tooltip("A tip"); });
    same_twice("a widget set_tooltip none", [&] { btn->set_tooltip(""); });
    same_twice("a widget set_visible", [&] { chk->set_visible(false); });
    same_twice("window set_title", [&] { win->set_title("Setters no-op"); });
    same_twice("text_view set_read_only", [&] { tv->set_read_only(true); });
    tv->set_read_only(false);
    pump();

    // -- "unchanged" is what's on screen: user edits, modes, re-applies --

    // A spin's edit with a user's typing not yet committed (7): the app's 4
    // again shows 4, and leaving the field keeps 4.
    {
        HWND ud = find(top, UPDOWN_CLASSW);
        HWND edit = ud ? reinterpret_cast<HWND>(SendMessageW(ud, UDM_GETBUDDY, 0, 0)) : nullptr;
        expect(edit != nullptr, "the spin's edit");
        if (edit)
        {
            spn->set_value(4);
            pump();
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"7"));
            SendMessageW(edit, EM_SETMODIFY, TRUE, 0);
            spn->set_value(4);
            expect(text_of(edit) == L"4", "a spin's 4 again over a user's uncommitted 7: it shows 4");
            SendMessageW(edit, WM_KILLFOCUS, 0, 0);
            pump();
            expect(spn->value() == 4 && text_of(edit) == L"4", "leaving the field keeps 4: " +
                                                                    std::to_string(spn->value()));
            // A new range: the value applied against it (15 in 0..20, then
            // 0..10: 10, shown).
            spn->set_range(0, 20);
            spn->set_value(15);
            spn->set_range(0, 10);
            expect(spn->value() == 10 && text_of(edit) == L"10",
                   "a spin's new range applies its value: 10, " + std::to_string(spn->value()));
            spn->set_range(0, 20);
            spn->set_value(4);
            pump();
        }
    }
    // A slider's new range: its thumb and value() agree (5 in 0..10, then
    // 0..4: 4, the thumb at the track's end).
    {
        HWND tbar = find(top, TRACKBAR_CLASSW);
        if (tbar)
        {
            sld->set_range(0, 10);
            sld->set_value(5);
            sld->set_range(0, 4);
            const LRESULT pos = SendMessageW(tbar, TBM_GETPOS, 0, 0), end = SendMessageW(tbar, TBM_GETRANGEMAX, 0, 0);
            expect(sld->value() == 4 && pos == end, "a slider's new range applies its value: 4 at the track's end, " +
                                                        std::to_string(sld->value()) + " at " + std::to_string(pos) +
                                                        " of " + std::to_string(end));
            sld->set_range(0, 10);
            pump();
        }
        else
            expect(false, "the slider's trackbar");
        // A slider showing its value, at its minimum from the start: shown.
        expect(sld_shown && find(top, L"Static", L"0") != nullptr,
               "a slider showing its value, at its minimum, shows it (0)");
    }
    // A progress bar pulsing, then the fraction it had: no longer pulsing,
    // at it.
    {
        HWND bar = find(top, PROGRESS_CLASSW);
        if (bar)
        {
            prg->set_fraction(0.5f);
            prg->pulse();
            prg->set_fraction(0.5f);
            const bool marquee = (GetWindowLongPtrW(bar, GWL_STYLE) & PBS_MARQUEE) != 0;
            const LRESULT pos = SendMessageW(bar, PBM_GETPOS, 0, 0);
            expect(!marquee && pos == 500, "a progress bar after pulse() at its fraction again: not pulsing, at 500, " +
                                               std::string(marquee ? "pulsing" : "not pulsing") + " at " +
                                               std::to_string(pos));
            pump();
        }
        else
            expect(false, "the progress bar");
    }
    // A text view the user edited ("Other"): the app's "Text" again sets it.
    {
        HWND rich = find(top, L"RICHEDIT50W");
        if (rich)
        {
            tv->set_text("Text");
            pump();
            SendMessageW(rich, EM_SETSEL, 0, -1);
            SendMessageW(rich, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"Other"));
            tv->set_text("Text");
            expect(tv->text() == "Text", "a text view the user edited, given its text again: \"Text\", \"" +
                                             tv->text() + "\"");
            pump();
        }
        else
            expect(false, "the text view's RichEdit");
    }
    // A read-only text view with text appended, through a new theme (which
    // sets its text again and appends what was appended): the text once.
    {
        tv->set_read_only(true);
        tv->set_text("base");
        tv->append(" tail", rap::text_style{});
        pump();
        // A new theme as the app takes one: the other colour scheme (a
        // WM_THEMECHANGED alone changes nothing, so rethemes nothing).
        (*ui)->set_color_scheme(rap::color_scheme::dark);
        pump();
        (*ui)->set_color_scheme(rap::color_scheme::light);
        pump();
        expect(tv->text() == "base tail", "a text view through a new theme, appended text once: \"base tail\", \"" +
                                              tv->text() + "\"");
        tv->set_read_only(false);
        tv->set_text("Text");
        pump();
    }
    // The text the user typed, then set by the app as it is ("new": nothing
    // written), appended to, made read-only, through a new theme: the app's
    // "new" is what the rebuild restores, not the "old" set before.
    {
        HWND rich = find(top, L"RICHEDIT50W");
        if (rich)
        {
            tv->set_text("old");
            pump();
            SendMessageW(rich, EM_SETSEL, 0, -1);
            SendMessageW(rich, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"new"));
            tv->set_text("new");
            tv->append(" tail", rap::text_style{});
            tv->set_read_only(true);
            pump();
            (*ui)->set_color_scheme(rap::color_scheme::dark);
            pump();
            (*ui)->set_color_scheme(rap::color_scheme::light);
            pump();
            expect(tv->text() == "new tail", "a text view the user edited to what the app then set, through a new "
                                             "theme: \"new tail\", \"" + tv->text() + "\"");
            tv->set_read_only(false);
            tv->set_text("Text");
            pump();
        }
        else
            expect(false, "the text view's RichEdit");
    }
    // A tool button's checked state again: its early return covers it (it
    // only invalidates, and a never-shown window keeps no update region, so a
    // repaint can't be seen here: GetUpdateRect is empty even for a change).
    // A splitter whose window grew (a stretch pane taking the width): the
    // sizes asked for again are laid out, and the panes and sizes() agree.
    {
        HWND lv = find(top, WC_LISTVIEWW), tv2 = find(top, WC_TREEVIEWW);
        RECT wr{};
        GetWindowRect(top, &wr);
        spl->set_sizes({300, 300, 300});
        pump();
        SetWindowPos(top, nullptr, 0, 0, wr.right - wr.left + 200, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER |
                                                                                      SWP_NOACTIVATE);
        pump();
        const unsigned was = layouts();
        spl->set_sizes({300, 300, 300});
#ifdef RAPPORT_TEST_BUILTIN_WIN32
        expect(layouts() != was, "a splitter given its sizes again after its window grew: laid out");
#else
        (void)was;
#endif
        pump();
        const std::vector<float> now = spl->sizes();
        const float scale = float(GetDpiForWindow(top)) / 96.f;
        expect(now.size() == 3 && lv && tv2 && std::abs(now[0] * scale - width_of(lv)) <= 2 &&
                   std::abs(now[1] * scale - width_of(tv2)) <= 2,
               "its panes and sizes() agree: " + std::to_string(now.size() == 3 ? now[0] : -1) + " / " +
                   std::to_string(lv ? width_of(lv) : -1) + ", " + std::to_string(now.size() == 3 ? now[1] : -1) +
                   " / " + std::to_string(tv2 ? width_of(tv2) : -1));
        SetWindowPos(top, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER |
                                                                                SWP_NOACTIVATE);
        pump();
    }
    // A new splitter (no sizes asked for), given the sizes it has: nothing
    // laid out, but the sizes are kept as asked for, so a wider window keeps
    // the second pane's (without them, the two stretch panes share it).
    {
        rap::splitter* fresh = nullptr;
        auto win2 = (*ui)->create(window{"Setters no-op splitter",
                                         {splitter(orientation::horizontal, label{"one"}.stretch(),
                                                   label{"two"}.stretch())
                                              .bind(fresh)
                                              .stretch()}}
                                      .size(800_px, 300_px));
        pump();
        HWND top2 = FindWindowW(nullptr, L"Setters no-op splitter");
        expect(top2 && !IsWindowVisible(top2) && fresh, "the splitter window, never shown");
        if (top2 && fresh)
        {
            const std::vector<float> before = fresh->sizes();
            fresh->set_sizes(before);
            pump();
            RECT r2{};
            GetWindowRect(top2, &r2);
            SetWindowPos(top2, nullptr, 0, 0, r2.right - r2.left + 200, r2.bottom - r2.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            pump();
            const std::vector<float> after = fresh->sizes();
            expect(before.size() == 2 && after.size() == 2 && std::abs(after[1] - before[1]) <= 2 &&
                       after[0] > before[0] + 100,
                   "a new splitter given its own sizes, then widened by 200: the second pane keeps its size, " +
                       (before.size() == 2 ? std::to_string(before[0]) + "/" + std::to_string(before[1]) : "?") +
                       " then " +
                       (after.size() == 2 ? std::to_string(after[0]) + "/" + std::to_string(after[1]) : "?"));
        }
    }
    // A list's header changed by someone else (a sort arrow, a column's
    // width, the columns' order, as a user's drag would): the app's again
    // applies.
    {
        HWND lv = find(top, WC_LISTVIEWW);
        HWND header = lv ? ListView_GetHeader(lv) : nullptr;
        if (lv && header)
        {
            lst->set_sort_indicator(1, rap::sort_order::descending);
            HDITEMW hd{};
            hd.mask = HDI_FORMAT;
            Header_GetItem(header, 1, &hd);
            hd.fmt = (hd.fmt & ~(HDF_SORTUP | HDF_SORTDOWN)) | HDF_SORTUP;
            Header_SetItem(header, 1, &hd);
            lst->set_sort_indicator(1, rap::sort_order::descending);
            Header_GetItem(header, 1, &hd);
            expect((hd.fmt & HDF_SORTDOWN) && !(hd.fmt & HDF_SORTUP), "a list's sort arrow changed under it, set "
                                                                       "again: descending");
            lst->set_column_widths({150, 90});
            ListView_SetColumnWidth(lv, 0, 60);  // as a user's resize
            lst->set_column_widths({150, 90});
            const float scale = float(GetDpiForWindow(top)) / 96.f;
            expect(std::abs(ListView_GetColumnWidth(lv, 0) - std::lround(150 * scale)) <= 1,
                   "a list's column resized under it, its width set again: " +
                       std::to_string(ListView_GetColumnWidth(lv, 0)));
            lst->set_column_order({0, 1});
            int swapped[2] = {1, 0};
            ListView_SetColumnOrderArray(lv, 2, swapped);  // as a user's drag
            lst->set_column_order({0, 1});
            int order[2] = {};
            ListView_GetColumnOrderArray(lv, 2, order);
            expect(order[0] == 0 && order[1] == 1, "a list's columns reordered under it, set again: 0, 1");
            // set_current, the selection cleared, set_current again: as
            // before (the row focused).
            lst->set_current(1);
            lst->select({});
            lst->set_current(1);
            expect(lst->current() == std::optional<size_t>(1), "a list's current row after its selection cleared: 1");
            pump();
        }
        else
            expect(false, "the list's header");
    }
    // Enabled is a control's own, the effective its and its containers'
    // (F17): the box off, its button off, the box on: the button stays off;
    // the box off, the button on (already), the box on: the button on.
    {
        HWND ih = find(top, L"Button", L"Inner");
        if (box_w && inner && ih)
        {
            box_w->set_enabled(false);
            inner->set_enabled(false);
            box_w->set_enabled(true);
            expect(!IsWindowEnabled(ih), "a button turned off, its box off and on again: still off");
            inner->set_enabled(true);
            expect(IsWindowEnabled(ih) != FALSE, "turned on again: on");
            box_w->set_enabled(false);
            g_written.clear();
            inner->set_enabled(true);
            expect(g_written.empty() && !IsWindowEnabled(ih),
                   "its box off, the button on again (as it is): nothing done, off with its box" + written());
            box_w->set_enabled(true);
            expect(IsWindowEnabled(ih) != FALSE, "the box on again: the button on");
            pump();
        }
        else
            expect(false, "the box and its button");
    }

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
