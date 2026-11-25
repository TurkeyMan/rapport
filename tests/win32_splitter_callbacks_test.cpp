// A splitter as its panes' callbacks and its own see it (#90). Hidden until
// the step that needs it shown (Win32 lays out a hidden window as a shown
// one), then shown without activation (RAPPORT_NO_ACTIVATE); the divider's
// bar is sent its mouse messages directly (no input injected).
//  1. A pane told its size (a canvas's on_resize) reads sizes() as it's
//     being laid out, not as the layout before: after set_sizes, after the
//     window resizes, and, shown, through window::resize, which lays out
//     ahead of the resize with the canvas told its size inside the
//     splitter's layout (the case that read the old sizes).
//  2. on_moved fires for a drag of the bar, and not for a press and release
//     with set_sizes between them and no drag.
//  3. A canvas's size() right after set_sizes, or a pane hidden, is the
//     size it's laid out at (it lays out a layout pending, with what's
//     pending), and the layout it's left with is that one. Read from inside
//     a layout (its on_resize, with another layout asked for there), it
//     doesn't lay out again within it.
//  4. A drag still on as the window goes reports nothing. (Windows sends
//     the bar no WM_CAPTURECHANGED as it's destroyed, so this held before
//     the splitter's teardown let go of the drag itself, too.)
#include <rapport/rapport.h>
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <cmath>
#include <cwchar>
#include <cstdio>
#include <string>
#include <vector>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
extern "C" void* rapport_win32_trace_splitter_bar(rap::splitter*, unsigned);
extern "C" unsigned rapport_win32_layout_depth_max(int reset);
extern "C" unsigned rapport_win32_layout_passes();
extern "C" void rapport_win32_set_resize_quiet_hook(void (*)(void*), void*);
#endif
// The backend's layout seams (exported): linked in a static build, looked up
// in rapport_win32.dll in a plugin one (the asan job's), once it's loaded.
unsigned (*g_depth_max)(int) = nullptr;
unsigned (*g_passes)() = nullptr;
void (*g_set_resize_quiet_hook)(void (*)(void*), void*) = nullptr;
bool layouts_seen = false;
void find_layout_seams()
{
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    g_depth_max = rapport_win32_layout_depth_max;
    g_passes = rapport_win32_layout_passes;
    g_set_resize_quiet_hook = rapport_win32_set_resize_quiet_hook;
#else
    if (HMODULE m = GetModuleHandleW(L"rapport_win32.dll"))
    {
        g_depth_max = reinterpret_cast<unsigned (*)(int)>(GetProcAddress(m, "rapport_win32_layout_depth_max"));
        g_passes = reinterpret_cast<unsigned (*)()>(GetProcAddress(m, "rapport_win32_layout_passes"));
        g_set_resize_quiet_hook = reinterpret_cast<void (*)(void (*)(void*), void*)>(
            GetProcAddress(m, "rapport_win32_set_resize_quiet_hook"));
    }
#endif
    layouts_seen = g_depth_max && g_passes && g_set_resize_quiet_hook;
}
unsigned depth_max(bool reset = false) { return g_depth_max ? g_depth_max(reset ? 1 : 0) : 0; }
unsigned passes() { return g_passes ? g_passes() : 0; }

