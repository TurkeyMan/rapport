// A window's document tabs dragged out of their windows and dropped, on
// Windows: stage 1's tab control (under a menu bar) and Windows 11's strip in
// the caption where there is one. A tab dragged is only ever the tab: along
// its row it reorders; past it, it's a drag image (a layered, topmost popup
// that never takes activation and that hit tests pass through) that follows
// the pointer, its row showing the gap where it was; nothing is asked until
// it's let go. Let go over another window's row in the group, that window's
// on_receive; anywhere else, on_detach, and the app's window put so its tab
// lands under the pointer (kept to the monitor's work area), then shown. No
// window is ever moved by the drag, nor put in the system's move loop.
//
// The windows are never shown: the strips are sent the mouse's messages and
// keys, nothing else. Test hooks (the "rapport-document-tabs-drag" message)
// stand in for what needs shown windows: the drop lookup's z-order
// (WindowFromPoint sees only shown windows) is a list of the test's windows,
// and in their test mode the drag image is made but never shown, and the
// window let go outside the rows is placed but not shown, noted instead.
//
// Checked, in each strip: a drag along the row reordering, on_reorder once;
// dragged out, the image made (its styles), following the pointer, the tab
// not yet out of its window and nothing asked; back along the row, the image
// gone; let go outside with a window returned: on_detach once then, with the
// pointer's place, the window put with the tab under the pointer (where it
// was held), shown (noted), the image gone; near the work area's corner, the
// window kept to the work area; a null return: nothing changed, nothing
// shown; let go over another window's row in the group: its on_receive with
// the index and the source, the app moving it; another group's row not
// marked; Escape, the capture lost and the window deactivated each putting
// the tab back, the image gone and nothing asked; a window's only tab
// dragged out and let go: nothing (the window not moved), back on its own
// row nothing, onto another row moved there, the app closing the emptied
// window in on_receive; on_detach closing its own window; never the move
// loop (no WM_ENTERSIZEMOVE, no SC_MOVE). And the right-click menu, as
// before: the app's items after the fixed ones, its Move to new window
// asking on_detach with no point; in the caption, Shift+F10 and the Menu key.
//
// The real thing (shown windows, the desktop's z-order, real input) is CI's:
// rapport-win32-document-tabs-tearoff-test --input runs only on a CI runner
// (GITHUB_ACTIONS=true and RAPPORT_TEST_SEND_INPUT=1).
//   rapport-win32-document-tabs-tearoff-test
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which they need.
#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <chrono>
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

