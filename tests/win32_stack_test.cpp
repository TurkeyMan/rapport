// A stack's pages (layout::stack) on Windows, in a window that's never
// shown (the backend lays out hidden windows): each page in a host window of
// its own, shown only while it's the page shown; homogeneous, the window's
// size not changing as another page shows, and sized to the page shown when
// not; the focus moving to the page shown when it was in the one hidden (to
// the control that had it there, else the first); a page added later built
// with real controls and its binds; and a page removed from one of its own
// callbacks hidden and out of ids() at once, destroyed once the message
// loop is reached.
#include <rapport/rapport.h>

#include <windows.h>

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
    std::fflush(stdout);
    if (!ok) ++fails;
}

using ids = std::vector<std::string>;

class empty_model : public rap::list_model
{
public:
    size_t row_count() const override { return 0; }
    std::string text(size_t, size_t) const override { return {}; }
};

// Our own top-level window with this title (it's never shown).
HWND find_window(const wchar_t* title)
{
    struct search
    {
        const wchar_t* title;
        HWND found;
    } s{title, nullptr};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM lp) -> BOOL
        {
            auto* s = reinterpret_cast<search*>(lp);
            wchar_t text[128] = {};
            GetWindowTextW(h, text, 128);
            if (std::wstring(text) != s->title) return TRUE;
            s->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&s));
    return s.found;
}