namespace
{
int failures = 0;
void expect(bool ok, const std::string& what)
{
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

void pump()
{
    for (int round = 0; round < 10; ++round)
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

std::string num(float v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

bool close_to(float got, float want, float scale)
{
    return std::fabs(got - want) <= 1.5f / scale;
}

int client_width(HWND root, int w)
{
    RECT c, r;
    GetClientRect(root, &c);
    GetWindowRect(root, &r);
    const int extra = (r.right - r.left) - c.right;
    SetWindowPos(root, nullptr, 0, 0, w + extra, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    pump();
    GetClientRect(root, &c);
    return c.right;
}

// A scroll pane's host that resizes its window as it's moved, once armed.
struct hook_data
{
    rap::window* win;
    bool armed = false;
    int fired = 0;
    std::function<void()> instead;  // done in place of the resize, if set
};
LRESULT CALLBACK resize_on_move(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
{
    auto* d = reinterpret_cast<hook_data*>(ref);
    if (msg == WM_WINDOWPOSCHANGED && d->armed)
    {
        d->armed = false;
        ++d->fired;
        if (d->instead)
            d->instead();
        else
        {
            const rap::size_f now = d->win->size();
            d->win->resize({now.w + 105, now.h});
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void press(HWND bar)
{
    SendMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2));
}
void drag_to(HWND bar, int dx)
{
    SendMessageW(bar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(short(2 + dx), 2));
}
void release(HWND bar, int dx = 0)
{
    SendMessageW(bar, WM_LBUTTONUP, 0, MAKELPARAM(short(2 + dx), 2));
}
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // every line out as it's printed, however the process ends
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui) return 1;
    using lookup = void* (*)(rap::splitter*, unsigned);
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    lookup bar_of = rapport_win32_trace_splitter_bar;
#else
    HMODULE module = GetModuleHandleW(L"rapport_win32.dll");
    lookup bar_of =
        module ? reinterpret_cast<lookup>(GetProcAddress(module, "rapport_win32_trace_splitter_bar")) : nullptr;
#endif
    if (!bar_of) return 1;
    find_layout_seams();
    expect(layouts_seen, "the backend's layout seams found (linked in, or exported by rapport_win32.dll)");

    using namespace rap::layout;
    rap::splitter* split = nullptr;
    rap::canvas* view = nullptr;
    rap::label* other = nullptr;
    int moved = 0;
    // What the canvas's on_resize saw: its size, and the splitter's sizes()
    // then; how deep in on_resize calls it was.
    float told = -1, seen = -1;
    int depth = 0, deepest = 0;
    // Every on_resize whose sizes()[0] isn't the size it's told, after the
    // window's first layout.
    int calls = 0, stale = 0;
    bool counting = false;
    float scale_now = 1;
    std::function<void()> in_resize;  // once, from inside an on_resize
    // A second splitter of two canvases: the first's on_resize may ask for
    // new sizes (once), the second's reads sizes()[1] against what it's told.
    rap::splitter* pair = nullptr;
    std::function<void()> first_asks;
    float second_told = -1, second_seen = -1;
    bool asked = false, second_after_ask = false;
    auto win = (*ui)->create(window{
        "Splitter callbacks",
        {vbox{
            splitter{rap::orientation::horizontal,
                     canvas{}
                         .on_resize(
                             [&](rap::size_f s)
                             {
                                 ++depth;
                                 deepest = std::max(deepest, depth);
                                 told = s.w;
                                 if (split && !split->sizes().empty()) seen = split->sizes()[0];
                                 if (counting)
                                 {
                                     ++calls;
                                     if (!close_to(seen, told, scale_now)) ++stale;
                                 }
                                 if (auto f = std::exchange(in_resize, nullptr)) f();
                                 --depth;
                             })
                         .bind(view)
                         .stretch(),
                     label{"Other"}.bind(other)}
                .bind(split)
                .stretch(),
            splitter{rap::orientation::horizontal,
                     canvas{}
                         .on_resize(
                             [&](rap::size_f)
                             {
                                 if (auto f = std::exchange(first_asks, nullptr)) f();
                             })
                         .stretch(),
                     canvas{}.on_resize(
                         [&](rap::size_f s)
                         {
                             second_told = s.w;
                             second_after_ask = asked;
                             if (pair && pair->sizes().size() == 2) second_seen = pair->sizes()[1];
                         })}
                .bind(pair)
                .stretch(),
        }}});
    HWND root = FindWindowW(nullptr, L"Splitter callbacks");
    if (!root || !split || !view || !other || !pair) return 2;
    split->on_moved([&] { ++moved; });
    expect(!IsWindowVisible(root), "the test window is never shown");
    const float scale = float(GetDpiForWindow(root)) / 96.f;
    scale_now = scale;
    client_width(root, int(std::lround(600 * scale)));
    counting = true;

    // 1. sizes() inside the pane's on_resize: as laid out now.
    told = seen = -1;
    split->set_sizes({0, 200});
    pump();
    expect(told > 0 && close_to(seen, told, scale),
           "after set_sizes, on_resize's sizes()[0] is the size it's told: " + num(seen) + ", told " + num(told));
    told = seen = -1;
    client_width(root, int(std::lround(700 * scale)));
    expect(told > 0 && close_to(seen, told, scale),
           "after a resize, on_resize's sizes()[0] is the size it's told: " + num(seen) + ", told " + num(told));
    expect(calls > 0 && stale == 0, "every on_resize's sizes()[0] the size it's told: " + std::to_string(stale) +
                                        " of " + std::to_string(calls) + " not");
    counting = false;

    // 3. size() right after set_sizes: the size it's laid out at, and the
    // layout stays that.
    split->set_sizes({0, 260});
    const float read = view->size().w;
    pump();
    const auto after = split->sizes();
    expect(after.size() == 2 && close_to(after[1], 260, scale), "laid out at the size asked for: " +
                                                                    (after.size() == 2 ? num(after[1]) : "none"));
    expect(after.size() == 2 && close_to(read, after[0], scale),
           "size() right after set_sizes is the new size: " + num(read) + ", laid out " +
               (after.size() == 2 ? num(after[0]) : "none"));
    expect(close_to(view->size().w, read, scale) && close_to(told, read, scale),
           "and the layout it's left with is that one: " + num(view->size().w) + ", told " + num(told));
    // A pane hidden, then size(): the canvas takes the whole splitter.
    const float whole = after.size() == 2 ? after[0] + after[1] + split->divider() : 0;
    other->set_visible(false);
    const float alone = view->size().w;
    pump();
    expect(close_to(alone, whole, scale) && close_to(view->size().w, alone, scale),
           "size() right after its neighbour is hidden is the whole splitter: " + num(alone) + " of " + num(whole));
    other->set_visible(true);
    pump();
    // Read from inside a layout, with another asked for there: no layout
    // within it.
    deepest = 0;
    in_resize = [&]
    {
        split->set_sizes({0, 180});  // a layout asked for
        view->size();
    };
    split->set_sizes({0, 220});
    pump();
    pump();
    expect(deepest == 1, "size() inside on_resize lays nothing out within that layout (on_resize " +
                             std::to_string(deepest) + " deep)");
    {
        const auto s = split->sizes();
        expect(s.size() == 2 && close_to(s[1], 180, scale),
               "the layout asked for inside it comes after: " + (s.size() == 2 ? num(s[1]) : "none"));
    }

    // 1, shown: window::resize lays a shown window out ahead of its resize,
    // the canvas told its new size from inside the splitter's layout.
    win->show();
    pump();
    calls = stale = 0;
    counting = true;
    win->resize({760, 300});
    pump();
    win->resize({620, 300});
    pump();
    counting = false;
    expect(calls > 0 && stale == 0, "shown, resized: every on_resize's sizes()[0] the size it's told: " +
                                        std::to_string(stale) + " of " + std::to_string(calls) + " not");

    // 2. on_moved: a drag, and a press and release with set_sizes between.
    HWND bar = static_cast<HWND>(bar_of(split, 0));
    if (!bar)
    {
        expect(false, "the bar found");
        return 1;
    }
    moved = 0;
    press(bar);
    drag_to(bar, -40);
    release(bar, -40);
    expect(moved == 1, "a drag of the bar: on_moved once (" + std::to_string(moved) + ")");
    moved = 0;
    press(bar);
    split->set_sizes({0, 240});
    pump();
    release(bar);
    expect(moved == 0, "a press, set_sizes, a release: no on_moved (" + std::to_string(moved) + ")");

    // 3, a drag: an on_resize from the drag's layout asks for new sizes and
    // reads size(): nothing laid out within the drag's, the sizes asked for
    // laid out after.
    deepest = 0;
    in_resize = [&]
    {
        split->set_sizes({0, 200});
        view->size();
    };
    press(bar);
    drag_to(bar, -25);
    release(bar, -25);
    pump();
    expect(deepest == 1, "a drag's on_resize asking for sizes and reading size(): nothing laid out within the "
                         "drag's (on_resize " + std::to_string(deepest) + " deep)");
    {
        const auto s = split->sizes();
        expect(s.size() == 2 && close_to(s[1], 200, scale),
               "the sizes it asked for, laid out after: " + (s.size() == 2 ? num(s[1]) : "none"));
    }

    // 1, a neighbour: during a drag of the first splitter, its pane's
    // on_resize sets the other splitter's sizes, in the same window, and
    // reads them back: what was asked for (that splitter isn't being placed
    // by the drag), not its last placement.
    {
        pump();
        float neighbour_seen = -1;
        in_resize = [&]
        {
            pair->set_sizes({0, 210});
            const auto s = pair->sizes();
            neighbour_seen = s.size() == 2 ? s[1] : -1;
        };
        press(bar);
        drag_to(bar, -20);
        release(bar, -20);
        in_resize = nullptr;
        expect(close_to(neighbour_seen, 210, scale),
               "during a drag of one splitter, another in the window reports the sizes asked of it: " +
                   num(neighbour_seen));
        pump();
    }

    // 1, a nested layout: during the drag's delivery, the window resized
    // (laying everything out again, at once, within it); then the other
    // splitter asked for new sizes and read back: what was asked for, its
    // nested placement over. And the dragged one reports its latest
    // placement, the nested one's.
    {
        pump();
        float neighbour_seen = -1, dragged_seen = -1, dragged_told = -1;
        int resized = 0;
        in_resize = [&]
        {
            const rap::size_f now = win->size();
            win->resize({now.w + 40, now.h});
            ++resized;
            pair->set_sizes({0, 255});
            const auto s = pair->sizes();
            neighbour_seen = s.size() == 2 ? s[1] : -1;
            const auto d = split->sizes();
            dragged_seen = d.empty() ? -1 : d[0];
            dragged_told = told;
        };
        depth_max(true);
        press(bar);
        drag_to(bar, -15);
        release(bar, -15);
        in_resize = nullptr;
        const unsigned deep = depth_max();
        expect(resized == 1, "the window resized from inside the drag's delivery (" + std::to_string(resized) + ")");
        if (layouts_seen)
            expect(deep == 1, "no layout of the window within the drag's: 1 deep at most (" + std::to_string(deep) + ")");
        expect(close_to(neighbour_seen, 255, scale),
               "after a nested layout, the other splitter reports the sizes asked of it: " + num(neighbour_seen));
        expect(close_to(dragged_seen, dragged_told, scale),
               "and the dragged one its latest placement: " + num(dragged_seen) + ", told " + num(dragged_told));
        pump();
        const auto s = pair->sizes();
        expect(s.size() == 2 && close_to(s[1], 255, scale) && close_to(told, split->sizes()[0], scale),
               "the layout after the drag's places the resize and the sizes asked for: " +
                   (s.size() == 2 ? num(s[1]) : "none"));
    }

    // A scroll pane as a splitter's first pane, moved at once while the
    // layout's placing (its host's WM_WINDOWPOSCHANGED), resizing the window
    // as it's moved: that layout isn't run within this one (which would
    // place the splitter and report sizes this one then places over), but
    // after it. The second pane's on_resize reads the size it's told each
    // time; after both, the sizes are the second layout's.
    {
        rap::splitter* d_split = nullptr;
        int d_calls = 0, d_stale = 0;
        float d_told = -1;
        auto d = (*ui)->create(window{
            "Splitter callbacks, pane resizing",
            {vbox{splitter{rap::orientation::horizontal, scroll{vbox{label{"In the pane"}}}.stretch(),
                           canvas{}.on_resize(
                               [&](rap::size_f s)
                               {
                                   d_told = s.w;
                                   if (!d_split || d_split->sizes().size() != 2) return;
                                   ++d_calls;
                                   if (!close_to(d_split->sizes()[1], s.w, scale)) ++d_stale;
                               })}
                      .bind(d_split)
                      .stretch()}}});
        HWND d_root = FindWindowW(nullptr, L"Splitter callbacks, pane resizing");
        // The pane's host: its label's panel's parent.
        HWND host = nullptr;
        if (d_root)
            EnumChildWindows(
                d_root,
                [](HWND h, LPARAM p) -> BOOL
                {
                    wchar_t text[32]{};
                    GetWindowTextW(h, text, 32);
                    if (std::wcscmp(text, L"In the pane")) return TRUE;
                    HWND panel = GetParent(h);
                    *reinterpret_cast<HWND*>(p) = panel ? GetParent(panel) : nullptr;
                    return FALSE;
                },
                reinterpret_cast<LPARAM>(&host));
        if (!d_split || !host)
            expect(false, "a pane resizing the window: the splitter and the scroll pane's host found");
        else
        {
            d->show();
            pump();
            d_split->set_sizes({0, 200});
            pump();
            hook_data hd{d.get()};
            SetWindowSubclass(host, resize_on_move, 0x106, reinterpret_cast<DWORD_PTR>(&hd));
            d_calls = d_stale = 0;
            depth_max(true);
            hd.armed = true;
            client_width(d_root, int(std::lround(620 * scale)));
            pump();
            RemoveWindowSubclass(host, resize_on_move, 0x106);
            const unsigned deep = depth_max();
            expect(hd.fired == 1, "a pane resizing the window: resized from its host's move (" +
                                      std::to_string(hd.fired) + ")");
            if (layouts_seen)
                expect(deep == 1, "a pane resizing the window: no layout within the layout (" + std::to_string(deep) +
                                      " deep)");
            expect(d_calls > 0 && d_stale == 0, "a pane resizing the window: the second pane's on_resize reads the "
                                                "size it's told each time (" +
                                                    std::to_string(d_stale) + " of " + std::to_string(d_calls) + " not)");
            const auto s = d_split->sizes();
            expect(s.size() == 2 && close_to(s[1], d_told, scale),
                   "after both layouts, sizes() is the second's: " + (s.size() == 2 ? num(s[1]) : "none") + ", told " +
                       num(d_told));
        }
        pump();
    }

    // The posted relayout pumped from inside a pass (a callback running the
    // message loop): it's done with that pass's re-run, and a layout asked
    // for later posts as ever, and is laid out.
    {
        pump();
        in_resize = [&]
        {
            split->set_sizes({0, 260});
            pump();  // the posted relayout, while this pass is under way
        };
        split->set_sizes({0, 200});
        pump();
        in_resize = nullptr;
        pump();
        const auto at = split->sizes();
        const float total = at.size() == 2 ? at[0] + at[1] : 0;
        expect(at.size() == 2 && close_to(at[1], 260, scale),
               "pumped inside a pass, the layout asked for there runs after it: " + (at.size() == 2 ? num(at[1]) : "none"));
        split->set_sizes({0, 315});
        pump();
        expect(close_to(told, total - 315, scale), "and a layout asked for later is laid out as it's posted: the canvas " +
                                                       num(told) + " for " + num(total - 315));
    }

    // A page shown in a stack (relayout_at_once) from a callback within a
    // layout: done after it as it would have been at once, measured anew,
    // so the canvas beside the stack gives way to its wider page.
    {
        rap::stack* f_stack = nullptr;
        rap::canvas* f_view = nullptr;
        std::function<void()> f_once;
        auto f = (*ui)->create(window{
            "Splitter callbacks, stack page",
            {hbox{canvas{}
                      .on_resize(
                          [&](rap::size_f)
                          {
                              if (auto fn = std::exchange(f_once, nullptr)) fn();
                          })
                      .bind(f_view)
                      .stretch(),
                  stack{{"narrow", label{"N"}},
                        {"wide", label{"A page a good deal wider than the other one, three hundred or so"}}}
                      .homogeneous(false)
                      .bind(f_stack)}}});
        HWND f_root = FindWindowW(nullptr, L"Splitter callbacks, stack page");
        if (!f_stack || !f_view || !f_root)
            expect(false, "a stack page: the stack and the canvas found");
        else
        {
            f->show();
            client_width(f_root, int(std::lround(800 * scale)));
            const float before = f_view->size().w;
            f_once = [&] { f_stack->show("wide"); };
            client_width(f_root, int(std::lround(820 * scale)));
            pump();
            const float after = f_view->size().w;
            // Measured anew, from scratch: what it should be.
            f_stack->set_visible(false);
            pump();
            f_stack->set_visible(true);
            pump();
            const float fresh = f_view->size().w;
            expect(f_stack->shown() == "wide", "a stack page: shown from the callback: " + f_stack->shown());
            expect(after < before && close_to(after, fresh, scale),
                   "a stack page: the canvas gives way to the wider page, " + num(after) + " (before " + num(before) +
                       ", measured anew " + num(fresh) + ")");
        }
    }

    // Another window laid out from within this one's placing (a scroll
    // pane's host here resizes that window as it's moved): its moves are a
    // batch of its own, delivered before its layout's done, so its
    // callbacks run within its layout, its sizes current, and a layout it
    // asks for there comes after; this window's layout goes on as it was.
    {
        rap::splitter* g_split = nullptr;
        rap::canvas* g_view = nullptr;
        rap::window* g_win_ptr = nullptr;
        int g_calls = 0, g_stale = 0;
        float g_told = -1, g_read = -1, g_told_then = -1;
        bool g_ask = false;
        auto gw = (*ui)->create(window{
            "Splitter callbacks, other window",
            {vbox{splitter{rap::orientation::horizontal,
                           canvas{}
                               .on_resize(
                                   [&](rap::size_f s)
                                   {
                                       g_told = s.w;
                                       if (!g_split || g_split->sizes().size() != 2) return;
                                       ++g_calls;
                                       if (!close_to(g_split->sizes()[0], s.w, scale)) ++g_stale;
                                       if (std::exchange(g_ask, false))
                                       {
                                           g_split->set_sizes({0, 170});
                                           g_told_then = s.w;
                                           g_read = g_view->size().w;  // within its layout: lays nothing out
                                       }
                                   })
                               .bind(g_view)
                               .stretch(),
                           label{"Other's other"}}
                      .bind(g_split)
                      .stretch()}}}.size(480_px, 200_px));
        g_win_ptr = gw.get();
        auto h = (*ui)->create(window{
            "Splitter callbacks, host window",
            {vbox{scroll{vbox{label{"In the host's pane"}}}.stretch()}}});
        HWND h_root = FindWindowW(nullptr, L"Splitter callbacks, host window");
        HWND host = nullptr;
        if (h_root)
            EnumChildWindows(
                h_root,
                [](HWND w, LPARAM p) -> BOOL
                {
                    wchar_t text[32]{};
                    GetWindowTextW(w, text, 32);
                    if (std::wcscmp(text, L"In the host's pane")) return TRUE;
                    HWND panel = GetParent(w);
                    *reinterpret_cast<HWND*>(p) = panel ? GetParent(panel) : nullptr;
                    return FALSE;
                },
                reinterpret_cast<LPARAM>(&host));
        if (!g_split || !g_view || !host)
            expect(false, "another window's layout: the windows found");
        else
        {
            gw->show();
            h->show();
            pump();
            hook_data hd{g_win_ptr};
            // That window laid out at once (sizes asked, then a size read),
            // while this one's still collecting its moves.
            hd.instead = [&]
            {
                g_split->set_sizes({0, 140});
                const float w = g_view->size().w;
                const auto s = g_split->sizes();
                std::printf("     in the hook: B's canvas %.1f, sizes %.1f %.1f, calls %d\n", w, s.size() == 2 ? s[0] : -1.f,
                            s.size() == 2 ? s[1] : -1.f, g_calls);
            };
            SetWindowSubclass(host, resize_on_move, 0x107, reinterpret_cast<DWORD_PTR>(&hd));
            g_calls = g_stale = 0;
            g_ask = true;
            depth_max(true);
            hd.armed = true;
            client_width(h_root, int(std::lround(500 * scale)));
            pump();
            RemoveWindowSubclass(host, resize_on_move, 0x107);
            const unsigned deep = depth_max();
            expect(hd.fired == 1, "another window's layout: started from this one's placing (" + std::to_string(hd.fired) + ")");
            if (layouts_seen)
                expect(deep == 1, "another window's layout: neither window laid out within itself (" + std::to_string(deep) +
                                      " deep)");
            expect(g_calls > 0 && g_stale == 0, "another window's layout: its on_resize reads its sizes current (" +
                                                    std::to_string(g_stale) + " of " + std::to_string(g_calls) + " not)");
            expect(g_read > 0 && close_to(g_read, g_told_then, scale),
                   "another window's layout: its callback runs within its layout (size() lays nothing out there): " +
                       num(g_read) + ", told " + num(g_told_then));
            const auto s = g_split->sizes();
            expect(s.size() == 2 && close_to(s[1], 170, scale) && close_to(g_view->size().w, s[0], scale),
                   "another window's layout: the sizes it asked for, laid out after: " + (s.size() == 2 ? num(s[1]) : "none"));
        }
        pump();
    }

    // A drag moved and let go of from within a layout of the window's own
    // (a callback running the message loop, the bar's messages in it): its
    // placement comes after that layout, and on_moved after it, seeing it.
    {
        pump();
        float moved_seen = -1;
        moved = 0;
        split->on_moved(
            [&]
            {
                ++moved;
                moved_seen = split->sizes().empty() ? -1 : split->sizes()[0];
            });
        press(bar);
        in_resize = [&]
        {
            drag_to(bar, 60);
            release(bar, 60);
        };
        split->set_sizes({0, 230});
        view->size();  // the layout, now; the drag within its delivery
        in_resize = nullptr;
        pump();
        const auto s = split->sizes();
        expect(moved == 1, "a drag let go of within a layout: on_moved once (" + std::to_string(moved) + ")");
        expect(s.size() == 2 && close_to(moved_seen, s[0], scale) && close_to(told, s[0], scale),
               "a drag let go of within a layout: on_moved sees the placement the drag made: " + num(moved_seen) +
                   ", placed " + (s.size() == 2 ? num(s[0]) : "none") + ", told " + num(told));
        split->on_moved([&] { ++moved; });
    }

    // The focus into a stack's new page, shown from a callback within a
    // layout: moved once the page is laid out (after that layout), as it is
    // when it's shown outside one. The window's pending focus (not active:
    // RAPPORT_NO_ACTIVATE) goes from the first page's entry to the second's.
    {
        rap::stack* k_stack = nullptr;
        std::function<void()> k_once;
        auto k = (*ui)->create(window{
            "Splitter callbacks, stack focus",
            {hbox{canvas{}
                      .on_resize(
                          [&](rap::size_f)
                          {
                              if (auto fn = std::exchange(k_once, nullptr)) fn();
                          })
                      .stretch(),
                  stack{{"a", entry{"Page a"}}, {"b", entry{"Page b"}}}.bind(k_stack)}}});
        HWND k_root = FindWindowW(nullptr, L"Splitter callbacks, stack focus");
        HWND found[2]{};
        if (k_root)
            EnumChildWindows(
                k_root,
                [](HWND w, LPARAM p) -> BOOL
                {
                    wchar_t text[16]{};
                    GetWindowTextW(w, text, 16);
                    auto* f = reinterpret_cast<HWND*>(p);
                    if (!std::wcscmp(text, L"Page a")) f[0] = w;
                    if (!std::wcscmp(text, L"Page b")) f[1] = w;
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(found));
        const HWND edit_a = found[0], edit_b = found[1];
        if (!k_stack || !edit_a || !edit_b)
            expect(false, "a stack's focus: the stack and its pages' entries found");
        else
        {
            k->show();
            pump();
            // Outside a callback first: what it does there.
            SetPropW(k_root, L"rapport-focus", edit_a);
            k_stack->show("b");
            pump();
            const HWND outside = static_cast<HWND>(GetPropW(k_root, L"rapport-focus"));
            k_stack->show("a");
            pump();
            // Then from a callback within a layout.
            SetPropW(k_root, L"rapport-focus", edit_a);
            k_once = [&] { k_stack->show("b"); };
            client_width(k_root, int(std::lround(520 * scale)));
            pump();
            const HWND inside = static_cast<HWND>(GetPropW(k_root, L"rapport-focus"));
            expect(outside == edit_b, "a stack's focus, shown outside a callback: the new page's entry");
            expect(k_stack->shown() == "b" && inside == edit_b,
                   "a stack's focus, shown from a callback within a layout: the new page's entry too (" +
                       std::string(inside == edit_b ? "it" : inside == edit_a ? "the old page's" : inside ? "another" : "none") +
                       ")");
        }
    }

    // An at-once layout (a stack's page shown in a shown window, drawing
    // off meanwhile) whose callback shows another stack's page, too wide for
    // the window: the window grows to it after, as it would have at once
    // (shown when it was asked, though drawing was off then).
    {
        rap::stack* m_outer = nullptr;
        rap::stack* m_inner = nullptr;
        std::function<void()> m_once;
        auto m = (*ui)->create(window{
            "Splitter callbacks, growing",
            {hbox{stack{{"small", label{"S"}}, {"bigger", label{"A somewhat bigger page"}}}.homogeneous(false).bind(m_outer),
                  canvas{}
                      .on_resize(
                          [&](rap::size_f)
                          {
                              if (auto fn = std::exchange(m_once, nullptr)) fn();
                          })
                      .stretch(),
                  stack{{"thin", label{"T"}},
                        {"wide", label{"A page far wider than this window is now, so the window has to grow to show "
                                       "all of it, every last word"}}}
                      .homogeneous(false)
                      .bind(m_inner)}}}.size(320_px, 120_px));
        HWND m_root = FindWindowW(nullptr, L"Splitter callbacks, growing");
        if (!m_outer || !m_inner || !m_root)
            expect(false, "growing: the stacks found");
        else
        {
            m->show();
            pump();
            RECT before{};
            GetClientRect(m_root, &before);
            m_once = [&] { m_inner->show("wide"); };
            m_outer->show("bigger");  // at once, the window shown
            pump();
            RECT after{};
            GetClientRect(m_root, &after);
            expect(m_inner->shown() == "wide", "growing: the wide page shown from the callback");
            expect(after.right > before.right + 100, "growing: the window grows to the wide page after (" +
                                                         std::to_string(before.right) + " to " +
                                                         std::to_string(after.right) + " px)");
        }
    }

    // A drag whose own layout's callback lets go of it (only the mouse-up,
    // within the drag's layout, which asks for no layout): on_moved once,
    // with the drag's placement, as the drag's layout ends, no other needed.
    {
        pump();
        float up_seen = -1;
        int up_moved = 0;
        split->on_moved(
            [&]
            {
                ++up_moved;
                up_seen = split->sizes().empty() ? -1 : split->sizes()[0];
            });
        press(bar);
        in_resize = [&] { release(bar, -18); };
        drag_to(bar, -18);  // the drag's layout, its on_resize letting go
        in_resize = nullptr;
        const int moved_then = up_moved;
        const auto s = split->sizes();
        expect(moved_then == 1 && !s.empty() && close_to(up_seen, s[0], scale) && close_to(told, s[0], scale),
               "a drag let go of from its own layout's callback: on_moved once as that layout ends, seeing it: " +
                   num(up_seen) + ", placed " + (s.empty() ? "none" : num(s[0])) + " (" + std::to_string(moved_then) +
                   " times)");
        pump();
        expect(up_moved == 1, "and not again (" + std::to_string(up_moved) + ")");
        split->on_moved([&] { ++moved; });
    }

    // A tab strip's on_changed for user switches within a layout: one, once
    // it's done, with the page current then (#100): two switches, B then C,
    // report C once; B then back to A report nothing.
    {
        rap::tabs* tb = nullptr;
        std::function<void()> tb_once;
        auto tw = (*ui)->create(window{
            "Splitter callbacks, tab switches",
            {hbox{canvas{}
                      .on_resize(
                          [&](rap::size_f)
                          {
                              if (auto fn = std::exchange(tb_once, nullptr)) fn();
                          })
                      .stretch(),
                  tabs{{"A", label{"Page A"}}, {"B", label{"Page B"}}, {"C", label{"Page C"}}}.bind(tb)}}}
                                     .size(520_px, 200_px));
        HWND tw_root = FindWindowW(nullptr, L"Splitter callbacks, tab switches");
        HWND tab = nullptr;
        for (HWND h = tw_root ? GetWindow(tw_root, GW_CHILD) : nullptr; h; h = GetWindow(h, GW_HWNDNEXT))
        {
            wchar_t cls[32]{};
            GetClassNameW(h, cls, 32);
            if (!std::wcscmp(cls, L"SysTabControl32")) tab = h;
        }
        if (!tb || !tab)
            expect(false, "tab switches: the tabs found");
        else
        {
            std::vector<int> reported;
            tb->on_changed([&](int i) { reported.push_back(i); });
            // A user's switch: the selection moved, the strip's TCN_SELCHANGE.
            auto user_switch = [&](int i)
            {
                TabCtrl_SetCurSel(tab, i);
                NMHDR n{tab, UINT_PTR(GetDlgCtrlID(tab)), UINT(TCN_SELCHANGE)};
                SendMessageW(GetParent(tab), WM_NOTIFY, n.idFrom, reinterpret_cast<LPARAM>(&n));
            };
            tw->show();
            pump();
            tb_once = [&]
            {
                user_switch(1);
                user_switch(2);
            };
            client_width(tw_root, int(std::lround(560 * scale)));
            pump();
            std::printf("     tab switches: callback ran %d, current %d, selection %d\n", tb_once ? 0 : 1, tb->current(),
                        int(TabCtrl_GetCurSel(tab)));
            expect(reported.size() == 1 && reported[0] == 2 && tb->current() == 2,
                   "tab switches B then C within a layout: one on_changed, C (" + std::to_string(reported.size()) +
                       (reported.empty() ? std::string() : ", last " + std::to_string(reported.back())) + ")");
            reported.clear();
            tb_once = [&]
            {
                user_switch(1);
                user_switch(2);  // back where it was
            };
            client_width(tw_root, int(std::lround(600 * scale)));
            pump();
            expect(reported.empty() && tb->current() == 2,
                   "tab switches away and back within a layout: no on_changed (" + std::to_string(reported.size()) + ")");
        }
    }

    // What waits for a layout runs in order, each after the layout the one
    // before asked for: two drags let go of within one layout (one after the
    // other), the first's on_moved asking the other splitter for new sizes;
    // the second's sees them laid out.
    {
        HWND pair_bar = static_cast<HWND>(bar_of(pair, 0));
        if (!pair_bar)
            expect(false, "in order: the second splitter's bar found");
        else
        {
            pump();
            float second_saw = -1;
            int order = 0, first_at = 0, second_at = 0;
            split->on_moved(
                [&]
                {
                    first_at = ++order;
                    pair->set_sizes({0, 222});  // a layout asked for
                });
            pair->on_moved(
                [&]
                {
                    second_at = ++order;
                    second_saw = second_told;
                });
            in_resize = [&]
            {
                press(bar);
                drag_to(bar, 10);
                release(bar, 10);
                press(pair_bar);
                drag_to(pair_bar, -10);
                release(pair_bar, -10);
            };
            split->set_sizes({0, 240});
            view->size();
            in_resize = nullptr;
            for (int i = 0; i < 3; ++i) pump();
            expect(first_at == 1 && second_at == 2, "in order: the first on_moved, then the second (" +
                                                        std::to_string(first_at) + ", " + std::to_string(second_at) + ")");
            expect(close_to(second_saw, 222, scale),
                   "in order: the second sees the layout the first asked for: the pane told " + num(second_saw) + " for 222");
            split->on_moved([&] { ++moved; });
            pair->on_moved([] {});
        }
    }

    // What waits for a layout made from within a call (a stack's page shown:
    // its at-once layout) isn't run within that call, but after it, from the
    // message loop: an on_moved queued there that destroys the window runs
    // once the stack's show has returned (under ASan in the asan job).
    {
        rap::stack* z_stack = nullptr;
        rap::splitter* z_split = nullptr;
        std::unique_ptr<rap::window> z;
        HWND z_bar = nullptr;
        bool z_drag = false;
        int z_moved = 0;
        z = (*ui)->create(window{
            "Splitter callbacks, gone after",
            {hbox{splitter{rap::orientation::horizontal,
                           canvas{}
                               .on_resize(
                                   [&](rap::size_f)
                                   {
                                       if (!std::exchange(z_drag, false) || !z_bar) return;
                                       press(z_bar);
                                       drag_to(z_bar, -14);
                                       release(z_bar, -14);
                                   })
                               .stretch(),
                           label{"Z"}}
                      .bind(z_split)
                      .stretch(),
                  stack{{"thin", label{"T"}}, {"wide", label{"A wider page than the other, so the panes give way"}}}
                      .homogeneous(false)
                      .bind(z_stack)}}}.size(560_px, 160_px));
        z_bar = z_split ? static_cast<HWND>(bar_of(z_split, 0)) : nullptr;
        if (!z_stack || !z_bar)
            expect(false, "gone after: the stack and the splitter found");
        else
        {
            z_split->on_moved(
                [&]
                {
                    ++z_moved;
                    z.reset();  // the window, from what waited for its layout
                });
            z->show();
            pump();
            z_drag = true;
            z_stack->show("wide");  // at once: the drag let go of within its layout
            const int during = z_moved;
            pump();
            expect(during == 0, "gone after: on_moved not run within the stack's show (" + std::to_string(during) + ")");
            expect(z_moved == 1 && !z, "gone after: on_moved ran after it, from the message loop, the window gone (" +
                                           std::to_string(z_moved) + ")");
        }
    }

    // What waits for a layout waits past the pass cap: a canvas asking for a
    // layout every time (20 times over), and a drag let go of on the 8th,
    // within a layout that's then capped and posted: on_moved runs once, when
    // no layout's owed any more, and sees the placement as it ends up.
    {
        rap::splitter* st_split = nullptr;
        rap::window* st_win = nullptr;
        HWND st_bar = nullptr;
        int st_asks = 0, st_moved = 0;
        float st_moved_seen = -1;
        auto st = (*ui)->create(window{
            "Splitter callbacks, storm",
            {vbox{splitter{rap::orientation::horizontal,
                           canvas{}
                               .on_resize(
                                   [&](rap::size_f)
                                   {
                                       if (!st_win || st_asks >= 20) return;
                                       if (++st_asks == 8 && st_bar)
                                       {
                                           press(st_bar);
                                           drag_to(st_bar, -12);
                                           release(st_bar, -12);
                                       }
                                       const rap::size_f now = st_win->size();
                                       st_win->resize({now.w + 2, now.h});
                                   })
                               .stretch(),
                           label{"Storm's other"}}
                      .bind(st_split)
                      .stretch()}}}.size(520_px, 160_px));
        st_win = st.get();
        HWND st_root = FindWindowW(nullptr, L"Splitter callbacks, storm");
        st_bar = st_split ? static_cast<HWND>(bar_of(st_split, 0)) : nullptr;
        if (!st_split || !st_bar || !st_root)
            expect(false, "a storm: the splitter and its bar found");
        else
        {
            st_split->on_moved(
                [&]
                {
                    ++st_moved;
                    st_moved_seen = st_split->sizes().empty() ? -1 : st_split->sizes()[0];
                });
            st->show();
            pump();
            st_asks = 0;
            RECT r{};
            GetWindowRect(st_root, &r);
            SetWindowPos(st_root, nullptr, 0, 0, (r.right - r.left) + 10, r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            const int moved_in_first = st_moved;
            for (int i = 0; i < 8; ++i) pump();
            const auto s = st_split->sizes();
            expect(st_asks == 20, "a storm: it asks 20 times (" + std::to_string(st_asks) + ")");
            expect(moved_in_first == 0, "a storm: on_moved not run within the capped layout (" +
                                            std::to_string(moved_in_first) + ")");
            expect(st_moved == 1 && !s.empty() && close_to(st_moved_seen, s[0], scale),
                   "a storm: on_moved once, after, seeing the placement as it ends up: " + num(st_moved_seen) +
                       ", placed " + (s.empty() ? "none" : num(s[0])) + " (" + std::to_string(st_moved) + " times)");
        }
    }

    // What waits for a layout may destroy the window: an on_moved, for a
    // drag let go of within a layout, that destroys its window. Nothing of
    // the window's touched after (under ASan in the asan job).
    {
        rap::splitter* dw_split = nullptr;
        std::unique_ptr<rap::window> dw;
        std::function<void()> dw_once;
        HWND dw_bar = nullptr;
        int dw_moved = 0;
        dw = (*ui)->create(window{
            "Splitter callbacks, destroyed after",
            {vbox{splitter{rap::orientation::horizontal,
                           canvas{}
                               .on_resize(
                                   [&](rap::size_f)
                                   {
                                       if (auto fn = std::exchange(dw_once, nullptr)) fn();
                                   })
                               .stretch(),
                           label{"Going"}}
                      .bind(dw_split)
                      .stretch()}}}.size(480_px, 160_px));
        dw_bar = dw_split ? static_cast<HWND>(bar_of(dw_split, 0)) : nullptr;
        if (!dw_split || !dw_bar)
            expect(false, "destroyed after: the splitter and its bar found");
        else
        {
            dw_split->on_moved(
                [&]
                {
                    ++dw_moved;
                    dw.reset();  // the window, from its own continuation
                });
            press(dw_bar);
            dw_once = [&]
            {
                drag_to(dw_bar, -20);
                release(dw_bar, -20);
            };
            dw_split->set_sizes({0, 150});
            pump();
            pump();
            expect(dw_moved == 1 && !dw, "destroyed after: on_moved ran and the window's gone, nothing touched after (" +
                                             std::to_string(dw_moved) + ")");
        }
    }

    // A window destroyed as window::resize's quiet ahead layout ends (the
    // canvases told nothing meanwhile, a thread-wide quiet): the quiet ends
    // with it, and a window made after still tells its canvas its size.
    if (layouts_seen)
    {
        std::unique_ptr<rap::window> q;
        q = (*ui)->create(window{"Splitter callbacks, quiet", {vbox{canvas{}.stretch()}}}.size(300_px, 150_px));
        q->show();
        pump();
        g_set_resize_quiet_hook([](void* p) { static_cast<std::unique_ptr<rap::window>*>(p)->reset(); }, &q);
        q->resize({360, 170});
        g_set_resize_quiet_hook(nullptr, nullptr);
        pump();
        // Its canvas told only through WM_SIZE (never shown, resized as a
        // window: no ahead layout), which the quiet holds back.
        int after_told = 0;
        auto later = (*ui)->create(window{"Splitter callbacks, after quiet",
                                          {vbox{canvas{}.on_resize([&](rap::size_f) { ++after_told; }).stretch()}}}
                                       .size(300_px, 150_px));
        pump();
        after_told = 0;
        client_width(FindWindowW(nullptr, L"Splitter callbacks, after quiet"), int(std::lround(420 * scale)));
        pump();
        expect(!q, "quiet: the window destroyed as its resize's quiet layout ended");
        expect(after_told > 0, "quiet: a window made after still tells its canvas its size (" + std::to_string(after_told) +
                                   " times)");
    }

    // A callback that asks for a layout every time (a canvas resizing its
    // window from its on_resize, 20 times over): the window's laid out again
    // after each, up to 8 passes in one go, then posted, and it settles.
    {
        rap::canvas* e_view = nullptr;
        rap::window* e_win = nullptr;
        int asks = 0;
        float e_told = -1;
        auto e = (*ui)->create(window{
            "Splitter callbacks, asking again",
            {vbox{canvas{}
                      .on_resize(
                          [&](rap::size_f s)
                          {
                              e_told = s.w;
                              if (!e_win || asks >= 20) return;
                              ++asks;
                              const rap::size_f now = e_win->size();
                              e_win->resize({now.w + 2, now.h});
                          })
                      .bind(e_view)
                      .stretch()}}});
        e_win = e.get();
        HWND e_root = FindWindowW(nullptr, L"Splitter callbacks, asking again");
        e->show();
        pump();
        asks = 0;
        depth_max(true);
        RECT r{};
        GetWindowRect(e_root, &r);
        SetWindowPos(e_root, nullptr, 0, 0, (r.right - r.left) + 10, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        const unsigned first = passes();
        for (int i = 0; i < 6; ++i) pump();
        if (layouts_seen)
        {
            expect(first == 8, "asking again every time: the layout stops at 8 passes in one go (" +
                                   std::to_string(first) + ")");
            expect(depth_max() == 1, "and never one within another (" + std::to_string(depth_max()) + " deep)");
        }
        expect(asks == 20, "and the rest come after, posted, until it stops asking (" + std::to_string(asks) + " of 20)");
        expect(e_view && close_to(e_view->size().w, e_told, scale),
               "it settles: the canvas laid out at the size it was told last: " +
                   (e_view ? num(e_view->size().w) : std::string("none")) + ", told " + num(e_told));
    }

    // 1, two panes in one layout: the first's on_resize asks for new sizes;
    // the second's, in the same layout, reads the sizes being placed. After
    // the layout, sizes() is what was asked for, and the layout it asked for
    // places it.
    pair->set_sizes({0, 150});
    pump();
    second_told = second_seen = -1;
    asked = second_after_ask = false;
    first_asks = [&]
    {
        pair->set_sizes({0, 260});
        asked = true;
    };
    pair->set_sizes({0, 190});
    view->size();  // the layout, now (outside any)
    expect(second_after_ask, "the first pane told first, and asked for new sizes before the second was told");
    expect(second_told > 0 && close_to(second_seen, second_told, scale),
           "the second pane's on_resize reads the size it's told, " + num(second_told) + ", not the one asked for "
           "from the first's: " + num(second_seen));
    {
        const auto s = pair->sizes();
        expect(s.size() == 2 && close_to(s[1], 260, scale),
               "after that layout, sizes() is the one asked for: " + (s.size() == 2 ? num(s[1]) : "none"));
    }
    pump();
    {
        const auto s = pair->sizes();
        expect(s.size() == 2 && close_to(s[1], 260, scale) && close_to(second_told, 260, scale),
               "and the layout it asked for places it: " + (s.size() == 2 ? num(s[1]) : "none") + ", told " +
                   num(second_told));
    }

    // 3 and 1, another window: a callback in this window's layout sets the
    // sizes of a splitter in another, and reads them back and its canvas's
    // size(). That window lays out then (its layout isn't this one's) and
    // reports the sizes asked for; this window's layout isn't re-entered.
    {
        rap::splitter* b_split = nullptr;
        rap::canvas* b_view = nullptr;
        auto b = (*ui)->create(window{
            "Splitter callbacks, another window",
            {vbox{splitter{rap::orientation::horizontal, canvas{}.bind(b_view).stretch(), label{"B's other"}}
                      .bind(b_split)
                      .stretch()}}});
        HWND b_root = FindWindowW(nullptr, L"Splitter callbacks, another window");
        if (!b_split || !b_view || !b_root)
            expect(false, "another window: its splitter found");
        else
        {
            client_width(b_root, int(std::lround(500 * scale)));
            const auto was = b_split->sizes();
            const float total = was.size() == 2 ? was[0] + was[1] : 0;
            float b_size_seen = -1, b_view_seen = -1;
            deepest = 0;
            in_resize = [&]
            {
                b_split->set_sizes({0, 140});
                const auto s = b_split->sizes();
                b_size_seen = s.size() == 2 ? s[1] : -1;
                b_view_seen = b_view->size().w;
            };
            split->set_sizes({0, 236});  // (a size no step before leaves it at)
            view->size();  // this window's layout, now
            expect(deepest == 1, "another window touched from this one's layout: this one's not re-entered (" +
                                     std::to_string(deepest) + " deep)");
            expect(close_to(b_size_seen, 140, scale),
                   "its splitter's sizes() is the one asked for: " + num(b_size_seen));
            expect(close_to(b_view_seen, total - 140, scale),
                   "and its canvas's size() is laid out for it then: " + num(b_view_seen) + ", for " +
                       num(total - 140));
        }
        pump();
    }

    // 4. A drag still on as the window goes.
    moved = 0;
    press(bar);
    drag_to(bar, -30);
    win.reset();
    pump();
    expect(moved == 0, "a drag on as the window is destroyed: no on_moved (" + std::to_string(moved) + ")");

    return failures ? 1 : 0;
}
