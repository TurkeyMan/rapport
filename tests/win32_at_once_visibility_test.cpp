// A shown window's decisions made while a relayout_at_once has its drawing
// off (#108): WM_SETREDRAW FALSE clears its WS_VISIBLE, so IsWindowVisible
// reads it, and every control in it, as hidden. What's decided in that
// time is decided as for the window shown. Shown without activating
// (RAPPORT_NO_ACTIVATE).
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
std::string rect_str(const RECT& r)
{
    return std::to_string(r.left) + "," + std::to_string(r.top) + " " + std::to_string(r.right - r.left) + "x" +
           std::to_string(r.bottom - r.top);
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
    using namespace rap::layout;
    using namespace rap::literals;

    // move(): kept clear of the work area's edge by the window's visible
    // frame (Windows' resize borders are invisible, and may overhang it).
    {
        std::unique_ptr<rap::window> win;
        rap::tabs* pages = nullptr;
        std::function<void()> in_layout;  // once, from the second page's canvas's first size
        win = (*ui)->create(window{"At-once visibility",
                                   {tabs{{"One", label{"First"}},
                                         {"Two", vbox{canvas{}
                                                          .on_resize(
                                                              [&](rap::size_f)
                                                              {
                                                                  if (auto f = std::exchange(in_layout, nullptr)) f();
                                                              })
                                                          .stretch()}}}
                                        .bind(pages)
                                        .stretch()}}
                                .size(320_px, 240_px));
        HWND top = win ? FindWindowW(nullptr, L"At-once visibility") : nullptr;
        expect(top && pages, "the window and its tabs");
        if (!top || !pages) return 1;
        win->show();
        pump();
        expect(IsWindowVisible(top) != FALSE, "shown");
        const float s = float(GetDpiForWindow(top)) / 96.f;
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(top, MONITOR_DEFAULTTONEAREST), &mi);
        RECT now;
        GetWindowRect(top, &now);
        // Its outer right edge 3 px past the work area's: inside the
        // invisible border, which may overhang it, so where it's put.
        const rap::point_f edge{(mi.rcWork.right + 3 - (now.right - now.left)) / s, (mi.rcWork.top + 40) / s};
        const rap::point_f away{(mi.rcWork.left + 40) / s, (mi.rcWork.top + 40) / s};
        win->move(edge);
        RECT plain;
        GetWindowRect(top, &plain);
        win->move(away);
        pump();
        RECT during{};
        in_layout = [&]
        {
            win->move(edge);
            GetWindowRect(top, &during);
        };
        pages->set_current(1);  // relayout_at_once: drawing off, the page laid out, its canvas sized
        pump();
        expect(!in_layout, "the second page's canvas sized within the switch");
        expect(EqualRect(&plain, &during) != FALSE,
               "move() within a relayout_at_once puts it where it puts it shown: " + rect_str(during) + ", shown " +
                   rect_str(plain));
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
