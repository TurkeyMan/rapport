// A window destroyed from its own canvas's on_resize (#108): the app's
// handler drops the window (its unique_ptr) while the canvas is telling it
// its size, inside the window's layout. Nothing of the canvas, nor of the
// layout that placed it, is touched after (it crashed with 0xC000041D,
// the canvas going on to its frame target on freed memory), nor the
// controls placed after it in the same pass (a second canvas, told of its
// size). Each way a canvas hears of its size: the window resized through
// rapport while hidden (laid out as it takes the size) and while shown
// (laid out ahead of it, the canvas placed and told inside the layout's
// place), and by Windows (WM_SIZE); dropped from on_resize, with and
// without a frame target to tell after, and from the frame target itself.
// And the other ways into placing a window's controls, outside its layout:
// a scroll pane's host sized on its own (its WM_SIZE lays its content out),
// a splitter's bar dragged. Shown without activating (RAPPORT_NO_ACTIVATE).
#include <rapport/rapport.h>
#include <windows.h>

#include <cstdio>
#include <memory>
#include <string>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
extern "C" void* rapport_win32_trace_splitter_bar(rap::splitter*, unsigned);
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
            if (wcscmp(c, f->cls)) return TRUE;
            f->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // every line out, however the process ends
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui)
    {
        std::printf("no win32 backend: %s\n", ui.error().c_str());
        return 77;
    }
    using bar_lookup = void* (*)(rap::splitter*, unsigned);
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    bar_lookup bar_of = rapport_win32_trace_splitter_bar;
#else
    HMODULE module = GetModuleHandleW(L"rapport_win32.dll");
    bar_lookup bar_of = module ? reinterpret_cast<bar_lookup>(
                                     GetProcAddress(module, "rapport_win32_trace_splitter_bar"))
                               : nullptr;
