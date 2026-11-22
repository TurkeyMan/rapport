// A scroll pane's content loop under the single-entry layout rule (#106,
// #108): a canvas in a scroll pane asks for layouts from its on_resize (the
// window resized, a tab switch's relayout_at_once) as the window's resized,
// hidden and shown. Through the backend's private layout seams:
// - no window layout runs within another (rapport_win32_layout_depth_max:
//   deepest 1);
// - every layout_now's passes stay below the limit (max_layout_passes, 8),
//   none reaching it, across the whole case (rapport_win32_layout_limits:
//   the most any made, and how many hit it), not only the last's;
// - no scroll pane's content loop nests within its own (rapport_win32
//   _scroll_loop_stats: the deepest any one pane's went, 1), and no layout
//   of a window begins within a loop of that window begun outside any
//   layout of it (none).
// Every way into the scroll's loop app code has now is within a layout of
// its window (place_all, a splitter's drag, a DPI or theme change's
// relayout). A new one that ran the window's layout within the loop shows
// in the last. So it's seen to, per window, both ways: the one standalone
// way in there is, the scroll's host resized directly from outside any
// layout (which app code can't do, it has no handle to the host), its
// canvas resizing its window, counts there, even while another window's
// layout is under way; and its canvas resizing only another window doesn't.
// Shown without activating.
#include <rapport/rapport.h>
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <functional>
#include <memory>
#include <string>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
extern "C" unsigned rapport_win32_layout_depth_max(int reset);
extern "C" void rapport_win32_layout_limits(int reset, unsigned* passes_max, unsigned* cap_hits);
extern "C" void rapport_win32_scroll_loop_stats(int reset, unsigned* deepest,
                                                unsigned* layouts_in_standalone);
#endif

