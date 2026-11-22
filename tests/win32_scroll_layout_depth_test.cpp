// A scroll pane's content loop under the single-entry layout rule (#106,
// #108): a canvas in a scroll pane asks for layouts from its on_resize (the
// window resized, a tab switch's relayout_at_once) as the window's resized,
// hidden and shown. No window layout runs within another (the deepest is 1),
// and the passes settle below the limit (max_layout_passes, 8). Every way
// into the scroll's loop now is within a window layout (place_all, a
// splitter's drag, a DPI or theme change's relayout); a new way in that ran
// a window's layout within the loop would show here. Read through the
// backend's layout seams (rapport_win32_layout_depth_max,
// rapport_win32_layout_passes). Shown without activating.
#include <rapport/rapport.h>
#include <windows.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
extern "C" unsigned rapport_win32_layout_depth_max(int reset);
extern "C" unsigned rapport_win32_layout_passes();
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
    unsigned (*passes)() = nullptr;
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    depth_max = rapport_win32_layout_depth_max;
    passes = rapport_win32_layout_passes;
#else
    if (HMODULE m = GetModuleHandleW(L"rapport_win32.dll"))
    {
        depth_max = reinterpret_cast<unsigned (*)(int)>(GetProcAddress(m, "rapport_win32_layout_depth_max"));
        passes = reinterpret_cast<unsigned (*)()>(GetProcAddress(m, "rapport_win32_layout_passes"));
    }
#endif
    expect(depth_max && passes, "the backend's layout seams (linked in, or exported by rapport_win32.dll)");
    if (!depth_max || !passes) return 1;
    using namespace rap::layout;
    using namespace rap::literals;
    constexpr unsigned max_layout_passes = 8;  // the backend's

    for (const bool shown : {false, true})
        for (const bool by_resize : {true, false})
        {
            const std::string what = std::string(shown ? "shown" : "hidden") +
                                      (by_resize ? ", the canvas resizing the window"
                                                 : ", the canvas switching tabs (relayout_at_once)");
            std::unique_ptr<rap::window> win;
            rap::tabs* pages = nullptr;
            int calls = 0, asked = 0;
            std::function<void()> ask;
            win = (*ui)->create(window{"Scroll depth",
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
                                            .stretch()}}
                                    .size(300_px, 200_px));
            HWND top = win ? FindWindowW(nullptr, L"Scroll depth") : nullptr;
            expect(top && pages, what + ": the window and its scrolled canvas");
            if (!top || !pages) return 1;
            if (shown) win->show();
            pump();
            // A few times, from within the layouts its own requests make.
            ask = [&]
            {
                if (++asked > 3) return;
                if (by_resize)
                    win->resize({float(300 + 20 * asked), float(220 + 20 * asked)});
                else
                {
                    pages->set_current(1);
                    pages->set_current(0);
                }
            };
            calls = 0;
            depth_max(1);
            SetWindowPos(top, nullptr, 0, 0, 420, 330, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            pump();
            const unsigned deepest = depth_max(0), last = passes();
            expect(calls > 0 && asked > 0, what + ": its on_resize asked for layouts (" + std::to_string(calls) +
                                               " calls)");
            expect(deepest == 1, what + ": no window layout within another (deepest " + std::to_string(deepest) + ")");
            expect(last >= 1 && last < max_layout_passes,
                   what + ": the passes settle below the limit (" + std::to_string(last) + ")");
            ask = nullptr;
            win.reset();
            pump();
        }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
