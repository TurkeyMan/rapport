// A window's document tabs reordered by dragging and by the keys, on
// Windows: stage 1's tab control (under a menu bar, as before Windows 11),
// and Windows 11's strip in the caption where there is one. The windows are
// never shown: the strip's HWND is sent the mouse's messages (pressed on a
// tab, moved past the drag threshold, let go) and the keys, nothing else.
//
// Each: dragged one place right, the others make room as it goes (the
// bookkeeping, ids(), and the strip's own order mid-drag), on_reorder once
// as it's let go, with the tab's id and new index; a press and release
// within the threshold, a click, moves nothing; Escape mid-drag puts the tab
// back and fires nothing; past the strip's end it stays at the end; dragged
// back where it was, nothing's told; Ctrl+Shift+PgDn and Ctrl+Shift+PgUp
// move the current tab, on_reorder once each, and at the row's end do
// nothing. In the caption, a tab dragged is the strip's to hit (HTCLIENT)
// wherever the pointer goes.
//
// UI Automation's order (ChildrenReordered, the TabItems following) needs a
// shown window: rapport-win32-caption-tabs-test checks it, on CI.
//   rapport-win32-document-tabs-reorder-test
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <commctrl.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
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

HWND top_of(const wchar_t* title)
{
    // Never shown: FindWindow finds hidden windows too.
    return FindWindowW(nullptr, title);
}

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

// The strip, either kind: where its tabs are, and the mouse and keys sent.
class strip
{
public:
    strip(HWND h, bool caption)
        : h_(h)
        , caption_(caption)
    {
    }
    RECT tab(int i) const
    {
        RECT r{};
        if (caption_)
        {
            static const UINT m = RegisterWindowMessageW(L"rapport-caption-tabs");
            SendMessageW(h_, m, MAKEWPARAM(3, i), reinterpret_cast<LPARAM>(&r));
        }
        else
            TabCtrl_GetItemRect(h_, i, &r);
        return r;
    }
    int current() const
    {
        static const UINT m = RegisterWindowMessageW(L"rapport-caption-tabs");
        return caption_ ? int(SendMessageW(h_, m, MAKEWPARAM(1, 0), 0)) : TabCtrl_GetCurSel(h_);
    }
    POINT centre(int i) const
    {
        const RECT r = tab(i);
        return {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    }
    void down(POINT p) { SendMessageW(h_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(p.x, p.y)); }
    void move(POINT p) { SendMessageW(h_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(short(p.x), short(p.y))); }
    void up(POINT p) { SendMessageW(h_, WM_LBUTTONUP, 0, MAKELPARAM(short(p.x), short(p.y))); }
    void escape() { SendMessageW(h_, WM_KEYDOWN, VK_ESCAPE, 1); }
    LRESULT hit(POINT p) const
    {
        ClientToScreen(h_, &p);
        return SendMessageW(h_, WM_NCHITTEST, 0, MAKELPARAM(short(p.x), short(p.y)));
    }
    HWND hwnd() const { return h_; }
    bool caption() const { return caption_; }

private:
    HWND h_;
    bool caption_;
};

// The thread's key state with exactly these modifiers down: every other
// modifier (Alt, the Windows key, the right-hand ones) and the mouse buttons
// cleared, so nothing left over from before can change what a posted key is.
void set_modifiers(bool ctrl, bool shift)
{
    BYTE state[256] = {};
    GetKeyboardState(state);
    for (int vk : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU,
                   VK_LWIN, VK_RWIN, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON})
        state[vk] = 0;
    if (ctrl) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
    if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
    SetKeyboardState(state);
}

// Ctrl and Shift held in the thread's key state (and nothing else), the key
// posted to the window, as the backend's loop takes typing; then let go.
void ctrl_shift_key(HWND top, WPARAM vk)
{
    set_modifiers(true, true);
    const LPARAM lp = 1 | (LPARAM(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC)) << 16) | (LPARAM(1) << 24);
    PostMessageW(top, WM_KEYDOWN, vk, lp);
    PostMessageW(top, WM_KEYUP, vk, lp | (LPARAM(3) << 30));
}
void keys_up()
{
    set_modifiers(false, false);
}