namespace
{
int fails = 0;
void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
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
HWND find_class(HWND top, const wchar_t* cls)
{
    struct find
    {
        const wchar_t* cls;
        HWND found = nullptr;
    } f{cls};
    EnumChildWindows(
        top,
        [](HWND h, LPARAM p) -> BOOL
        {
            auto* f = reinterpret_cast<find*>(p);
            wchar_t c[64]{};
            GetClassNameW(h, c, 64);
            if (std::wcscmp(c, f->cls)) return TRUE;
            f->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui)
    {
        std::printf("no win32 backend: %s\n", ui.error().c_str());
        return 77;
    }
    unsigned (*depth_max)(int) = nullptr;
    void (*limits)(int, unsigned*, unsigned*) = nullptr;
    void (*scroll_loops)(int, unsigned*, unsigned*) = nullptr;
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    depth_max = rapport_win32_layout_depth_max;
    limits = rapport_win32_layout_limits;
    scroll_loops = rapport_win32_scroll_loop_stats;
#else
    if (HMODULE m = GetModuleHandleW(L"rapport_win32.dll"))
    {
        depth_max = reinterpret_cast<unsigned (*)(int)>(GetProcAddress(m, "rapport_win32_layout_depth_max"));
        limits = reinterpret_cast<void (*)(int, unsigned*, unsigned*)>(
            GetProcAddress(m, "rapport_win32_layout_limits"));
        scroll_loops = reinterpret_cast<void (*)(int, unsigned*, unsigned*)>(
            GetProcAddress(m, "rapport_win32_scroll_loop_stats"));
    }
#endif
    expect(depth_max && limits && scroll_loops,
           "the backend's layout seams (linked in, or exported by rapport_win32.dll)");
    if (!depth_max || !limits || !scroll_loops) return 1;
    using namespace rap::layout;
    using namespace rap::literals;
    constexpr unsigned max_layout_passes = 8;  // the backend's

    enum class how
    {
        window_resized,  // the window resized by Windows (its layout)
        host_resized,    // the scroll's host alone, from outside any layout: the standalone way in
    };
    struct each
    {
        bool shown;
        bool by_resize;
        how via;
    };
    const each cases[] = {
        {false, true, how::window_resized}, {false, false, how::window_resized},
        {true, true, how::window_resized},  {true, false, how::window_resized},
        {false, true, how::host_resized},
    };
    for (const each& c : cases)
    {
        const std::string what =
            std::string(c.via == how::host_resized
                            ? "the scroll's host resized alone (the standalone way in)"
                        : c.shown ? "shown"
                                  : "hidden") +
            (c.by_resize ? ", the canvas resizing the window"
                         : ", the canvas switching tabs (relayout_at_once)");
        std::unique_ptr<rap::window> win;
        rap::tabs* pages = nullptr;
        int calls = 0, asked = 0;
        std::function<void()> ask;
        win = (*ui)->create(window{
            "Scroll depth",
            {tabs{{"One", scroll{vbox{canvas{}
                                          .on_resize(
                                              [&](rap::size_f)
                                              {
                                                  ++calls;
                                                  if (ask) ask();
                                              })
                                          .height(400_px)
                                          .stretch(),
                                      label{"Below"}}}
                              .stretch()},
                  {"Two", label{"Two"}}}
                 .bind(pages)
                 .stretch()}}.size(300_px, 200_px));
        HWND top = win ? FindWindowW(nullptr, L"Scroll depth") : nullptr;
        expect(top && pages, what + ": the window and its scrolled canvas");
        if (!top || !pages) return 1;
        if (c.shown) win->show();
        pump();
        // A few times, from within the layouts its own requests make.
        ask = [&]
        {
            if (++asked > 3) return;
            if (c.by_resize)
                win->resize({float(300 + 20 * asked), float(220 + 20 * asked)});
            else
            {
                pages->set_current(1);
                pages->set_current(0);
            }
        };
        calls = 0;
        depth_max(1);
        limits(1, nullptr, nullptr);
        scroll_loops(1, nullptr, nullptr);
        if (c.via == how::host_resized)
        {
            const HWND canvas_h = find_class(top, L"rapportCanvas");
            const HWND host = canvas_h ? GetParent(GetParent(canvas_h)) : nullptr;
            expect(host != nullptr, what + ": the scroll's host");
            RECT hr{};
            if (host) GetClientRect(host, &hr);
            if (host)
                SetWindowPos(host, nullptr, 0, 0, hr.right + 40, hr.bottom + 30,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else
            SetWindowPos(top, nullptr, 0, 0, 420, 330, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump();
        unsigned passes_max = 0, cap_hits = 0, loops_deepest = 0, in_standalone = 0;
        const unsigned deepest = depth_max(0);
        limits(0, &passes_max, &cap_hits);
        scroll_loops(0, &loops_deepest, &in_standalone);
        expect(calls > 0 && asked > 0,
               what + ": its on_resize asked for layouts (" + std::to_string(calls) + " calls)");
        expect(deepest == 1, what + ": no window layout within another (deepest " +
                                 std::to_string(deepest) + ")");
        expect(passes_max >= 1 && passes_max < max_layout_passes && cap_hits == 0,
               what + ": every layout's passes below the limit (the most " +
                   std::to_string(passes_max) + ", " + std::to_string(cap_hits) + " at the limit)");
        expect(loops_deepest == 1,
               what + ": the scroll's content loop not within another of its own (deepest " +
                   std::to_string(loops_deepest) + ")");
        if (c.via == how::host_resized)
            expect(in_standalone > 0,
                   what + ": seen, a window layout within a scroll loop no window layout holds (" +
                       std::to_string(in_standalone) + ")");
        else
            expect(in_standalone == 0,
                   what + ": no window layout within a scroll loop no window layout holds (" +
                       std::to_string(in_standalone) + ")");
        ask = nullptr;
        win.reset();
        pump();
    }
    // Per window, both ways (the single-entry rule is a window's own): two
    // windows, A (a canvas, no scroll) and B (a canvas in a scroll pane).
    // - A in the middle of its layout (from its canvas's on_resize) when B's
    //   host is resized directly: B's loop is still standalone for B (no
    //   layout of B holds it), and B laid out within it (its canvas resizing
    //   B) counts. A's layout under way doesn't hide it.
    // - B's host resized directly, its loop laying out only A (B's canvas
    //   resizing A): that's another window's layout, not one within B's own
    //   loop, so it doesn't count.
    for (const bool b_lays_out_b : {true, false})
    {
        const std::string what =
            b_lays_out_b ? "A mid-layout, B's host resized directly, B laid out within B's loop"
                         : "B's host resized directly, B's loop laying out only A";
        std::unique_ptr<rap::window> a, b;
        std::function<void()> on_a, on_b;  // once each, from the canvases' on_resize
        a = (*ui)->create(window{
            "Scroll depth A",
            {canvas{}
                 .on_resize(
                     [&](rap::size_f)
                     {
                         if (auto f = std::exchange(on_a, nullptr)) f();
                     })
                 .stretch()}}.size(240_px, 160_px));
        b = (*ui)->create(window{
            "Scroll depth B",
            {scroll{vbox{canvas{}
                             .on_resize(
                                 [&](rap::size_f)
                                 {
                                     if (auto f = std::exchange(on_b, nullptr)) f();
                                 })
                             .height(400_px)
                             .stretch(),
                         label{"Below"}}}
                 .stretch()}}.size(300_px, 200_px));
        HWND top_a = a ? FindWindowW(nullptr, L"Scroll depth A") : nullptr;
        HWND top_b = b ? FindWindowW(nullptr, L"Scroll depth B") : nullptr;
        const HWND canvas_b = top_b ? find_class(top_b, L"rapportCanvas") : nullptr;
        const HWND host_b = canvas_b ? GetParent(GetParent(canvas_b)) : nullptr;
        expect(top_a && top_b && host_b, what + ": the two windows and B's scroll host");
        if (!top_a || !top_b || !host_b) return 1;
        pump();
        bool b_called = false, a_called = false;
        const auto resize_b_host = [&]
        {
            RECT hr{};
            GetClientRect(host_b, &hr);
            SetWindowPos(host_b, nullptr, 0, 0, hr.right + 40, hr.bottom + 30,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        };
        if (b_lays_out_b)
        {
            on_b = [&]
            {
                b_called = true;
                b->resize({360, 260});  // a layout of B, within B's standalone loop
            };
            on_a = [&]
            {
                a_called = true;
                resize_b_host();  // within A's layout
            };
        }
        else
            on_b = [&]
            {
                b_called = true;
                a->resize({280, 200});  // a layout of A only
            };
        depth_max(1);
        limits(1, nullptr, nullptr);
        scroll_loops(1, nullptr, nullptr);
        if (b_lays_out_b)
            SetWindowPos(top_a, nullptr, 0, 0, 300, 220,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);  // A's layout
        else
            resize_b_host();
        pump();
        unsigned loops_deepest = 0, in_standalone = 0;
        scroll_loops(0, &loops_deepest, &in_standalone);
        expect(b_called && (!b_lays_out_b || a_called), what + ": the callbacks run");
        expect(loops_deepest == 1, what + ": no scroll pane's loop within its own (deepest " +
                                       std::to_string(loops_deepest) + ")");
        if (b_lays_out_b)
            expect(in_standalone > 0, what +
                                          ": counted, B laid out within B's loop no layout of B "
                                          "holds, A's under way or not (" +
                                          std::to_string(in_standalone) + ")");
        else
            expect(in_standalone == 0,
                   what + ": not counted, A's layout isn't one within B's own loop (" +
                       std::to_string(in_standalone) + ")");
        a.reset();
        b.reset();
        pump();
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