#endif
    expect(bar_of != nullptr, "the backend's rapport_win32_trace_splitter_bar seam");
    if (!bar_of) return 1;
    using namespace rap::layout;
    using namespace rap::literals;

    // Who drops the window: on_resize (with a frame target to tell after, or
    // none), or the frame target itself (told after on_resize).
    enum class dropper
    {
        resize,
        resize_with_target,
        target,
    };
    // How it's resized: through rapport, hidden or shown (ahead), or by
    // Windows.
    enum class resized
    {
        hidden,
        ahead,
        by_windows,
        scroll_host,  // the scroll pane's host alone, by Windows (on a scroll pane)
        bar_drag,     // the splitter's bar dragged (in a splitter)
    };
    // What the canvas is in, the second canvas placed after it there: each
    // container stops placing when the window's gone.
    enum class holder
    {
        box,
        splitter,
        scroll,
        tab_page,
    };
    for (const holder in : {holder::box, holder::splitter, holder::scroll, holder::tab_page})
        for (const resized by : {resized::hidden, resized::ahead, resized::by_windows,
                                 resized::scroll_host, resized::bar_drag})
            for (const dropper who :
                 {dropper::resize, dropper::resize_with_target, dropper::target})
            {
                if ((by == resized::scroll_host && in != holder::scroll) ||
                    (by == resized::bar_drag && in != holder::splitter))
                    continue;
                const std::string how =
                    std::string(in == holder::box        ? "in a box, "
                                : in == holder::splitter ? "in a splitter, "
                                : in == holder::scroll   ? "on a scroll pane, "
                                                         : "on a tab page, ") +
                    (by == resized::hidden       ? "hidden, resized through rapport"
                     : by == resized::ahead      ? "shown, resized through rapport (laid out ahead)"
                     : by == resized::by_windows ? "resized by Windows (WM_SIZE)"
                     : by == resized::scroll_host ? "its host alone resized by Windows"
                                                  : "its bar dragged") +
                    (who == dropper::resize ? ", on_resize drops it"
                     : who == dropper::resize_with_target
                         ? ", on_resize drops it, a frame target set"
                         : ", its frame target drops it");
                std::unique_ptr<rap::window> win;
                rap::canvas* view = nullptr;
                rap::splitter* split = nullptr;
                bool armed = false, dropped = false;
                int after = 0;     // calls into the app after the canvas went
                int returned = 0;  // the dropping handler going on after, through its own captures
                const auto drop = [&]
                {
                    armed = false;
                    win.reset();  // the window, and this canvas with it
                    dropped = true;
                };
                auto first = canvas{}
                                 .on_resize(
                                     [&](rap::size_f)
                                     {
                                         if (dropped) ++after;
                                         if (armed && who != dropper::target)
                                         {
                                             drop();
                                             ++returned;  // still whole
                                         }
                                     })
                                 .bind(view)
                                 .stretch();
                // Placed after it in the same pass: told nothing once the
                // window's gone.
                auto second = canvas{}
                                  .on_resize(
                                      [&](rap::size_f)
                                      {
                                          if (dropped) ++after;
                                      })
                                  .stretch();
                switch (in)
                {
                case holder::box:
                    win = (*ui)->create(
                        window{"Canvas destroy", {vbox{first, second}.stretch()}}.size(300_px,
                                                                                       200_px));
                    break;
                case holder::splitter:
                    win = (*ui)->create(window{
                        "Canvas destroy",
                        {splitter{rap::orientation::horizontal, first, second}
                             .bind(split)
                             .stretch()}}.size(300_px, 200_px));
                    break;
                case holder::scroll:
                    win = (*ui)->create(
                        window{"Canvas destroy", {scroll{vbox{first, second}}.stretch()}}.size(
                            300_px, 200_px));
                    break;
                case holder::tab_page:
                    win = (*ui)->create(
                        window{"Canvas destroy", {tabs{{"One", vbox{first, second}}}.stretch()}}
                            .size(300_px, 200_px));
                    break;
                }
                HWND top = win ? FindWindowW(nullptr, L"Canvas destroy") : nullptr;
                expect(top && view, how + ": the window and its canvas");
                if (!top || !view) return 1;
                if (who != dropper::resize)
                    view->on_frame_target(
                        [&](const rap::frame_target&)
                        {
                            if (dropped) ++after;
                            if (armed && who == dropper::target)
                            {
                                drop();
                                ++returned;  // still whole
                            }
                        });
                if (by == resized::ahead) win->show();
                pump();
                expect((by == resized::ahead) == bool(IsWindowVisible(top)),
                       how + (by == resized::ahead ? ": shown" : ": hidden"));
                armed = true;
                if (by == resized::by_windows)
                    SetWindowPos(top, nullptr, 0, 0, 420, 330,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                else if (by == resized::scroll_host)
                {
                    // The canvas's panel's host: sized on its own, out of any
                    // layout of the window's.
                    const HWND canvas_h = find_class(top, L"rapportCanvas");
                    const HWND host = canvas_h ? GetParent(GetParent(canvas_h)) : nullptr;
                    RECT hr{};
                    if (host) GetClientRect(host, &hr);
                    expect(host != nullptr, how + ": the scroll pane's host");
                    if (host)
                        SetWindowPos(host, nullptr, 0, 0, hr.right + 60, hr.bottom + 40,
                                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
                else if (by == resized::bar_drag)
                {
                    const HWND bar = split ? static_cast<HWND>(bar_of(split, 0)) : nullptr;
                    expect(bar != nullptr, how + ": the splitter's bar");
                    if (bar)
                    {
                        SendMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2));
                        SendMessageW(bar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(32, 2));
                        // (The bar went with the window if the drag dropped it.)
                        if (IsWindow(bar)) SendMessageW(bar, WM_LBUTTONUP, 0, MAKELPARAM(32, 2));
                    }
                }
                else
                    win->resize({360, 260});
                pump();
                expect(dropped && returned == 1,
                       how + ": the window dropped, the handler going on after");
                expect(!win && !IsWindow(top),
                       how + ": the window's gone, nothing of it running after");
                expect(after == 0, how + ": nothing told after the canvas went (" +
                                       std::to_string(after) + ")");
            }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