struct told
{
    std::string id;
    int index = -1;
};

// The keys are the backend's loop's to take: steps run from a timer, one a
// tick, after the mouse's checks.
std::vector<std::function<void()>> steps;

// One window's: its tabs, what they told, and its strip.
struct fixture
{
    std::unique_ptr<rap::window> win;
    std::vector<told> reordered;
    std::vector<std::string> selected;
    rap::document_tabs* tabs = nullptr;
    HWND top = nullptr;
};
std::vector<std::unique_ptr<fixture>> fixtures;

// The checks, on one window's strip.
void reorder(rap::ui& ui, const char* name, bool with_menu)
{
    fixtures.push_back(std::make_unique<fixture>());
    fixture& f = *fixtures.back();
    std::vector<told>& reordered = f.reordered;
    std::vector<std::string>& selected = f.selected;
    rap::document_tabs*& tabs = f.tabs;
    using namespace rap::layout;
    const std::wstring title = with_menu ? L"Reorder, menu bar" : L"Reorder";
    window w{with_menu ? "Reorder, menu bar" : "Reorder", {label{"Content"}}};
    w.size(1000_px, 300_px);
    if (with_menu) w.menu_bar({{"_File", [] { return rap::menu{}; }}});
    w.tabs(document_tabs{{{"a", "Alpha", {}, {}}, {"b", "Beta", {}, {}}, {"c", "Gamma", {}, {}}}}
               .current("a")
               .on_select([&](const std::string& id) { selected.push_back(id); })
               .on_reorder([&](const std::string& id, int index) { reordered.push_back({id, index}); })
               .bind(tabs));
    f.win = ui.create(std::move(w));  // never shown
    pump();
    HWND top = f.top = top_of(title.c_str());
    const std::string at = std::string(" (") + name + ")";
    expect(top && !IsWindowVisible(top), "the window, never shown" + at);
    if (!top || !tabs) return;
    HWND caption = strip_of(top), control = tab_control_of(top);
    expect(with_menu ? (control && !caption) : (caption || control), "its strip" + at);
    strip s(caption ? caption : control, caption != nullptr);
    if (!s.hwnd()) return;
    const UINT dpi = GetDpiForWindow(top);
    const int threshold = GetSystemMetricsForDpi(SM_CXDRAG, dpi);
    using ids = std::vector<std::string>;

    // A click within the threshold: current, nothing moved.
    POINT a = s.centre(0);
    s.down(a);
    s.move(POINT{a.x + threshold, a.y});
    s.up(POINT{a.x + threshold, a.y});
    expect(reordered.empty() && tabs->ids() == ids{"a", "b", "c"}, "a click moves nothing" + at);

    // Alpha dragged one place right, a step at a time.
    const POINT b = s.centre(1);
    s.down(a);
    s.move(POINT{a.x + threshold + 1, a.y});
    expect(tabs->ids() == ids{"a", "b", "c"} && reordered.empty(),
           "just past the threshold: dragged, still first" + at + ": " + join(tabs->ids()));
    if (s.caption())
        expect(s.hit(POINT{a.x + threshold + 1, 1}) == HTCLIENT,
               "dragged, the strip's to hit even along its top (HTCLIENT)" + at);
    s.move(b);
    expect(tabs->ids() == ids{"b", "a", "c"},
           "over Beta: Beta's made room, live (ids " + join(tabs->ids()) + ")" + at);
    expect(s.current() == 1 && tabs->current() == "a", "and Alpha's still current, shown second" + at);
    if (s.caption())
    {
        const RECT r0 = s.tab(0), r1 = s.tab(1);
        expect(r0.left < r1.left && std::abs((r1.left + r1.right) / 2 - b.x) <= 1,
               "the strip shows Beta first, Alpha under the pointer" + at + ": " + std::to_string(r0.left) + ", " +
                   std::to_string(r1.left) + ".." + std::to_string(r1.right));
    }
    expect(reordered.empty(), "nothing told mid-drag" + at);
    s.up(b);
    expect(reordered.size() == 1 && reordered[0].id == "a" && reordered[0].index == 1,
           "let go: on_reorder(a, 1) once" + at + " (" + std::to_string(reordered.size()) + ")");
    expect(tabs->ids() == ids{"b", "a", "c"} && tabs->current() == "a" && s.current() == 1,
           "the bind and the strip agree: b, a, c, Alpha current" + at);
    if (s.caption()) expect(s.hit(POINT{s.centre(1).x, 1}) != HTCLIENT, "let go, the strip's top the window's again" + at);

    // Escape mid-drag: back where it was, nothing told.
    reordered.clear();
    POINT from = s.centre(1);
    s.down(from);
    s.move(POINT{from.x + threshold + 1, from.y});
    s.move(s.centre(2));
    expect(tabs->ids() == ids{"b", "c", "a"}, "dragged to the end, live" + at + ": " + join(tabs->ids()));
    s.escape();
    expect(tabs->ids() == ids{"b", "a", "c"} && s.current() == 1, "Escape: back where it was" + at + ": " +
                                                                       join(tabs->ids()));
    s.move(s.centre(2));
    s.up(s.centre(2));
    expect(reordered.empty() && tabs->ids() == ids{"b", "a", "c"}, "and nothing told, nothing moved after" + at);

    // Past the strip's end: at the end, and no further.
    from = s.centre(1);
    s.down(from);
    s.move(POINT{from.x + threshold + 1, from.y});
    RECT client;
    GetClientRect(s.hwnd(), &client);
    s.move(POINT{client.right + 400, from.y + 200});
    expect(tabs->ids() == ids{"b", "c", "a"}, "dragged out past the end: last" + at + ": " + join(tabs->ids()));
    s.move(POINT{-400, from.y});
    expect(tabs->ids() == ids{"a", "b", "c"}, "and past the start: first" + at + ": " + join(tabs->ids()));
    // Back where it started, let go: nothing told.
    s.move(from);
    s.up(from);
    expect(reordered.empty() && tabs->ids() == ids{"b", "a", "c"},
           "back where it started, let go: nothing told" + at + ": " + join(tabs->ids()));

    // The app's own changes mid-drag: the drag's put back first.
    from = s.centre(1);
    s.down(from);
    s.move(POINT{from.x + threshold + 1, from.y});
    s.move(s.centre(0));
    expect(tabs->ids() == ids{"a", "b", "c"}, "Alpha dragged first again, live" + at + ": " + join(tabs->ids()));
    tabs->add({"d", "Delta", {}, {}});
    expect(tabs->ids() == ids{"b", "a", "c", "d"}, "the app adds a tab mid-drag: the drag's undone first" + at +
                                                       ": " + join(tabs->ids()));
    s.move(s.centre(2));
    s.up(s.centre(2));
    expect(reordered.empty() && tabs->ids() == ids{"b", "a", "c", "d"}, "and the rest of it does nothing" + at);
    tabs->remove("d");

    // The keys, through the backend's loop: the current tab (Alpha,
    // second) right, then at the end right again, then left twice.
    selected.clear();
    auto press = [top](WPARAM vk) { return [top, vk] { ctrl_shift_key(top, vk); }; };
    steps.push_back(press(VK_NEXT));
    steps.push_back(
        [&f, s, at, press, top]
        {
            keys_up();
            expect(f.tabs->ids() == ids{"b", "c", "a"} && f.reordered.size() == 1 && f.reordered.back().id == "a" &&
                       f.reordered.back().index == 2 && s.current() == 2,
                   "Ctrl+Shift+PgDn: Alpha last, on_reorder(a, 2) once" + at + ": " + join(f.tabs->ids()));
            ctrl_shift_key(top, VK_NEXT);
        });
    steps.push_back(
        [&f, at, top]
        {
            keys_up();
            expect(f.reordered.size() == 1 && f.tabs->ids() == ids{"b", "c", "a"}, "at the end, again: nothing" + at);
            ctrl_shift_key(top, VK_PRIOR);
        });
    steps.push_back(press(VK_PRIOR));
    steps.push_back(
        [&f, s, at]
        {
            keys_up();
            expect(f.tabs->ids() == ids{"a", "b", "c"} && f.reordered.size() == 3 && f.reordered[2].index == 0 &&
                       s.current() == 0 && f.tabs->current() == "a",
                   "Ctrl+Shift+PgUp twice: first again, on_reorder once each" + at + ": " + join(f.tabs->ids()));
            expect(f.selected.empty(), "and no on_select from any of it" + at);

            // Between a press and its release (no move), the app changes
            // the tabs: the press's drag is cancelled, its release tells
            // nothing.
            auto& r = f.reordered;
            auto& t = *f.tabs;
            strip st = s;
            r.clear();
            POINT p = st.centre(1);  // Beta
            st.down(p);
            t.insert(0, {"y", "Inserted", {}, {}});
            st.up(p);
            expect(r.empty() && t.ids() == ids{"y", "a", "b", "c"},
                   "a tab added before the pressed one, then let go: no on_reorder" + at);
            t.remove("y");
            p = st.centre(1);
            st.down(p);
            t.remove("b");
            st.up(p);
            expect(r.empty() && t.ids() == ids{"a", "c"}, "the pressed tab removed, then let go: no on_reorder" + at);
            t.insert(1, {"b", "Beta", {}, {}});
            p = st.centre(1);
            st.down(p);
            t.remove("b");
            t.insert(2, {"b", "Beta", {}, {}});
            st.up(p);
            expect(r.empty() && t.ids() == ids{"a", "c", "b"},
                   "removed and added again elsewhere, then let go: no on_reorder" + at);
            t.remove("b");
            t.insert(1, {"b", "Beta", {}, {}});
            // A key's move while pressed: the press's drag cancelled, the
            // key's on_reorder once (the key's taken by the loop, next).
            st.down(st.centre(1));
            t.select("b");
            ctrl_shift_key(f.top, VK_NEXT);
        });
    steps.push_back(
        [&f, s, at]
        {
            keys_up();
            strip st = s;
            expect(f.reordered.size() == 1 && f.reordered[0].id == "b" && f.reordered[0].index == 2 &&
                       f.tabs->ids() == ids{"a", "c", "b"},
                   "Ctrl+Shift+PgDn while pressed: the key's on_reorder, once" + at + ": " + join(f.tabs->ids()));
            st.up(st.centre(1));
            expect(f.reordered.size() == 1 && f.tabs->ids() == ids{"a", "c", "b"}, "and let go: nothing more" + at);
        });
}

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options options{.backend = "win32"};
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 2;
    std::printf("build %lu\n", windows_build());
    reorder(**ui, "stage 1, under a menu bar", true);
    if (windows_build() >= 22000)
        reorder(**ui, "in the caption", false);
    else
        std::printf("     no caption strip before Windows 11: stage 1 only\n");
    size_t step = 0;
    rap::subscription tick = (*ui)->timer(30, true,
                                          [&]
                                          {
                                              if (step < steps.size())
                                              {
                                                  steps[step++]();
                                                  return;
                                              }
                                              (*ui)->quit(0);
                                          });
    (*ui)->run();
    keys_up();
    for (auto& f : fixtures)
        expect(f->win && f->top && !IsWindowVisible(f->top), "never shown, to the end");
    fixtures.clear();
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