// The system's move loop, entered by any window of this thread: never, by a
// tab drag.
int loop_seen = 0;
LRESULT CALLBACK watch_sent(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        const auto* c = reinterpret_cast<const CWPSTRUCT*>(lp);
        if (c->message == WM_ENTERSIZEMOVE || (c->message == WM_SYSCOMMAND && (c->wParam & 0xFFF0) == SC_MOVE))
            ++loop_seen;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
LRESULT CALLBACK watch_posted(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        const auto* m = reinterpret_cast<const MSG*>(lp);
        if (m->message == WM_SYSCOMMAND && (m->wParam & 0xFFF0) == SC_MOVE) ++loop_seen;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
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
HWND g_hook_window = nullptr;  // a window kept to the end, for the test hooks
SIZE g_size{700, 300};          // the windows' size (logical px)

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
    std::vector<std::pair<std::string, int>> reordered;
    // How it answers.
    bool refuse_detach = false, accept = true;
    bool close_emptied = false;    // on_receive closes the source it empties, there and then
    bool close_on_detach = false;  // on_detach closes this window once it's made the new one
    fixture* made = nullptr;       // the window on_detach made last

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
    // A strip's point on the screen.
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
    RECT window_rect() const
    {
        RECT r{};
        GetWindowRect(top, &r);
        return r;
    }
    // Pressed on tab i and moved past the threshold, along the row.
    POINT start(int i)
    {
        const POINT p = centre(i);
        down(p);
        move(POINT{p.x + threshold() + 1, p.y});
        return p;
    }
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
        .on_reorder([self](const std::string& id, int index) { self->reordered.push_back({id, index}); })
        .on_detach(
            [self, group](const std::string& id, std::optional<rap::point_f> at) -> rap::window*
            {
                self->detached.push_back({id, at});
                if (self->refuse_detach) return nullptr;
                fixture* made = make(self->name + "+" + id, {id}, group);
                self->tabs->remove(id);
                self->made = made;
                rap::window* w = made->win.get();
                if (self->close_on_detach)
                {
                    self->tabs = nullptr;
                    self->win.reset();
                }
                return w;
            })
        .on_receive(
            [self](const rap::document_tabs::transfer& t)
            {
                self->received.push_back({t.id, t.index, t.source});
                if (!self->accept) return false;
                for (auto& o : fixtures)
                    if (o->win && o->win.get() == t.source)
                    {
                        o->tabs->remove(t.id);
                        self->tabs->insert(t.index, {t.id, t.title, t.icon, t.tooltip});
                        // As an app may: the emptied window closed at once,
                        // in the middle of the drag's end.
                        if (self->close_emptied && o->tabs->ids().empty())
                        {
                            o->tabs = nullptr;
                            o->win.reset();
                        }
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
    w.size(rap::extent{float(g_size.cx), rap::extent::unit::px}, rap::extent{float(g_size.cy), rap::extent::unit::px});
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
    ask(g_hook_window, 1, reinterpret_cast<LPARAM>(v.data()));
}

std::string pt(std::optional<rap::point_f> p)
{
    return p ? std::to_string(int(p->x)) + "," + std::to_string(int(p->y)) : "none";
}
std::string pt(POINT p)
{
    return std::to_string(p.x) + "," + std::to_string(p.y);
}

// The drag image now (none: null).
HWND image()
{
    return HWND(ask(g_hook_window, 3));
}
// As the spec has it: a popup, layered, topmost, never activated, a tool
// window (not on the taskbar), transparent to hit tests; and never shown
// here.
bool image_styled(HWND h)
{
    const LONG_PTR need = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT;
    return h && IsWindow(h) && (GetWindowLongPtrW(h, GWL_EXSTYLE) & need) == need &&
           (GetWindowLongPtrW(h, GWL_STYLE) & WS_POPUP) && !IsWindowVisible(h);
}

bool same(const RECT& x, const RECT& y)
{
    return EqualRect(&x, &y) != FALSE;
}

// A window's frame as the desktop shows it (its invisible resize borders
// not counted), as rapport keeps it to the work area.
RECT frame_of(HWND h)
{
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) || IsRectEmpty(&r))
        GetWindowRect(h, &r);
    return r;
}

std::string where(const RECT& r)
{
    return std::to_string(r.left) + "," + std::to_string(r.top) + "-" + std::to_string(r.right) + "," +
           std::to_string(r.bottom);
}

RECT work_area()
{
    RECT r{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    return r;
}

// Where a window's frame goes, let go with its first tab held at `grab`
// (from the tab's top-left) at `at` on the screen, the tab's top-left
// `off` from its frame's and the frame `size` big: before it's kept to
// the work area, and after (as rapport keeps it: moved in, left and top
// first).
RECT frame_for(POINT at, POINT grab, POINT off, SIZE size)
{
    const LONG l = at.x - grab.x - off.x, t = at.y - grab.y - off.y;
    return RECT{l, t, l + size.cx, t + size.cy};
}
RECT kept_in(RECT f, const RECT& work)
{
    const LONG w = f.right - f.left, h = f.bottom - f.top;
    const LONG l = std::max(work.left, std::min(f.left, work.right - w));
    const LONG t = std::max(work.top, std::min(f.top, work.bottom - h));
    return RECT{l, t, l + w, t + h};
}
bool inside(const RECT& f, const RECT& work)
{
    return f.left >= work.left && f.top >= work.top && f.right <= work.right && f.bottom <= work.bottom;
}

int timers()
{
    return int(ask(g_hook_window, 7));
}
int route()
{
    return int(ask(g_hook_window, 8));
}

void run(const char* stage, bool last)
{
    const std::string at = std::string(" (") + stage + ")";
    const std::string tag = g_menu_bar ? "1" : "2";
    using ids = std::vector<std::string>;
    fixture& a = *make("A" + tag, {"a", "b", "c"}, "g");
    fixture& b = *make("B" + tag, {"x", "y"}, "g");
    fixture& c = *make("C" + tag, {"p", "q"}, "other");
    fixture& d = *make("D" + tag, {"solo"}, "g");
    expect(a.top && b.top && c.top && d.top && a.strip && b.strip && !IsWindowVisible(a.top),
           "the windows and their strips, never shown" + at);
    if (!a.strip || !b.strip || !c.strip || !d.strip) return;
    expect(a.caption == !g_menu_bar, "the strip expected" + at);
    g_hook_window = b.top;
    // A at the work area's top left: a window let go from it fits in the
    // work area as it's put, even on a small screen (a runner's 1024 x 768),
    // so it's put exactly, not kept in.
    const RECT work = work_area();
    place(a, work.left + 20, work.top + 20);
    place(b, 100, 700);
    place(c, 900, 700);
    place(d, 900, 100);
    stack({a.top, b.top, c.top, d.top});
    ask(b.top, 0, 1);  // test mode: nothing shown
    ask(b.top, 6, 0);  // Escape up, whatever the desktop's key does meanwhile
    loop_seen = 0;
    const int th = a.threshold();
    RECT strip_rect;
    GetClientRect(a.strip, &strip_rect);
    const int below = strip_rect.bottom + 200;  // past the tear-off distance, under A's strip
    const RECT a_was = a.window_rect();

    // Along the row: reordered, on_reorder once, no image.
    POINT p = a.start(0);
    a.move(a.centre(1));
    expect(a.tabs->ids() == ids{"b", "a", "c"} && !image(), "along the row: the others make room, no image" + at);
    a.up(a.centre(1));
    expect(a.reordered.size() == 1 && a.reordered[0].first == "a" && a.reordered[0].second == 1,
           "let go: on_reorder(a, 1) once" + at);
    a.reordered.clear();
    // Back as it was: a, b, c.
    a.start(1);
    a.move(a.centre(0));
    a.up(a.centre(0));
    expect(a.tabs->ids() == ids{"a", "b", "c"}, "and back" + at + ": " + join(a.tabs->ids()));
    a.reordered.clear();

    // Out of the row: the drag image, nothing asked yet.
    const RECT b_tab = a.tab(1);
    p = a.start(1);
    const POINT grab{p.x - b_tab.left, p.y - b_tab.top};
    POINT out{p.x, below};
    a.move(out);
    HWND img = image();
    expect(img != nullptr, "dragged out of the row: a drag image" + at);
    expect(image_styled(img), "layered, topmost, no activation, a tool window, hit tests through; not shown here" + at);
    expect(a.detached.empty() && a.tabs->ids() == ids{"a", "b", "c"},
           "nothing asked yet, the tab still in its window" + at + ": " + join(a.tabs->ids()));
    if (img)
    {
        RECT ir;
        GetWindowRect(img, &ir);
        const POINT s = a.screen(out);
        expect(ir.left == s.x - grab.x && ir.top == s.y - grab.y,
               "the image held under the pointer where the tab was grabbed" + at + ": " + pt(POINT{ir.left, ir.top}) +
                   " for " + pt(POINT{s.x - grab.x, s.y - grab.y}));
        expect(ir.right - ir.left == b_tab.right - b_tab.left, "the tab's width" + at);
    }
    out = POINT{p.x + 40, below + 30};
    a.move(out);
    if (img && IsWindow(img))
    {
        RECT ir;
        GetWindowRect(img, &ir);
        const POINT s = a.screen(out);
        expect(ir.left == s.x - grab.x && ir.top == s.y - grab.y, "and following it" + at);
    }
    // Back along the row: no image, reordering again.
    a.move(a.centre(2));
    expect(!image() && !IsWindow(img) && a.tabs->ids() == ids{"a", "c", "b"},
           "back along the row: the image gone, reordering" + at + ": " + join(a.tabs->ids()));
    a.escape();
    expect(a.tabs->ids() == ids{"a", "b", "c"}, "Escape: back where it was" + at);
    a.up(a.centre(2));
    expect(a.detached.empty() && a.reordered.empty(), "and nothing told" + at);

    // Escape with the image out: back, the image gone, nothing asked, then or
    // as it's let go.
    p = a.start(1);
    a.move(POINT{p.x, below});
    img = image();
    a.escape();
    expect(img && !IsWindow(img) && !image() && a.tabs->ids() == ids{"a", "b", "c"},
           "Escape out of the row: the image gone, the tab back" + at);
    expect(timers() == 0 && route() == 2, "the strip's own WM_KEYDOWN heard it, no Escape timer left" + at);
    a.up(POINT{p.x, below});
    expect(a.detached.empty() && a.reordered.empty() && !ask(b.top, 5), "and let go after: nothing asked" + at);

    // The capture lost: the same.
    p = a.start(2);
    a.move(POINT{p.x, below});
    img = image();
    SendMessageW(a.strip, WM_CAPTURECHANGED, 0, 0);
    expect(img && !IsWindow(img) && !image() && a.tabs->ids() == ids{"a", "b", "c"},
           "the capture lost: the image gone, the tab back" + at);
    expect(timers() == 0, "no Escape timer left" + at);
    a.up(POINT{p.x, below});
    expect(a.detached.empty() && a.reordered.empty(), "and nothing asked" + at);
    // The window deactivated: the same.
    p = a.start(0);
    a.move(a.centre(2));
    a.move(POINT{p.x, below});
    img = image();
    SendMessageW(a.top, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);
    expect(img && !IsWindow(img) && !image() && a.tabs->ids() == ids{"a", "b", "c"},
           "the window deactivated: the image gone, the tab back" + at + ": " + join(a.tabs->ids()));
    expect(timers() == 0, "no Escape timer left" + at);
    a.up(POINT{p.x, below});
    expect(a.detached.empty() && a.reordered.empty(), "and nothing asked" + at);

    // Escape seen however the focus is set (a drag from a window that isn't
    // the foreground's: keys go elsewhere): its state polled, as OLE's
    // DoDragDrop does, on each move and on a timer while the drag holds the
    // mouse. The test hook stands in for the key's state.
    ask(b.top, 6, 0);  // Escape up
    p = a.start(1);
    expect(ask(b.top, 7) == 1, "a drag on: Escape watched, on its timer" + at);
    a.move(POINT{p.x, below});
    img = image();
    ask(b.top, 6, 1);  // Escape down
    a.move(POINT{p.x + 5, below});
    expect(img && !IsWindow(img) && !image() && a.tabs->ids() == ids{"a", "b", "c"} && route() == 3,
           "Escape down, seen on the next move: the image gone, the tab back" + at);
    ask(b.top, 6, 0);
    a.up(POINT{p.x + 5, below});
    expect(a.detached.empty() && a.reordered.empty() && ask(b.top, 7) == 0,
           "and nothing asked, the timer gone" + at);
    // Seen on the timer, with no move.
    p = a.start(1);
    a.move(POINT{p.x, below});
    img = image();
    ask(b.top, 6, 1);
    {
        const DWORD until = GetTickCount() + 2000;
        while (image() && GetTickCount() < until)
        {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
            pump();
        }
    }
    expect(img && !IsWindow(img) && !image() && a.tabs->ids() == ids{"a", "b", "c"} && ask(b.top, 7) == 0 &&
               route() == 4,
           "Escape down, seen on the timer: the image gone, the tab back, the timer gone" + at);
    ask(b.top, 6, 0);
    a.up(POINT{p.x, below});
    expect(a.detached.empty() && a.reordered.empty(), "and nothing asked" + at);
    // Escape already down as the tab's pressed: not a press (it must be
    // pressed during the drag).
    ask(b.top, 6, 1);
    p = a.start(1);
    a.move(POINT{p.x, below});
    expect(image() != nullptr, "Escape held from before the press: the drag goes on" + at);
    ask(b.top, 6, 0);
    a.move(POINT{p.x + 3, below});
    ask(b.top, 6, 1);
    a.move(POINT{p.x + 6, below});
    expect(!image() && a.tabs->ids() == ids{"a", "b", "c"}, "and let up and pressed again: cancelled" + at);
    ask(b.top, 6, 0);
    a.up(POINT{p.x + 6, below});
    // Escape pressed, then let go before any poll (no move, no timer): the
    // release polls it first, and cancels rather than tearing off.
    a.refuse_detach = true;
    p = a.start(1);
    a.move(POINT{p.x, below});
    img = image();
    ask(b.top, 6, 1);
    a.up(POINT{p.x, below});
    expect(a.detached.empty() && img && !IsWindow(img) && !image() && a.tabs->ids() == ids{"a", "b", "c"} &&
               timers() == 0 && route() == 5,
           "Escape down, let go before it's polled: cancelled as it's let go, nothing asked" + at + " (" +
               std::to_string(a.detached.size()) + ", route " + std::to_string(route()) + ")");
    ask(b.top, 6, 0);
    // Likewise let go over B's row: not dropped.
    b.received.clear();
    p = a.start(1);
    {
        const RECT bx0 = b.tab(0);
        const POINT on_b = a.client(b.screen(POINT{bx0.right - 4, (bx0.top + bx0.bottom) / 2}));
        a.move(on_b);
        ask(b.top, 6, 1);
        a.up(on_b);
    }
    expect(b.received.empty() && a.detached.empty() && a.tabs->ids() == ids{"a", "b", "c"} && timers() == 0 &&
               b.drop_mark() == -1,
           "and over B's row: not dropped" + at);
    a.refuse_detach = false;
    ask(b.top, 6, 0);  // up, for the rest
    expect(a.detached.empty() && ask(b.top, 7) == 0, "nothing asked, no timer" + at);

    // Let go outside, refused: on_detach once, nothing changes.
    a.refuse_detach = true;
    p = a.start(1);
    a.move(POINT{p.x, below});
    img = image();
    a.up(POINT{p.x, below});
    const POINT let_go = a.screen(POINT{p.x, below});
    expect(a.detached.size() == 1 && a.detached[0].first == "b",
           "let go outside: on_detach(b) once, as it's let go" + at + " (" + std::to_string(a.detached.size()) + ")");
    if (!a.detached.empty())
    {
        const float s = float(GetDpiForWindow(a.top)) / 96.f;
        const auto& q = a.detached[0].second;
        expect(q && std::abs(q->x - let_go.x / s) < 1 && std::abs(q->y - let_go.y / s) < 1,
               "with the pointer's place, in the window's logical px" + at + ": " + pt(q));
    }
    expect(a.tabs->ids() == ids{"a", "b", "c"} && !ask(b.top, 5) && img && !IsWindow(img) && !image(),
           "null: nothing changed, nothing shown, the image gone" + at);
    expect(timers() == 0, "no Escape timer left" + at);
    a.refuse_detach = false;
    a.detached.clear();

    // Let go outside: the app's window, put with the tab under the pointer.
    // Its frame is A's (the same window, but for its tabs), its first tab as
    // far from its frame's corner: let go where that frame fits in the work
    // area with room (over A's own content, out past its row), it's put
    // exactly so, not kept in.
    const RECT a_frame = frame_of(a.top);
    const SIZE frame_size{a_frame.right - a_frame.left, a_frame.bottom - a_frame.top};
    POINT off{};
    {
        const RECT r = a.tab(0);
        const POINT o = a.screen(POINT{r.left, r.top});
        off = POINT{o.x - a_frame.left, o.y - a_frame.top};
    }
    std::printf("     work area %s, A's frame %s, its first tab %ld,%ld in it%s\n", where(work).c_str(),
                where(a_frame).c_str(), off.x, off.y, at.c_str());
    const POINT dropped{work.left + 40 + off.x + grab.x, work.top + 200 + off.y + grab.y};
    const RECT room = frame_for(dropped, grab, off, frame_size);
    p = a.start(1);
    out = a.client(dropped);
    a.move(POINT{p.x, below});
    a.move(out);
    img = image();
    a.up(out);
    expect(a.detached.size() == 1 && a.made && a.tabs->ids() == ids{"a", "c"},
           "let go outside: on_detach(b), the app's window made, b out of A" + at + ": " + join(a.tabs->ids()));
    expect(img && !IsWindow(img) && !image(), "the image gone" + at);
    if (!a.made) return;
    fixture& t = *a.made;
    expect(t.tabs && t.tabs->ids() == ids{"b"} && !IsWindowVisible(t.top), "the new window holds b, never shown" + at);
    expect(HWND(ask(b.top, 5)) == t.top, "and it's shown (here, noted)" + at);
    if (inside(room, work))
    {
        const RECT r = t.tab(0);
        const POINT o = t.screen(POINT{r.left, r.top});
        expect(o.x + grab.x == dropped.x && o.y + grab.y == dropped.y,
               "put with its tab under the pointer, held where it was grabbed" + at + ": " +
                   pt(POINT{o.x + grab.x, o.y + grab.y}) + " for " + pt(dropped) + " (frame " +
                   where(frame_of(t.top)) + ", work area " + where(work) + ")");
    }
    else
        std::printf("     skipped: no room in the work area %s for the frame %s, to check the tab under the "
                    "pointer%s\n",
                    where(work).c_str(), where(room).c_str(), at.c_str());

    // Let go by the work area's right edge, and in its bottom right corner:
    // kept in, where kept_in says (moved left, and up), the tab not under the
    // pointer then.
    const POINT edge{work.right - 3, dropped.y};
    const POINT corner{work.right - 3, work.bottom - 3};
    for (const POINT s : {edge, corner})
    {
        const std::string which = s.y == edge.y ? "by the work area's right edge" : "in the work area's corner";
        a.made = nullptr;
        p = a.start(1);  // c
        a.move(POINT{p.x, below});
        a.move(a.client(s));
        a.up(a.client(s));
        if (!a.made)
        {
            expect(false, "let go " + which + ": a window made" + at);
            continue;
        }
        const RECT got = frame_of(a.made->top);
        const RECT want = kept_in(frame_for(s, grab, off, frame_size), work);
        expect(EqualRect(&got, &want) != FALSE, "let go " + which + ": kept in, at " + where(want) + at + ": " +
                                                    where(got) + " (work area " + where(work) + ")");
        expect(HWND(ask(b.top, 5)) == a.made->top, "and shown (noted)" + at);
        a.made->win.reset();
        a.tabs->add({"c", "Tab c", {}, {}});
    }
    expect(a.tabs->ids() == ids{"a", "c"}, "A: a, c" + at + ": " + join(a.tabs->ids()));

    // Over B's row: B marks where it'd land; let go there: B's on_receive.
    a.detached.clear();
    a.tabs->add({"m", "Tab m", {}, {}});
    const RECT bx = b.tab(0);
    const POINT over_b = b.screen(POINT{bx.right - 4, (bx.top + bx.bottom) / 2});
    p = a.start(2);  // m
    a.move(POINT{p.x, below});
    a.move(a.client(over_b));
    expect(b.drop_mark() == 1 && image() && a.tabs->ids() == ids{"a", "c", "m"} && a.detached.empty(),
           "over B's first tab's right half: B marks 1, the image out, A as it was" + at + " (" +
               std::to_string(b.drop_mark()) + ")");
    expect(c.drop_mark() == -1 && a.drop_mark() == -1 && d.drop_mark() == -1, "and no other row" + at);
    // Over C's row, another group: no mark.
    const POINT over_c = c.screen(c.centre(0));
    a.move(a.client(over_c));
    expect(c.drop_mark() == -1 && b.drop_mark() == -1, "over C's row, another group: nothing marked" + at);
    a.move(a.client(over_b));
    img = image();
    a.up(a.client(over_b));
    expect(b.received.size() == 1 && b.received[0].id == "m" && b.received[0].index == 1 &&
               b.received[0].source == a.win.get(),
           "let go over B: on_receive(m, 1, A) once" + at);
    expect(b.tabs->ids() == ids{"x", "m", "y"} && a.tabs->ids() == ids{"a", "c"} && b.drop_mark() == -1 &&
               a.detached.empty() && img && !IsWindow(img),
           "the app moved it; nothing marked, nothing detached, the image gone" + at + ": " + join(b.tabs->ids()));
    expect(timers() == 0, "no Escape timer left" + at);
    expect(c.received.empty(), "and C was asked nothing" + at);
    // Refused by B: both as they were.
    b.received.clear();
    b.accept = false;
    p = a.start(1);  // c
    a.move(a.client(over_b));
    a.up(a.client(over_b));
    expect(b.received.size() == 1 && b.tabs->ids() == ids{"x", "m", "y"} && a.tabs->ids() == ids{"a", "c"} &&
               a.detached.empty() && !image(),
           "B's on_receive says no: both rows as they were" + at);
    b.accept = true;
    b.received.clear();
    // Let go over C's row (another group): outside every row of the group,
    // so on_detach; refused here.
    a.refuse_detach = true;
    p = a.start(1);
    a.move(a.client(over_c));
    a.up(a.client(over_c));
    expect(c.received.empty() && a.detached.size() == 1 && a.tabs->ids() == ids{"a", "c"},
           "let go over another group's row: C asked nothing, on_detach asked" + at);
    a.refuse_detach = false;
    a.detached.clear();

    // A window's only tab: dragged, it's only the tab.
    const RECT d_was = d.window_rect();
    p = d.start(0);
    d.move(POINT{p.x, below});
    img = image();
    expect(img && image_styled(img) && same(d.window_rect(), d_was),
           "the only tab dragged out: the drag image, the window where it was" + at);
    d.up(POINT{p.x, below});
    expect(d.detached.empty() && d.tabs->ids() == ids{"solo"} && same(d.window_rect(), d_was) && !IsWindow(img) &&
               !ask(b.top, 5),
           "let go outside: nothing (no on_detach, the window not moved), the image gone" + at);
    // Back on its own row: nothing.
    p = d.start(0);
    d.move(POINT{p.x, below});
    d.move(p);
    d.up(p);
    expect(d.detached.empty() && d.reordered.empty() && d.received.empty() && d.tabs->ids() == ids{"solo"} &&
               same(d.window_rect(), d_was) && !image(),
           "let go on its own row: nothing" + at);
    // Onto B's row: moved there, the app closing the emptied window there
    // and then.
    b.close_emptied = true;
    const RECT b0 = b.tab(0);
    const POINT b_first = b.screen(POINT{b0.left + 4, (b0.top + b0.bottom) / 2});
    const HWND d_top = d.top;
    p = d.start(0);
    d.move(d.client(b_first));
    expect(b.drop_mark() == 0, "the only tab over B's first tab's left half: B marks 0" + at);
    img = image();
    d.up(d.client(b_first));
    expect(b.received.size() == 1 && b.received[0].id == "solo" && b.received[0].index == 0 && !d.win &&
               !IsWindow(d_top) && img && !IsWindow(img) && !image(),
           "let go over B: B takes solo, the app closing D in on_receive; the image gone" + at);
    expect(b.tabs->ids() == ids{"solo", "x", "m", "y"}, "B has it first" + at + ": " + join(b.tabs->ids()));
    b.close_emptied = false;
    b.received.clear();

    // A torn off window's tab, the source closed in on_receive likewise.
    a.made = nullptr;
    p = a.start(1);  // c
    a.move(POINT{p.x, below});
    a.up(POINT{p.x, below});
    if (!a.made) return expect(false, "torn off again" + at);
    fixture& t2 = *a.made;
    const HWND t2_top = t2.top;
    b.close_emptied = true;
    const rap::window* t2_win = t2.win.get();
    p = t2.start(0);
    t2.move(t2.client(b_first));
    t2.up(t2.client(b_first));
    expect(b.received.size() == 1 && b.received[0].id == "c" && b.received[0].source == t2_win && !t2.win &&
               !IsWindow(t2_top) && b.tabs->ids().front() == "c",
           "a torn-off window's only tab onto B: on_receive(c, 0, it), its window closed by the app" + at);
    b.close_emptied = false;

    // on_detach closing its own window there and then: the app's window put
    // and shown all the same.
    fixture& s = *make("S" + tag, {"s1", "s2"}, "g");
    place(s, 500, 400);
    s.close_on_detach = true;
    const HWND s_top = s.top;
    ask(b.top, 5);
    RECT s_strip;
    GetClientRect(s.strip, &s_strip);
    p = s.start(0);
    s.move(POINT{p.x, s_strip.bottom + 200});
    img = image();
    s.up(POINT{p.x, s_strip.bottom + 200});
    fixture* sm = s.made;
    expect(!s.win && !IsWindow(s_top) && sm && sm->tabs && sm->tabs->ids() == ids{"s1"},
           "on_detach closing its own window: closed, the app's window holds s1" + at);
    expect(sm && HWND(ask(b.top, 5)) == sm->top && !IsWindowVisible(sm->top) && img && !IsWindow(img),
           "and it's shown (noted), the image gone" + at);
    expect(timers() == 0, "no Escape timer left" + at);

    expect(loop_seen == 0 && same(a.window_rect(), a_was), "never the move loop, no window moved by a drag" + at);

    // Let go somewhere the last move wasn't (no move there first): where it's
    // let go decides, not where it last moved. In a group (R) and in none (E).
    fixture& r = *make("R" + tag, {"r1", "r2", "r3"}, "g");
    fixture& e = *make("E" + tag, {"e1", "e2", "e3"}, "");
    place(r, 900, 100);
    place(e, 1700, 100);
    stack({a.top, b.top, c.top, r.top, e.top});
    for (fixture* g : {&r, &e})
    {
        fixture& w = *g;
        const std::string in = std::string(g == &r ? " in a group" : " in no group") + at;
        RECT ws;
        GetClientRect(w.strip, &ws);
        w.refuse_detach = true;
        // Out, then let go over its own row: no detach; it lands there.
        POINT q = w.start(0);
        w.move(POINT{q.x, ws.bottom + 200});
        const bool was_out = image() != nullptr;
        w.up(w.centre(2));
        expect(was_out && w.detached.empty() && !image() && w.tabs->ids() == (g == &r ? ids{"r2", "r3", "r1"} : ids{"e2", "e3", "e1"}),
               "out, then let go over its own row: not torn off, put where it was let go" + in + ": " +
                   join(w.tabs->ids()));
        expect(w.reordered.size() == 1 && w.reordered[0].second == 2, "and on_reorder once" + in);
        w.reordered.clear();
        const ids before = w.tabs->ids();
        // Along the row, then let go outside: torn off (asked; refused here).
        q = w.start(0);
        w.move(w.centre(1));
        expect(!image(), "along the row: no image" + in);
        w.up(POINT{q.x, ws.bottom + 200});
        expect(w.detached.size() == 1 && w.detached[0].first == before[0] && w.tabs->ids() == before && !image(),
               "along the row, then let go outside: on_detach asked, the tab back where it was" + in + ": " +
                   join(w.tabs->ids()));
        expect(w.reordered.empty(), "and no on_reorder" + in);
        w.detached.clear();
        // Along the row, then let go over another window's row: in the group,
        // dropped there; in none, outside every row (on_detach).
        b.received.clear();
        b.accept = false;
        q = w.start(0);
        w.move(w.centre(1));
        w.up(w.client(over_b));
        if (g == &r)
            expect(b.received.size() == 1 && b.received[0].id == before[0] && w.detached.empty() &&
                       w.tabs->ids() == before,
                   "along the row, then let go over B's row: B's on_receive" + in);
        else
            expect(b.received.empty() && w.detached.size() == 1 && w.tabs->ids() == before,
                   "along the row, then let go over B's row, in no group: on_detach, B asked nothing" + in);
        expect(w.reordered.empty() && !image(), "and no on_reorder" + in);
        b.accept = true;
        b.received.clear();
        w.detached.clear();
        w.refuse_detach = false;
    }

    // The right-click menu: the fixed items, then the app's.
    tab_menu_capture cap{};
    cap.choose = -1;
    ask(b.top, 4, reinterpret_cast<LPARAM>(&cap));
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
    a.made = nullptr;
    ask(b.top, 5);
    cap = tab_menu_capture{};
    cap.choose = 3;
    SendMessageW(a.strip, WM_CONTEXTMENU, WPARAM(a.strip), MAKELPARAM(o_at.x, o_at.y));
    expect(a.detached.size() == 1 && a.detached[0].first == "a" && !a.detached[0].second,
           "Move to new window: on_detach(a) with no point" + at);
    // Shown, as the handler returns it unshown (here, never shown: noted).
    expect(a.made && HWND(ask(b.top, 5)) == a.made->top && !IsWindowVisible(a.made->top),
           "and the app's window shown (the tests' noted, not shown)" + at);
    // A window's only tab: the item disabled.
    cap = tab_menu_capture{};
    cap.choose = -1;
    if (a.made)
    {
        fixture& lone = *a.made;
        const POINT lone_at = lone.screen(lone.centre(0));
        SendMessageW(lone.strip, WM_CONTEXTMENU, WPARAM(lone.strip), MAKELPARAM(lone_at.x, lone_at.y));
        expect(cap.count > 3 && !cap.enabled[3], "the only tab: Move to new window disabled" + at);
    }
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
    // The source window destroyed mid-drag (by the app): the drag, its image
    // and its timer gone with it.
    {
        fixture& x = *make("X" + tag, {"x1", "x2"}, "g");
        place(x, 500, 900);
        RECT xs;
        GetClientRect(x.strip, &xs);
        const POINT xp = x.start(0);
        x.move(POINT{xp.x, xs.bottom + 200});
        const HWND ximg = image();
        expect(ximg && timers() == 1, "X's tab out: its image, its timer" + at);
        x.tabs = nullptr;
        x.win.reset();
        expect(!IsWindow(ximg) && !image() && timers() == 0 && b.drop_mark() == -1,
               "X closed mid-drag: no image, no timer, nothing marked" + at);
    }

    // Escape as a WM_KEYDOWN for another of this thread's windows (C's),
    // through the message loop: the drag cancelled all the same. The loop
    // runs once a process (rapport's quit ends it for good): in the last run.
    if (last)
    {
        RECT bs;
        GetClientRect(b.strip, &bs);
        b.detached.clear();
        p = b.start(0);
        const POINT b_out{p.x, bs.bottom + 200};
        b.move(b_out);
        img = image();
        const ids was = b.tabs->ids();
        PostMessageW(c.top, WM_KEYDOWN, VK_ESCAPE, 1);
        rap::subscription done = g_ui->timer(20, false, [] { g_ui->quit(0); });
        g_ui->run();
        expect(img && !IsWindow(img) && !image() && timers() == 0 && b.tabs->ids() == was,
               "Escape for C's window, through the message loop: B's drag cancelled" + at);
        expect(route() == 1 && HWND(ask(b.top, 9)) == c.top && ask(b.top, 10) == WM_KEYDOWN,
               "heard by the loop, as C's WM_KEYDOWN" + at + " (" + std::to_string(route()) + ")");
        b.up(b_out);
        expect(b.detached.empty() && b.received.empty(), "and nothing asked" + at);
    }

    ask(b.top, 4, 0);
    ask(b.top, 6, -1);
    ask(b.top, 0, 0);
    stack({});
    ask(b.top, 1, 0);
}

// -- --input: the real thing, CI only ----------------------------------------
//
// Shown windows, the real mouse and keys (SendInput), the desktop's z-order:
// A's tab dragged out and let go on the desktop: on_detach then, and the
// app's window shown with its tab where it was let go; one dragged onto B's
// row: B takes it; D's only tab dragged out and let go: nothing, D not
// moved; then onto B's row: B takes it and the app closes D; Escape with a
// tab out: back, nothing asked. Each step waits for what it's after (the
// pointer there, the drag's capture, the handler's call) with a bounded
// timeout, and a step that fails or times out ends the test then. Real input
// moves the pointer and presses keys on whatever desktop it runs on, so it
// runs only on a CI runner: GITHUB_ACTIONS=true and RAPPORT_TEST_SEND_INPUT=1.

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
    in.mi.dx = LONG((int64_t(screen.x - x0) * 65535 + (w - 1) / 2) / (w - 1));
    in.mi.dy = LONG((int64_t(screen.y - y0) * 65535 + (h - 1) / 2) / (h - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | flags;
    SendInput(1, &in, sizeof in);
}
// A key, with its scan code as well as its virtual key (some builds drop a
// keyboard INPUT whose wScan is 0); what was sent, and SendInput's answer,
// printed. True if it was taken.
bool send_key(WORD vk, bool up)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SetLastError(0);
    const UINT sent = SendInput(1, &in, sizeof(INPUT));
    const DWORD error = GetLastError();
    std::printf("     SendInput(key %s: wVk 0x%02X, wScan 0x%02X, dwFlags 0x%lX; cbSize %zu) = %u, error %lu\n",
                up ? "up" : "down", in.ki.wVk, in.ki.wScan, in.ki.dwFlags, sizeof(INPUT), sent, error);
    std::fflush(stdout);
    return sent == 1;
}

// As the backend's (key_log).
struct key_log
{
    int count;
    struct entry
    {
        HWND hwnd;
        UINT message;
        WPARAM key;
        LPARAM flags;
    } entries[64];
};

// The steps: each an action, then a condition it waits for (true at once if
// none), with a timeout; a step whose condition isn't met in time fails the
// test and ends it.
struct step
{
    std::string what;
    std::function<void()> act;
    std::function<bool()> done;
    int timeout_ms = 3000;
    // Timed out: true if that's been accounted for (a skip), not a failure.
    std::function<bool()> timed_out;
};
std::vector<step> steps;
std::function<void()> g_on_timeout;  // more to print, if the step waiting times out

bool pointer_at(POINT p)
{
    POINT c{};
    GetCursorPos(&c);
    return std::abs(c.x - p.x) <= 1 && std::abs(c.y - p.y) <= 1;
}

// A drag in steps: pressed at `from` on `f`'s strip (the drag's capture
// taken), through `via`, to `to`, each move waited for (the pointer there),
// and let go there (or not, `release` false).
void drag(fixture& f, POINT from, std::vector<POINT> via, POINT to, bool release = true)
{
    steps.push_back({"the pointer on the tab", [from] { send_mouse(from, 0); }, [from] { return pointer_at(from); }});
    steps.push_back({"pressed: the strip has the mouse", [from] { send_mouse(from, MOUSEEVENTF_LEFTDOWN); },
                     [&f] { return GetCapture() == f.strip; }});
    via.push_back(to);
    POINT last = from;
    for (POINT p : via)
    {
        for (int k = 1; k <= 6; ++k)
        {
            const POINT q{last.x + (p.x - last.x) * k / 6, last.y + (p.y - last.y) * k / 6};
            steps.push_back({"the pointer moved", [q] { send_mouse(q, 0); }, [q] { return pointer_at(q); }});
        }
        last = p;
    }
    if (release)
        steps.push_back({"let go: the drag over", [to] { send_mouse(to, MOUSEEVENTF_LEFTUP); },
                         [&f] { return !f.win || GetCapture() != f.strip; }});
}
void check(std::string what, std::function<bool()> ok, int timeout_ms = 3000)
{
    steps.push_back({std::move(what), [] {}, std::move(ok), timeout_ms});
}

// A window, by the test's names for them.
std::string window_name(HWND h)
{
    if (!h) return "none";
    for (auto& f : fixtures)
        if (f->top == h)
            return f->name;
        else if (f->strip == h)
            return f->name + "'s strip";
        else if (f->top && IsChild(f->top, h))
            return "a child of " + f->name;
    wchar_t cls[64]{};
    GetClassNameW(h, cls, 64);
    std::string s;
    for (wchar_t c : std::wstring(cls)) s += char(c < 128 ? c : '?');
    return "another window (" + s + ")";
}

// The Escape step's record.
key_log keys{};
bool escape_sent = false, async_seen = false, async_logged = false, escape_dropped = false;
ULONGLONG escape_at = 0;

int input_mode(rap::ui& ui)
{
    if (!env_is("GITHUB_ACTIONS", "true") || !env_is("RAPPORT_TEST_SEND_INPUT", "1"))
    {
        std::printf("skipped: real input (SendInput) only on a CI runner "
                    "(GITHUB_ACTIONS=true, RAPPORT_TEST_SEND_INPUT=1)\n");
        return 77;
    }
    g_menu_bar = windows_build() < 22000;  // the caption's strip where there is one
    // Small enough that all three fit on a runner's 1024 x 768 screen, and
    // a window let go has room where it's let go.
    g_size = SIZE{460, 200};
    fixture& a = *make("Input A", {"a", "b", "c", "e"}, "g");
    fixture& b = *make("Input B", {"x", "y"}, "g");
    fixture& d = *make("Input D", {"solo"}, "g");
    if (!a.strip || !b.strip || !d.strip) return expect(false, "the windows' strips"), 1;
    for (fixture* f : {&a, &b, &d}) f->win->show();
    const RECT work = work_area();
    place(a, work.left + 10, work.top + 10);
    RECT r0;
    GetWindowRect(a.top, &r0);
    place(d, r0.right + 20, work.top + 10);
    place(b, work.left + 10, r0.bottom + 40);
    SetForegroundWindow(a.top);
    pump();
    using ids = std::vector<std::string>;
    RECT ar, dr;
    GetWindowRect(a.top, &ar);
    GetWindowRect(d.top, &dr);
    const RECT bx = b.tab(0);
    const POINT over_b = b.screen(POINT{bx.right - 6, (bx.top + bx.bottom) / 2});
    // Let go where the new window (A's frame, its first tab as far in) fits
    // in the work area as it's put, its tab under the pointer: below A's
    // row, over A's content or the desktop, and not over B.
    const RECT a_frame = frame_of(a.top);
    const SIZE frame_size{a_frame.right - a_frame.left, a_frame.bottom - a_frame.top};
    const RECT a0 = a.tab(0);
    const POINT a0s = a.screen(POINT{a0.left, a0.top});
    const POINT off{a0s.x - a_frame.left, a0s.y - a_frame.top};
    const POINT from_b = a.screen(a.centre(1));
    const RECT b1 = a.tab(1);
    const POINT grab{a.centre(1).x - b1.left, a.centre(1).y - b1.top};
    const POINT out{work.left + 30 + off.x + grab.x, a0s.y + (a0.bottom - a0.top) + 60};
    const RECT room = frame_for(out, grab, off, frame_size);
    const bool fits = inside(room, work);
    std::printf("     work area %s, A's frame %s, B's %s; let go at %ld,%ld, the new frame %s%s\n",
                where(work).c_str(), where(a_frame).c_str(), where(frame_of(b.top)).c_str(), out.x, out.y,
                where(room).c_str(), fits ? "" : " (doesn't fit: its place not checked)");

    // A's b, dragged out and let go: on_detach then, the window shown there.
    drag(a, from_b, {POINT{from_b.x + 12, from_b.y}, POINT{from_b.x, out.y}}, out, false);
    check("dragged out: nothing asked before it's let go", [&] { return a.detached.empty(); }, 0);
    steps.push_back({"let go outside: on_detach(b) once", [out] { send_mouse(out, MOUSEEVENTF_LEFTUP); },
                     [&] { return a.detached.size() == 1 && a.made && a.made->win; }});
    check("its window shown with its tab where it was let go",
          [&]
          {
              if (!a.made || !IsWindowVisible(a.made->top)) return false;
              if (!fits) return true;
              RECT r = a.made->tab(0);
              MapWindowPoints(a.made->strip, nullptr, reinterpret_cast<POINT*>(&r), 2);
              return PtInRect(&r, out) != FALSE;
          });
    check("and b out of A",
          [&] { return a.tabs->ids() == ids{"a", "c", "e"} && a.made && a.made->tabs->ids() == ids{"b"}; });
    steps.push_back({"(the new window closed)", [&] {
                         if (a.made) a.made->win.reset();
                     },
                     nullptr});
    // A's c onto B's row: B takes it, second.
    const POINT from_c = a.screen(a.centre(1));
    drag(a, from_c, {POINT{from_c.x + 12, from_c.y}, POINT{from_c.x, ar.bottom + 20}}, over_b);
    check("dragged onto B's row and let go: on_receive(c, 1, A)",
          [&]
          {
              return b.received.size() == 1 && b.received[0].id == "c" && b.received[0].index == 1 &&
                     b.received[0].source == a.win.get();
          });
    check("and the app moved it", [&] { return b.tabs->ids() == ids{"x", "c", "y"} && a.tabs->ids() == ids{"a", "e"}; });
    // Escape with a tab out: back, nothing asked.
    const POINT from_e = a.screen(a.centre(1));
    drag(a, from_e, {POINT{from_e.x + 12, from_e.y}, POINT{from_e.x, out.y}}, out, false);
    steps.push_back({"Escape down, held",
                     [&]
                     {
                         // Where the keys go (Escape must cancel either way).
                         auto name = window_name;
                         GUITHREADINFO gi{sizeof gi};
                         GetGUIThreadInfo(0, &gi);
                         std::printf("     at Escape: foreground %s (its focus %s, active %s, capture %s); this "
                                     "thread's active %s, focus %s, capture %s; the source %s\n",
                                     name(GetForegroundWindow()).c_str(), name(gi.hwndFocus).c_str(),
                                     name(gi.hwndActive).c_str(), name(gi.hwndCapture).c_str(),
                                     name(GetActiveWindow()).c_str(), name(GetFocus()).c_str(),
                                     name(GetCapture()).c_str(), name(a.top).c_str());
                         g_on_timeout = [&, name]
                         {
                             std::printf("     at the timeout: foreground %s, this thread's focus %s, capture %s; "
                                         "Escape heard by route %d\n",
                                         name(GetForegroundWindow()).c_str(), name(GetFocus()).c_str(),
                                         name(GetCapture()).c_str(), int(ask(a.top, 8)));
                         };
                         ask(a.top, 8);
                         keys = key_log{};
                         ask(a.top, 11, reinterpret_cast<LPARAM>(&keys));
                         escape_sent = send_key(VK_ESCAPE, false);
                         escape_at = GetTickCount64();
                         async_logged = false;
                     },
                     // Held about 200 ms (the loop pumping meanwhile), its
                     // state looked at 100 ms in.
                     [&]
                     {
                         const ULONGLONG held = GetTickCount64() - escape_at;
                         if (held >= 100 && !async_logged)
                         {
                             async_logged = true;
                             const SHORT s = GetAsyncKeyState(VK_ESCAPE);
                             async_seen = (s & 0x8000) != 0;
                             std::printf("     100 ms into the hold: GetAsyncKeyState(VK_ESCAPE) 0x%04X\n",
                                         unsigned(USHORT(s)));
                         }
                         return held >= 200;
                     }});
    steps.push_back({"Escape up", [&] { escape_sent = send_key(VK_ESCAPE, true) && escape_sent; }, nullptr});
    steps.push_back({"Escape: the drag cancelled", nullptr, [&] { return GetCapture() != a.strip; }, 1500,
                     [&]
                     {
                         // Proven dropped: taken by SendInput, but seen
                         // nowhere, neither as a message nor in the key's state.
                         bool any_escape = false;
                         for (int k = 0; k < keys.count; ++k)
                             any_escape = any_escape || keys.entries[k].key == VK_ESCAPE;
                         if (g_on_timeout) g_on_timeout();
                         if (escape_sent && !any_escape && !async_seen && ask(a.top, 8) == 0)
                         {
                             std::printf("     skipped: Escape by real input: SendInput took it, but it never "
                                         "arrived (no key message on this thread, no async key state): the runner "
                                         "drops injected keys here. The hidden tests cover Escape's routes.\n");
                             escape_dropped = true;
                             return true;
                         }
                         return false;
                     }});
    steps.push_back({"(how Escape was heard)", [&]
                     {
                         g_on_timeout = nullptr;
                         ask(a.top, 11, 0);
                         const int how = int(ask(a.top, 8));
                         static const char* routes[] = {"none",
                                                        "the message loop",
                                                        "the strip's WM_KEYDOWN",
                                                        "the key's state, on a move",
                                                        "the key's state, on the timer",
                                                        "the key's state, as it was let go"};
                         HWND h = HWND(ask(a.top, 9));
                         std::printf("     Escape heard by %s%s\n", how >= 0 && how <= 5 ? routes[how] : "?",
                                     how == 1 ? (" (WM_KEYDOWN for " + window_name(h) + ")").c_str() : "");
                         std::printf("     keys the loop took during the hold: %d\n", keys.count);
                         for (int k = 0; k < keys.count; ++k)
                         {
                             const auto& e = keys.entries[k];
                             const char* m = e.message == WM_KEYDOWN      ? "WM_KEYDOWN"
                                             : e.message == WM_KEYUP      ? "WM_KEYUP"
                                             : e.message == WM_SYSKEYDOWN ? "WM_SYSKEYDOWN"
                                                                          : "WM_SYSKEYUP";
                             std::printf("       %s vk 0x%02X lParam 0x%08llX for %s\n", m, unsigned(e.key),
                                         (unsigned long long)e.flags, window_name(e.hwnd).c_str());
                         }
                     },
                     nullptr});
    steps.push_back({"and let go",
                     [&, out, from_e]
                     {
                         if (escape_dropped)  // still dragged: back over its row first, so it's not torn off
                         {
                             send_mouse(from_e, 0);
                             send_mouse(from_e, MOUSEEVENTF_LEFTUP);
                         }
                         else
                             send_mouse(out, MOUSEEVENTF_LEFTUP);
                     },
                     [] { return GetAsyncKeyState(VK_LBUTTON) >= 0; }});
    check("Escape: nothing asked, A as it was",
          [&] { return a.detached.size() == 1 && a.tabs->ids() == ids{"a", "e"} && b.received.size() == 1; });
    // D's only tab out and let go: nothing, D where it was.
    const POINT from_d = d.screen(d.centre(0));
    const POINT out_d{from_d.x, dr.bottom + 60};
    drag(d, from_d, {POINT{from_d.x + 12, from_d.y}, POINT{from_d.x, dr.bottom + 20}}, out_d);
    check("the only tab dragged out and let go: nothing, D not moved",
          [&]
          {
              RECT now;
              GetWindowRect(d.top, &now);
              return d.detached.empty() && d.tabs->ids() == ids{"solo"} && EqualRect(&now, &dr);
          });
    // Then onto B's row: B takes it, and the app closes D.
    steps.push_back({"(B closes what it empties)", [&] { b.close_emptied = true; }, nullptr});
    drag(d, from_d, {POINT{from_d.x + 12, from_d.y}, POINT{from_d.x, dr.bottom + 20}}, over_b);
    check("the only tab onto B's row: B takes it, D closed",
          [&]
          {
              const std::vector<std::string> now = b.tabs->ids();
              return b.received.size() == 2 && b.received[1].id == "solo" && !d.win &&
                     std::find(now.begin(), now.end(), "solo") != now.end();
          });

    size_t at = 0;
    bool acted = false;
    auto since = std::chrono::steady_clock::now();
    const auto started = since;
    int code = 0;
    rap::subscription tick = ui.timer(
        30, true,
        [&]
        {
            // The whole run's bound, past any one step's.
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(60))
            {
                expect(false, "the run finished within 60 s");
                code = 1;
                return ui.quit(1);
            }
            if (at >= steps.size()) return ui.quit(0);
            step& s = steps[at];
            if (!acted)
            {
                acted = true;
                since = std::chrono::steady_clock::now();
                if (s.act) s.act();
            }
            if (!s.done || s.done())
            {
                if (s.done) expect(true, s.what);
                ++at;
                acted = false;
                return;
            }
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since);
            if (waited.count() >= s.timeout_ms)
            {
                if (s.timed_out && s.timed_out())
                {
                    ++at;
                    acted = false;
                    return;
                }
                expect(false, s.what + " (timed out after " + std::to_string(waited.count()) + " ms)");
                if (g_on_timeout) g_on_timeout();
                code = 1;
                ui.quit(1);
            }
        });
    ui.run();
    send_mouse(POINT{0, 0}, MOUSEEVENTF_LEFTUP);
    send_key(VK_ESCAPE, true);
    fixtures.clear();
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails || code ? 1 : 0;
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
    HHOOK sent = SetWindowsHookExW(WH_CALLWNDPROC, watch_sent, nullptr, GetCurrentThreadId());
    HHOOK posted = SetWindowsHookExW(WH_GETMESSAGE, watch_posted, nullptr, GetCurrentThreadId());
    g_menu_bar = true;
    run("stage 1, under a menu bar", windows_build() < 22000);
    if (windows_build() >= 22000)
    {
        g_menu_bar = false;
        run("in the caption", true);
    }
    else
        std::printf("     no caption strip before Windows 11: stage 1 only\n");
    pump();
    set_keys({});
    for (auto& f : fixtures)
        expect(!f->win || (f->top && !IsWindowVisible(f->top)), "never shown, to the end: " + f->name);
    if (sent) UnhookWindowsHookEx(sent);
    if (posted) UnhookWindowsHookEx(posted);
    fixtures.clear();
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
