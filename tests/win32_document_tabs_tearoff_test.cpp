// A window's document tabs torn off into windows of their own and dropped
// into other windows, on Windows: stage 1's tab control (under a menu bar)
// and Windows 11's strip in the caption where there is one. The windows are
// never shown: the strips are sent the mouse's messages and keys, nothing
// else. Two test hooks (the "rapport-document-tabs-drag" message) stand in
// for what needs shown windows: the drop lookup's z-order (WindowFromPoint
// and EnumWindows see only shown windows) is a list of the test's windows,
// and a window a tab drag moves isn't put into the system's move loop (which
// needs the real mouse) but moved here with SetWindowPos, as the loop would,
// and its end sent (WM_EXITSIZEMOVE, its wParam 1 for an Escape: the thread's
// key state is no witness here, where real input may come in meanwhile).
//
// Checked, in each strip: on_detach once with the tab's id and the pointer's
// place, a null return refusing it (the drag going on along the row, not
// asked again); a tab torn off, its window moved by the drag (the window's
// own WM_WINDOWPOSCHANGED finding the strip under the pointer, which marks
// where it would land) and let go over another window's strip: on_receive
// there once with the id, the index and the torn-off window as the source;
// a false return changing nothing; a strip of another group neither marked
// nor asked; a tab dragged straight onto another window's strip (no tearing
// off) dropped there; Escape mid-drag putting it back and asking nothing,
// and Escape at a torn-off window's move sending the tab home (group or none) to where it
// was; the only tab of a window moving its window, on_detach not asked; the
// right-click menu with the app's items after the fixed ones, its Move to
// new window asking on_detach with no point; and in the caption, Shift+F10
// and the Menu key opening it.
//
// The real thing (the move loop, the z-order, a menu shown) is CI's:
// rapport-win32-document-tabs-tearoff-test --input, with real input, runs
// only on a CI runner (GITHUB_ACTIONS=true and RAPPORT_TEST_SEND_INPUT=1).
//   rapport-win32-document-tabs-tearoff-test
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <commctrl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
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

std::string join(const std::vector<std::string>& v)
{
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : ",") + s;
    return out;
}

void pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

DWORD windows_build()
{
    using fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto get =
        ntdll ? reinterpret_cast<fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    return get && get(&v) == 0 ? v.dwBuildNumber : 0;
}

// The modifiers this test sets for a message it sends, and none other: a
// key state left by anything before (Alt, the Windows key, a button) can't
// leak into it.
void set_keys(std::initializer_list<int> down)
{
    BYTE state[256] = {};
    GetKeyboardState(state);
    for (int vk : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU,
                   VK_LWIN, VK_RWIN, VK_ESCAPE, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON})
        state[vk] = 0;
    for (int vk : down) state[vk] = 0x80;
    SetKeyboardState(state);
}

UINT drag_message()
{
    static const UINT m = RegisterWindowMessageW(L"rapport-document-tabs-drag");
    return m;
}
LRESULT ask(HWND top, int what, LPARAM lp = 0)
{
    return SendMessageW(top, drag_message(), MAKEWPARAM(what, 0), lp);
}

// As the backend's (tab_menu_capture).
struct tab_menu_capture
{
    int choose;
    int count;
    wchar_t labels[16][64];
    int enabled[16];
    int opened;
};

HWND strip_of(HWND top)
{
    return top ? FindWindowExW(top, nullptr, L"rapportCaptionTabs", nullptr) : nullptr;
}