// The hosts that are `root`'s children, in the order they were made.
std::vector<HWND> hosts(HWND root)
{
    std::vector<HWND> out;
    for (HWND c = GetWindow(root, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
    {
        wchar_t cls[64] = {};
        GetClassNameW(c, cls, 64);
        if (std::wstring(cls).starts_with(L"rapportHost")) out.push_back(c);
    }
    return out;
}

bool shown_style(HWND h)
{
    return (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE) != 0;
}

RECT client_rect(HWND h)
{
    RECT r{};
    GetClientRect(h, &r);
    return r;
}

HWND pending_focus(HWND root)
{
    return static_cast<HWND>(GetPropW(root, L"rapport-focus"));
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

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options opts;
    opts.backend = "win32";
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::printf("FAIL no win32 backend: %s\n", ui.error().c_str());
        return 1;
    }
    using namespace rap::layout;
    using namespace rap::literals;

    empty_model model;
    rap::stack* st = nullptr;
    rap::entry *a_entry = nullptr, *b_entry = nullptr;
    rap::list* big = nullptr;
    auto win = (*ui)->create(
        window{"rapport stack test",
               {stack{{"a", form{{"Name", entry{"a-entry"}.bind(a_entry)}, {"", button{"First"}}}
                                .align_group("g")},
                      {"b", vbox{entry{"b-entry"}.bind(b_entry),
                                 list{{{"Name"}}, &model}.bind(big).width(500_px).height(300_px)}}}
                    .bind(st)}});
    HWND root = find_window(L"rapport stack test");
    if (!root || !st || !a_entry || !b_entry || !big)
    {
        std::printf("FAIL the window or its binds\n");
        return 1;
    }
    expect(!IsWindowVisible(root), "the window is never shown");

    // -- a host per page, only the shown one shown
    std::vector<HWND> h = hosts(root);
    expect(h.size() == 2, "a host window per page");
    if (h.size() != 2) return 1;
    HWND host_a = h[0], host_b = h[1];
    HWND edit_a = FindWindowExW(host_a, nullptr, L"Edit", L"a-entry");
    HWND edit_b = FindWindowExW(host_b, nullptr, L"Edit", L"b-entry");
    HWND first = FindWindowExW(host_a, nullptr, L"Button", L"First");
    HWND list_b = FindWindowExW(host_b, nullptr, L"SysListView32", nullptr);
    expect(edit_a && edit_b && first && list_b, "each page's controls are in its host");
    expect(st->ids() == ids{"a", "b"} && st->shown() == "a", "the first page is shown");
    expect(shown_style(host_a) && !shown_style(host_b), "only the shown page's host is shown");

    // -- homogeneous: sized to its largest page from the start
    const rap::size_f before = win->size();
    const RECT a_rect = client_rect(host_a);
    st->show("b");
    expect(st->shown() == "b", "show: b");
    expect(!shown_style(host_a) && shown_style(host_b), "show: b's host shown, a's hidden");
    const rap::size_f after = win->size();
    expect(before.w == after.w && before.h == after.h,
           "homogeneous: the window's size doesn't change on show");
    const RECT b_rect = client_rect(host_b);
    expect(b_rect.right == a_rect.right && b_rect.bottom == a_rect.bottom,
           "homogeneous: each page given the same area");
    RECT list_rect{};
    GetWindowRect(list_b, &list_rect);
    expect(list_rect.right - list_rect.left <= b_rect.right &&
               list_rect.bottom - list_rect.top <= b_rect.bottom &&
               list_rect.right - list_rect.left >= 500,
           "homogeneous: the largest page fits the area it had before it was shown");

    // Not homogeneous: sized to the page shown (a small one first).
    rap::stack* sized = nullptr;
    auto win2 = (*ui)->create(
        window{"rapport stack test 2",
               {stack{{"small", label{"Small"}},
                      {"large", list{{{"Name"}}, &model}.width(500_px).height(300_px)}}
                    .homogeneous(false)
                    .bind(sized)}});
    expect(sized && win2->size().w < before.w && win2->size().h < before.h,
           "homogeneous(false): sized to the (smaller) page shown, not the largest");

    // -- the focus
    st->show("a");
    a_entry->focus();  // the window hidden: the control it'll focus when shown
    expect(pending_focus(root) == edit_a, "the focus asked for in page a");
    st->show("b");
    expect(pending_focus(root) == edit_b,
           "show: the focus moves to the page shown's first control");
    st->show("a");
    expect(pending_focus(root) == edit_a,
           "show: back to the control that had it in that page, not its first");
    RemovePropW(root, L"rapport-focus");
    st->show("b");
    expect(pending_focus(root) == nullptr, "show: the focus elsewhere is left there");

    // -- a page added later: real controls, binds
    rap::button* late = nullptr;
    bool clicked = false;
    HWND late_hwnd = nullptr, host_c = nullptr;
    st->add("c", form{{"A longer label",
                       button{"Late"}.bind(late).on_click(
                           [&]
                           {
                               clicked = true;
                               st->remove("c");
                               expect(st->ids() == ids{"a", "b"},
                                      "removed in its own callback: out of ids() at once");
                               expect(st->shown() == "b",
                                      "the shown page, the last, removed: the previous shows");
                               expect(!shown_style(host_c) && shown_style(host_b),
                                      "removed in its own callback: its host hidden at once");
                               expect(IsWindow(late_hwnd) != FALSE,
                                      "removed in its own callback: its controls still there");
                           })}}
                     .align_group("g"));
    expect(late != nullptr, "add: the page's binds are set");
    expect(st->ids() == ids{"a", "b", "c"} && st->shown() == "b", "add: last, and hidden");
    h = hosts(root);
    expect(h.size() == 3, "add: a host of its own");
    if (h.size() != 3) return 1;
    host_c = h[2];
    late_hwnd = FindWindowExW(host_c, nullptr, L"Button", L"Late");
    expect(late_hwnd != nullptr, "add: a real button, in its host");
    expect(!shown_style(host_c), "add: its host hidden");

    // -- removed from its own callback
    st->show("c");
    expect(shown_style(host_c) && IsWindowVisible(host_c) == FALSE,
           "show: c's host shown (in a hidden window)");
    late->focus();
    expect(pending_focus(root) == late_hwnd, "the focus asked for in page c");
    SendMessageW(host_c, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(late_hwnd), BN_CLICKED),
                 reinterpret_cast<LPARAM>(late_hwnd));
    expect(clicked, "the button's callback ran");
    expect(IsWindow(late_hwnd) && IsWindow(host_c),
           "after the callback returned: not destroyed before the loop runs");
    expect(pending_focus(root) == edit_b,
           "the focus, in the page removed, moves to the page shown's");
    pump();
    expect(!IsWindow(late_hwnd) && !IsWindow(host_c),
           "once the loop has run: the page's windows are destroyed");
    expect(hosts(root).size() == 2, "the window's other pages are kept");

    // A page not shown removed; the window laid out again after (its form's
    // alignment group no longer holds the page's form).
    st->remove("a");
    expect(st->ids() == ids{"b"} && st->shown() == "b", "a page not shown removed");
    pump();
    expect(!IsWindow(host_a) && !IsWindow(edit_a), "and destroyed");
    st->add("a", form{{"Name", entry{"a-again"}}}.align_group("g"));
    st->show("a");
    expect(hosts(root).size() == 2 && st->shown() == "a", "an id removed, added again");

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