HWND tab_control_of(HWND top)
{
    HWND found = nullptr;
    EnumChildWindows(
        top,
        [](HWND h, LPARAM p) -> BOOL
        {
            wchar_t name[32]{};
            GetClassNameW(h, name, 32);
            if (_wcsicmp(name, WC_TABCONTROLW) || !(GetWindowLongPtrW(h, GWL_STYLE) & TCS_FIXEDWIDTH))
                return TRUE;
            *reinterpret_cast<HWND*>(p) = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// One window: its tabs, what its handlers were told, and how they answer.
struct told_receive
{
    std::string id;
    int index = -1;
    rap::window* source = nullptr;
};
struct fixture;
std::vector<std::unique_ptr<fixture>> fixtures;
rap::ui* g_ui = nullptr;
bool g_menu_bar = false;  // stage 1 (under a menu bar), or the caption's strip

struct fixture
{
    std::string name;
    std::unique_ptr<rap::window> win;
    rap::document_tabs* tabs = nullptr;
    HWND top = nullptr, strip = nullptr;
    bool caption = false;
    // What it was told.
    std::vector<std::pair<std::string, std::optional<rap::point_f>>> detached;
    std::vector<told_receive> received;
    // How it answers.
    bool refuse_detach = false, accept = true;
    fixture* made = nullptr;  // the window on_detach made last

    RECT tab(int i) const
    {
        RECT r{};
        if (caption)
        {
            static const UINT m = RegisterWindowMessageW(L"rapport-caption-tabs");
            SendMessageW(strip, m, MAKEWPARAM(3, i), reinterpret_cast<LPARAM>(&r));
        }
        else
            TabCtrl_GetItemRect(strip, i, &r);
        return r;
    }
    POINT centre(int i) const
    {
        const RECT r = tab(i);
        return {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    }
    // A tab's point on the screen.
    POINT screen(POINT client) const
    {
        ClientToScreen(strip, &client);
        return client;
    }
    // A screen point as this strip's client one.
    POINT client(POINT s) const
    {
        ScreenToClient(strip, &s);
        return s;
    }
    void down(POINT p) { SendMessageW(strip, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(short(p.x), short(p.y))); }
    void move(POINT p) { SendMessageW(strip, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(short(p.x), short(p.y))); }
    void up(POINT p) { SendMessageW(strip, WM_LBUTTONUP, 0, MAKELPARAM(short(p.x), short(p.y))); }
    void escape() { SendMessageW(strip, WM_KEYDOWN, VK_ESCAPE, 1); }
    int drop_mark() const { return int(ask(top, 2)); }
    int threshold() const { return GetSystemMetricsForDpi(SM_CXDRAG, GetDpiForWindow(top)); }
};

fixture* make(const std::string& name, std::vector<std::string> ids, const std::string& group);

// The handlers, as an app's: on_detach makes a window with the tab and takes
// it out of this one; on_receive takes it from the source into this one.
rap::layout::document_tabs tabs_spec(fixture& f, const std::vector<std::string>& ids, const std::string& group)
{
    std::vector<rap::document_tab> items;
    for (const std::string& id : ids) items.push_back({id, "Tab " + id, {}, {}});
    fixture* self = &f;
    return rap::layout::document_tabs{items}
        .current(ids.empty() ? std::string() : ids[0])
        .group(group)
        .on_detach(
            [self, group](const std::string& id, std::optional<rap::point_f> at) -> rap::window*
            {
                self->detached.push_back({id, at});
                if (self->refuse_detach) return nullptr;
                fixture* made = make(self->name + "+" + id, {id}, group);
                self->tabs->remove(id);
                self->made = made;
                return made->win.get();
            })
        .on_receive(
            [self](const rap::document_tabs::transfer& t)
            {
                self->received.push_back({t.id, t.index, t.source});
                if (!self->accept) return false;
                for (auto& o : fixtures)
                    if (o->win.get() == t.source)
                    {
                        o->tabs->remove(t.id);
                        self->tabs->insert(t.index, {t.id, t.title, t.icon, t.tooltip});
                        return true;
                    }
                return false;
            })
        .on_context_menu(
            [](const std::string& id)
            {
                return rap::menu{{"_Rename " + id, [] {}}, {"_Duplicate", [] {}}};
            })
        .bind(f.tabs);
}

fixture* make(const std::string& name, std::vector<std::string> ids, const std::string& group)
{
    fixtures.push_back(std::make_unique<fixture>());
    fixture& f = *fixtures.back();
    f.name = name;
    using namespace rap::layout;
    window w{name, {label{"Content"}}};
    w.size(700_px, 300_px);
    if (g_menu_bar) w.menu_bar({{"_File", [] { return rap::menu{}; }}});
    w.tabs(tabs_spec(f, ids, group));
    f.win = g_ui->create(std::move(w));  // never shown
    pump();
    f.top = FindWindowW(nullptr, std::wstring(name.begin(), name.end()).c_str());
    HWND caption = strip_of(f.top);
    f.caption = caption != nullptr;
    f.strip = caption ? caption : tab_control_of(f.top);
    return &f;
}

// Windows apart on the screen (never shown, so nowhere anyone sees).
void place(fixture& f, int x, int y)
{
    SetWindowPos(f.top, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// The drop lookup's windows, in z-order.
void stack(std::vector<HWND> v)
{
    v.push_back(nullptr);
    ask(v[0] ? v[0] : fixtures[0]->top, 1, reinterpret_cast<LPARAM>(v.data()));
}

// A window moved by a tab drag, as the move loop would: the pointer, at
// `from` on the screen as it took the window, now at `to`.
void move_window(HWND w, POINT from, POINT to, POINT window_at)
{
    SetWindowPos(w, nullptr, window_at.x + (to.x - from.x), window_at.y + (to.y - from.y), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

std::string pt(std::optional<rap::point_f> p)
{
    return p ? std::to_string(int(p->x)) + "," + std::to_string(int(p->y)) : "none";
}

void run(const char* stage)
{
    const std::string at = std::string(" (") + stage + ")";
    const std::string tag = g_menu_bar ? "1" : "2";
    fixture& a = *make("A" + tag, {"a", "b", "c"}, "g");
    fixture& b = *make("B" + tag, {"x", "y"}, "g");
    fixture& c = *make("C" + tag, {"p", "q"}, "other");
    fixture& d = *make("D" + tag, {"solo"}, "g");
    expect(a.top && b.top && c.top && d.top && a.strip && b.strip && !IsWindowVisible(a.top),
           "the windows and their strips, never shown" + at);
    if (!a.strip || !b.strip || !c.strip || !d.strip) return;
    expect(a.caption == !g_menu_bar, "the strip expected" + at);
    place(a, 100, 100);
    place(b, 100, 700);
    place(c, 900, 700);
    place(d, 900, 100);
    stack({a.top, b.top, c.top, d.top});
    ask(a.top, 0, 1);  // no move loop
    using ids = std::vector<std::string>;
    const int th = a.threshold();
    RECT strip_rect;
    GetClientRect(a.strip, &strip_rect);
    const POINT below{0, strip_rect.bottom + 200};  // past the tear-off distance, under A's strip

    // Refused: on_detach once, nothing changes, the drag goes on.
    a.refuse_detach = true;
    POINT p = a.centre(1);
    a.down(p);
    a.move(POINT{p.x + th + 1, p.y});
    a.move(POINT{p.x, below.y});
    const POINT torn_at = a.screen(POINT{p.x, below.y});
    expect(a.detached.size() == 1 && a.detached[0].first == "b",
           "dragged out: on_detach(b) once" + at + " (" + std::to_string(a.detached.size()) + ")");
    if (!a.detached.empty())
    {
        const float s = float(GetDpiForWindow(a.top)) / 96.f;
        const auto& q = a.detached[0].second;
        expect(q && std::abs(q->x - torn_at.x / s) < 1 && std::abs(q->y - torn_at.y / s) < 1,
               "with the pointer's place, in the window's logical px" + at + ": " + pt(q));
    }
    a.move(POINT{p.x + 5, below.y + 50});
    expect(a.detached.size() == 1, "refused: not asked again this drag" + at);
    expect(a.tabs->ids() == ids{"a", "b", "c"} && !ask(a.top, 3), "refused: nothing changed, nothing moving" + at);
    a.move(a.centre(2));
    expect(a.tabs->ids() == ids{"a", "c", "b"}, "and the drag goes on along the row" + at + ": " + join(a.tabs->ids()));
    a.escape();
    expect(a.tabs->ids() == ids{"a", "b", "c"}, "Escape: back where it was" + at + ": " + join(a.tabs->ids()));
    a.up(a.centre(2));
    a.refuse_detach = false;
    a.detached.clear();

    // Escape before tearing off: back, and no detach.
    p = a.centre(0);
    a.down(p);
    a.move(POINT{p.x + th + 1, p.y});
    a.move(a.centre(2));
    a.escape();
    a.move(POINT{p.x, below.y});
    a.up(POINT{p.x, below.y});
    expect(a.detached.empty() && a.tabs->ids() == ids{"a", "b", "c"},
           "Escape mid-drag: back at its index, and nothing torn off after" + at);

    // Torn off, then dropped on B's strip.
    p = a.centre(1);
    a.down(p);
    a.move(POINT{p.x + th + 1, p.y});
    a.move(POINT{p.x, below.y});
    expect(a.detached.size() == 1 && a.made && a.tabs->ids() == ids{"a", "c"},
           "torn off: on_detach(b), the app's window made, b out of A" + at + ": " + join(a.tabs->ids()));
    if (!a.made) return;
    fixture& t = *a.made;
    expect(t.tabs && t.tabs->ids() == ids{"b"} && !IsWindowVisible(t.top), "the new window holds b, never shown" + at);
    expect(HWND(ask(a.top, 3)) == t.top, "and it's the window the drag moves" + at);
    RECT tr;
    GetWindowRect(t.top, &tr);
    const POINT t_at{tr.left, tr.top};
    POINT tab_screen = t.screen(t.centre(0));
    expect(std::abs(tab_screen.x - torn_at.x) <= t.tab(0).right - t.tab(0).left &&
               std::abs(tab_screen.y - torn_at.y) <= t.tab(0).bottom - t.tab(0).top,
           "put with its tab under the pointer" + at);
    // Over B's first tab's right half: it'd land second.
    const RECT bx = b.tab(0);
    const POINT over_b = b.screen(POINT{bx.right - 4, (bx.top + bx.bottom) / 2});
    move_window(t.top, torn_at, over_b, t_at);
    expect(b.drop_mark() == 1, "moved over B's strip: B marks index 1" + at + " (" + std::to_string(b.drop_mark()) + ")");
    expect(c.drop_mark() == -1 && a.drop_mark() == -1, "and no other strip" + at);
    // On to C (another group): no mark there, none on B.
    const POINT over_c = c.screen(c.centre(0));
    GetWindowRect(t.top, &tr);
    move_window(t.top, over_b, over_c, POINT{tr.left, tr.top});
    expect(c.drop_mark() == -1 && b.drop_mark() == -1, "over C's strip, another group: nothing marked" + at);
    // Back over B, let go.
    GetWindowRect(t.top, &tr);
    move_window(t.top, over_c, over_b, POINT{tr.left, tr.top});
    expect(b.drop_mark() == 1, "back over B: marked again" + at + " (" + std::to_string(b.drop_mark()) + ")");
    set_keys({});
    SendMessageW(t.top, WM_EXITSIZEMOVE, 0, 0);
    expect(b.received.size() == 1 && b.received[0].id == "b" && b.received[0].index == 1 &&
               b.received[0].source == t.win.get(),
           "let go over B: on_receive(b, 1, the torn-off window) once" + at + " (" +
               std::to_string(b.received.size()) + (b.received.empty() ? "" : ", " + b.received[0].id + " " + std::to_string(b.received[0].index)) + ")");
    expect(b.tabs->ids() == ids{"x", "b", "y"} && t.tabs->ids().empty() && b.drop_mark() == -1 && !ask(a.top, 3),
           "the app moved it: B has it second, the torn-off window's empty, nothing marked or moving" + at + ": " +
               join(b.tabs->ids()));
    expect(c.received.empty(), "and C was asked nothing" + at);

    // Torn off again, refused by B: nothing changes.
    b.received.clear();
    b.accept = false;
    a.made = nullptr;
    p = a.centre(1);  // c
    a.down(p);
    a.move(POINT{p.x + th + 1, p.y});
    a.move(POINT{p.x, below.y});
    if (!a.made) return expect(false, "torn off again" + at);
    fixture& t2 = *a.made;
    GetWindowRect(t2.top, &tr);
    const RECT b0 = b.tab(0);
    const POINT b_first = b.screen(POINT{b0.left + 4, (b0.top + b0.bottom) / 2});
    move_window(t2.top, a.screen(POINT{p.x, below.y}), b_first, POINT{tr.left, tr.top});
    expect(b.drop_mark() == 0, "over B's first tab's left half: index 0" + at + " (" + std::to_string(b.drop_mark()) + ")");
    SendMessageW(t2.top, WM_EXITSIZEMOVE, 0, 0);
    expect(b.received.size() == 1 && b.received[0].id == "c" && b.tabs->ids() == ids{"x", "b", "y"} &&
               t2.tabs->ids() == ids{"c"},
           "B's on_receive says no: both rows as they were" + at);
    b.accept = true;

    // Escape at a torn-off window's move: the tab offered back where it was.
    p = t2.centre(0);
    a.received.clear();
    // t2 has the one tab: dragging it moves t2 itself.
    t2.down(p);
    t2.move(POINT{p.x + th + 1, p.y});
    expect(HWND(ask(a.top, 3)) == t2.top && t2.detached.empty(),
           "the only tab, dragged: its window moves, nothing torn off" + at);
    SendMessageW(t2.top, WM_EXITSIZEMOVE, 1, 0);  // the move loop ended by Escape
    expect(a.received.empty(), "its own move cancelled: nothing offered (it wasn't torn off by this drag)" + at);

    // Torn off from A and cancelled with Escape: offered back to A at its index.
    a.tabs->add({"k", "Tab k", {}, {}});
    a.made = nullptr;
    p = a.centre(0);  // a
    a.down(p);
    a.move(POINT{p.x + th + 1, p.y});
    a.move(POINT{p.x, below.y});
    if (!a.made) return expect(false, "torn off a third time" + at);
    fixture& t3 = *a.made;
    SendMessageW(t3.top, WM_EXITSIZEMOVE, 1, 0);  // the move loop ended by Escape
    expect(a.received.size() == 1 && a.received[0].id == "a" && a.received[0].index == 0 &&
               a.received[0].source == t3.win.get(),
           "Escape at the torn-off window's move: offered back to A at index 0" + at);
    expect(a.tabs->ids() == ids{"a", "k"} && t3.tabs->ids().empty(), "and taken back" + at + ": " + join(a.tabs->ids()));

    // No group: Escape sends it home all the same (a group limits drops on
    // other windows' strips, not going back).
    fixture& e = *make("E" + tag, {"e1", "e2"}, "");
    p = e.centre(0);
    e.down(p);
    e.move(POINT{p.x + th + 1, p.y});
    e.move(POINT{p.x, below.y});
    if (!e.made) return expect(false, "torn off with no group" + at);
    fixture& te = *e.made;
    expect(e.tabs->ids() == ids{"e2"} && HWND(ask(a.top, 3)) == te.top,
           "no group: torn off all the same, its window moving" + at);
    SendMessageW(te.top, WM_EXITSIZEMOVE, 1, 0);  // the move loop ended by Escape
    expect(e.received.size() == 1 && e.received[0].id == "e1" && e.received[0].index == 0 &&
               e.received[0].source == te.win.get(),
           "no group, Escape: sent home, E's on_receive(e1, 0, the torn-off window)" + at);
    expect(e.tabs->ids() == ids{"e1", "e2"} && te.tabs->ids().empty(),
           "and back in E, the torn-off window empty (the app's to close)" + at + ": " + join(e.tabs->ids()));

    // Straight onto another window's strip, no tearing off.
    a.tabs->add({"m", "Tab m", {}, {}});
    b.received.clear();
    a.detached.clear();
    p = a.centre(2);  // m
    a.down(p);
    a.move(POINT{p.x + th + 1, p.y});
    const RECT by = b.tab(1);
    a.move(a.client(b.screen(POINT{by.right - 2, (by.top + by.bottom) / 2})));
    expect(b.drop_mark() == 2 && a.tabs->ids() == ids{"a", "k", "m"} && a.detached.empty(),
           "dragged onto B's strip: B marks index 2, A keeps it in place, nothing torn off" + at + " (" +
               std::to_string(b.drop_mark()) + ")");
    a.move(a.centre(0));
    expect(b.drop_mark() == -1 && a.tabs->ids() == ids{"m", "a", "k"},
           "back over its own: along its row again" + at + ": " + join(a.tabs->ids()));
    a.move(a.client(b.screen(POINT{by.right - 2, (by.top + by.bottom) / 2})));
    a.up(a.client(b.screen(POINT{by.right - 2, (by.top + by.bottom) / 2})));
    expect(b.received.size() == 1 && b.received[0].id == "m" && b.received[0].index == 2 &&
               b.received[0].source == a.win.get(),
           "let go there: B's on_receive(m, 2, A)" + at);
    expect(b.tabs->ids() == ids{"x", "b", "m", "y"} && a.tabs->ids() == ids{"a", "k"} && b.drop_mark() == -1,
           "and the app moved it" + at + ": " + join(b.tabs->ids()));

    // Onto C, another group: nothing marked (it'd be torn off there: refused).
    a.tabs->add({"n", "Tab n", {}, {}});
    a.refuse_detach = true;
    p = a.centre(1);
    a.down(p);
    a.move(POINT{p.x + th + 1, p.y});
    const POINT on_c = a.client(c.screen(c.centre(0)));
    a.move(on_c);
    expect(c.drop_mark() == -1, "dragged onto C's strip, another group: not marked" + at);
    a.escape();
    a.up(on_c);
    expect(c.received.empty() && a.tabs->ids() == ids{"a", "k", "n"}, "and asked nothing" + at);
    a.refuse_detach = false;

    // The only tab: its window moves, and it's dropped on B.
    b.received.clear();
    p = d.centre(0);
    d.down(p);
    d.move(POINT{p.x + th + 1, p.y});
    expect(HWND(ask(a.top, 3)) == d.top && d.detached.empty(),
           "the only tab dragged: D itself moves (no on_detach)" + at);
    GetWindowRect(d.top, &tr);
    move_window(d.top, d.screen(POINT{p.x + th + 1, p.y}), b_first, POINT{tr.left, tr.top});
    expect(b.drop_mark() == 0, "D over B's strip: marked" + at);
    SendMessageW(d.top, WM_EXITSIZEMOVE, 0, 0);
    expect(b.received.size() == 1 && b.received[0].id == "solo" && b.received[0].source == d.win.get() &&
               d.tabs->ids().empty(),
           "let go: B takes solo from D, D left empty (the app's to close)" + at);
    expect(IsWindow(d.top), "rapport doesn't close it" + at);

    // The right-click menu: the fixed items, then the app's.
    tab_menu_capture cap{};
    cap.choose = -1;
    ask(a.top, 4, reinterpret_cast<LPARAM>(&cap));
    a.tabs->add({"o", "Tab o", {}, {}});
    const POINT o_at = a.screen(a.centre(0));
    SendMessageW(a.strip, WM_CONTEXTMENU, WPARAM(a.strip), MAKELPARAM(o_at.x, o_at.y));
    auto label = [&](int i) { return i < cap.count ? std::wstring(cap.labels[i]) : std::wstring(L"?"); };
    expect(cap.count == 7 && label(0) == L"_New tab" && label(1).empty() && label(2) == L"_Close tab" &&
               label(3) == L"_Move to new window" && label(4).empty() && label(5) == L"_Rename a" &&
               label(6) == L"_Duplicate",
           "the menu: New tab, Close tab, Move to new window, then the app's after a separator" + at + " (" +
               std::to_string(cap.count) + ")");
    expect(cap.count > 3 && cap.enabled[3], "Move to new window enabled, with tabs to spare" + at);
    // Move to new window: on_detach with no point.
    a.detached.clear();
    cap = tab_menu_capture{};
    cap.choose = 3;
    SendMessageW(a.strip, WM_CONTEXTMENU, WPARAM(a.strip), MAKELPARAM(o_at.x, o_at.y));
    expect(a.detached.size() == 1 && a.detached[0].first == "a" && !a.detached[0].second,
           "Move to new window: on_detach(a) with no point" + at);
    expect(!ask(a.top, 3), "and nothing's moving: the app places the window" + at);
    // A window's only tab: the item disabled.
    cap = tab_menu_capture{};
    cap.choose = -1;
    fixture& lone = *a.made;
    const POINT lone_at = lone.screen(lone.centre(0));
    SendMessageW(lone.strip, WM_CONTEXTMENU, WPARAM(lone.strip), MAKELPARAM(lone_at.x, lone_at.y));
    expect(cap.count > 3 && !cap.enabled[3], "the only tab: Move to new window disabled" + at);
    // The keys, where the strip takes the focus (the caption's).
    if (a.caption)
    {
        cap = tab_menu_capture{};
        cap.choose = -1;
        // As DefWindowProc has them: the menu as the key's let go.
        SendMessageW(a.strip, WM_KEYDOWN, VK_APPS, 1);
        SendMessageW(a.strip, WM_KEYUP, VK_APPS, 1 | (LPARAM(3) << 30));
        expect(cap.count == 7 && cap.opened == 1, "the Menu key: the menu, once" + at);
        cap = tab_menu_capture{};
        cap.choose = -1;
        set_keys({VK_SHIFT, VK_LSHIFT});
        SendMessageW(a.strip, WM_SYSKEYDOWN, VK_F10, 1);
        SendMessageW(a.strip, WM_SYSKEYUP, VK_F10, 1 | (LPARAM(3) << 30));
        set_keys({});
        expect(cap.count == 7 && cap.opened == 1, "Shift+F10: the menu, once" + at);
    }
    ask(a.top, 4, 0);
    ask(a.top, 0, 0);
    stack({});
    ask(a.top, 1, 0);
}

// -- --input: the real thing, CI only ----------------------------------------
//
// Shown windows, the real mouse and keys (SendInput): a tab dragged out of
// A tears off into the app's window, which the system's move loop carries
// with the pointer onto B's strip, where it's let go: B's on_receive; one
// torn off and Escape pressed mid-move: offered back to A; a window's only
// tab dragged: the window moves, and dropped on B. The z-order is the
// desktop's (no stand-in), and nothing's simulated. Real input moves the
// pointer and presses keys on whatever desktop it runs on, so it runs only
// on a CI runner: GITHUB_ACTIONS=true and RAPPORT_TEST_SEND_INPUT=1.

bool env_is(const char* name, const char* value)
{
    const char* v = std::getenv(name);
    return v && std::strcmp(v, value) == 0;
}

void send_mouse(POINT screen, DWORD flags)
{
    const int x0 = GetSystemMetrics(SM_XVIRTUALSCREEN), y0 = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = std::max(2, GetSystemMetrics(SM_CXVIRTUALSCREEN)), h = std::max(2, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = LONG((int64_t(screen.x - x0) * 65535) / (w - 1));
    in.mi.dy = LONG((int64_t(screen.y - y0) * 65535) / (h - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | flags;
    SendInput(1, &in, sizeof in);
}
void send_key(WORD vk, bool up)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
}

// Steps, one a timer tick: the input's taken by the loop (or the move
// loop, which dispatches the timer too) between them.
std::vector<std::function<void()>> steps;

// A drag in steps: pressed at `from`, through `via`, let go at `to`
// (or not, `release` false), each move a step of its own.
void drag(POINT from, std::vector<POINT> via, POINT to, bool release = true)
{
    steps.push_back([from] { send_mouse(from, 0); });
    steps.push_back([from] { send_mouse(from, MOUSEEVENTF_LEFTDOWN); });
    via.push_back(to);
    POINT last = from;
    for (POINT p : via)
    {
        for (int k = 1; k <= 6; ++k)
        {
            const POINT q{last.x + (p.x - last.x) * k / 6, last.y + (p.y - last.y) * k / 6};
            steps.push_back([q] { send_mouse(q, 0); });
        }
        last = p;
    }
    if (release) steps.push_back([to] { send_mouse(to, MOUSEEVENTF_LEFTUP); });
    steps.push_back([] {});  // the drop's handled
}

int input_mode(rap::ui& ui)
{
    if (!env_is("GITHUB_ACTIONS", "true") || !env_is("RAPPORT_TEST_SEND_INPUT", "1"))
    {
        std::printf("skipped: real input (SendInput) only on a CI runner "
                    "(GITHUB_ACTIONS=true, RAPPORT_TEST_SEND_INPUT=1)\n");
        return 77;
    }
    g_menu_bar = windows_build() < 22000;  // the caption's strip where there is one
    fixture& a = *make("Input A", {"a", "b", "c"}, "g");
    fixture& b = *make("Input B", {"x", "y"}, "g");
    fixture& d = *make("Input D", {"solo"}, "g");
    fixture& u = *make("Input U", {"u1", "u2"}, "");  // no group
    if (!a.strip || !b.strip || !d.strip) return expect(false, "the windows' strips"), 1;
    for (fixture* f : {&a, &b, &d, &u}) f->win->show();
    place(a, 80, 80);
    place(b, 80, 520);
    place(d, 840, 80);
    place(u, 840, 520);
    SetForegroundWindow(a.top);
    pump();
    using ids = std::vector<std::string>;
    // A's b, torn off downward, carried onto B's first tab's right half.
    const POINT from = a.screen(a.centre(1));
    RECT ar;
    GetWindowRect(a.top, &ar);
    const RECT bx = b.tab(0);
    const POINT over_b = b.screen(POINT{bx.right - 6, (bx.top + bx.bottom) / 2});
    const POINT on_b = b.screen(POINT{bx.left + 6, (bx.top + bx.bottom) / 2});
    const POINT out{from.x, ar.bottom - 40}, out2{from.x, ar.bottom + 60};
    steps.push_back([] {});
    drag(from, {POINT{from.x + 12, from.y}, out}, over_b);
    steps.push_back(
        [&]
        {
            expect(a.detached.size() == 1 && a.detached[0].first == "b", "real input: torn off, on_detach(b) once");
            expect(a.made && b.received.size() == 1 && b.received[0].id == "b" && b.received[0].index == 1 &&
                       b.received[0].source == a.made->win.get(),
                   "carried by the move loop onto B and let go: on_receive(b, 1, the torn-off window)");
            expect(b.tabs->ids() == ids{"x", "b", "y"} && a.tabs->ids() == ids{"a", "c"},
                   "and the app moved it: " + join(b.tabs->ids()));
            if (a.made) a.made->win->close();  // empty: the app's to close
            a.made = nullptr;
        });
    // Then c (where b was) torn off, and Escape pressed mid-move: offered
    // back to A.
    drag(from, {POINT{from.x + 12, from.y}}, out2, false);
    steps.push_back([] { send_key(VK_ESCAPE, false); });
    steps.push_back([] { send_key(VK_ESCAPE, true); });
    steps.push_back([out2] { send_mouse(out2, MOUSEEVENTF_LEFTUP); });
    steps.push_back([] {});
    steps.push_back(
        [&]
        {
            expect(a.received.size() == 1 && a.received[0].id == "c" && a.received[0].index == 1,
                   "Escape mid-move: offered back to A at its index");
            expect(a.tabs->ids() == ids{"a", "c"}, "and taken back: " + join(a.tabs->ids()));
            if (a.made) a.made->win->close();
            b.received.clear();
        });
    // U's u1 (no group) torn off and Escape pressed mid-move: sent home all
    // the same.
    const POINT from_u = u.screen(u.centre(0));
    RECT ur;
    GetWindowRect(u.top, &ur);
    const POINT out_u{from_u.x, ur.bottom + 60};
    drag(from_u, {POINT{from_u.x + 12, from_u.y}}, out_u, false);
    steps.push_back([] { send_key(VK_ESCAPE, false); });
    steps.push_back([] { send_key(VK_ESCAPE, true); });
    steps.push_back([out_u] { send_mouse(out_u, MOUSEEVENTF_LEFTUP); });
    steps.push_back([] {});
    steps.push_back(
        [&]
        {
            expect(u.received.size() == 1 && u.received[0].id == "u1" && u.received[0].index == 0,
                   "no group, Escape mid-move: sent home to U at its index");
            expect(u.tabs->ids() == ids{"u1", "u2"}, "and taken back: " + join(u.tabs->ids()));
            if (u.made) u.made->win->close();
        });
    // D's only tab: D itself moves, and it's dropped on B.
    const POINT from3 = d.screen(d.centre(0));
    drag(from3, {POINT{from3.x + 12, from3.y}}, on_b);
    steps.push_back(
        [&]
        {
            expect(d.detached.empty() && b.received.size() == 1 && b.received[0].id == "solo" &&
                       b.received[0].source == d.win.get(),
                   "the only tab dragged: its window moved onto B, which takes it");
        });
    size_t step = 0;
    rap::subscription tick = ui.timer(60, true,
                                      [&]
                                      {
                                          if (step < steps.size())
                                          {
                                              auto s = steps[step++];
                                              s();
                                              return;
                                          }
                                          ui.quit(0);
                                      });
    ui.run();
    send_mouse(POINT{0, 0}, MOUSEEVENTF_LEFTUP);
    fixtures.clear();
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options options{.backend = "win32"};
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 2;
    g_ui = ui->get();
    std::printf("build %lu\n", windows_build());
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--input") == 0) return input_mode(**ui);
    g_menu_bar = true;
    run("stage 1, under a menu bar");
    if (windows_build() >= 22000)
    {
        g_menu_bar = false;
        run("in the caption");
    }
    else
        std::printf("     no caption strip before Windows 11: stage 1 only\n");
    pump();
    set_keys({});
    for (auto& f : fixtures) expect(f->top && !IsWindowVisible(f->top), "never shown, to the end: " + f->name);
    fixtures.clear();
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
