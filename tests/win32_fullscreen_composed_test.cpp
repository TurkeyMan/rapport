// A fullscreen window's canvas is kept composed by the DWM (#121): its swap
// chain's alpha is premultiplied while the window's fullscreen, so it isn't
// promoted to direct or independent flip or an overlay plane, and it's
// opaque (ignored alpha) again once the window's back. The window is never
// shown (nothing reaches the screen): the canvas's paints are sent to it.
// Skips (77) where there's no GPU device for the canvas to draw with.
#include <rapport/rapport.h>
#define NOMINMAX
#include <windows.h>
#include <dxgi1_2.h>
#include "win32_test_windows.h"
#include <chrono>
#include <cstdio>
#include <cwchar>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
extern "C" int rapport_win32_canvas_alpha_mode(HWND);
#endif

namespace
{
int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

int (*g_alpha_mode)(HWND) = nullptr;
void find_seam()
{
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    g_alpha_mode = rapport_win32_canvas_alpha_mode;
#else
    if (HMODULE m = GetModuleHandleW(L"rapport_win32.dll"))
        g_alpha_mode = reinterpret_cast<int (*)(HWND)>(GetProcAddress(m, "rapport_win32_canvas_alpha_mode"));
#endif
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

HWND canvas_in(HWND window)
{
    HWND found = nullptr;
    EnumChildWindows(
        window,
        [](HWND child, LPARAM arg) -> BOOL
        {
            wchar_t cls[64]{};
            GetClassNameW(child, cls, 64);
            if (std::wcscmp(cls, L"rapportCanvas")) return TRUE;
            *reinterpret_cast<HWND*>(arg) = child;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// The canvas painted (sent: a hidden window gets no WM_PAINT) until its
// alpha mode is `want`, or a few seconds pass. What it was last.
int paint_until(HWND canvas, int want, int seconds)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    int mode = g_alpha_mode(canvas);
    while (mode != want && std::chrono::steady_clock::now() < until)
    {
        SendMessageW(canvas, WM_PAINT, 0, 0);
        pump();
        Sleep(10);
        mode = g_alpha_mode(canvas);
    }
    return mode;
}
}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui) return 1;
    find_seam();
    if (!g_alpha_mode)
    {
        std::fprintf(stderr, "no rapport_win32_canvas_alpha_mode seam\n");
        return 1;
    }
    using namespace rap::layout;
    rap::canvas* view = nullptr;
    auto win = (*ui)->create(window{"Fullscreen composed", {canvas{}.bind(view).stretch()}});
    HWND root = test_window(L"Fullscreen composed");
    HWND canvas = root ? canvas_in(root) : nullptr;
    if (!win || !view || !canvas) return 2;
    view->on_paint([](rap::painter& p, rap::size_f s)
                   { p.rect({0, 0, s.w, s.h}, 0, rap::paint{rap::color{50, 100, 150, 255}}, std::nullopt); });
    expect(!IsWindowVisible(root), "the test window is never shown");

    // Windowed: on the GPU, its swap chain opaque (alpha ignored).
    const int windowed = paint_until(canvas, DXGI_ALPHA_MODE_IGNORE, 10);
    if (windowed == -1)
    {
        std::fprintf(stderr, "no GPU device for the canvas: skipped\n");
        return 77;
    }
    expect(windowed == DXGI_ALPHA_MODE_IGNORE, "windowed, the swap chain's alpha is ignored");

    win->set_fullscreen(true);
    expect(win->fullscreen(), "fullscreen");
    expect(!IsWindowVisible(root), "still hidden in fullscreen");
    expect(paint_until(canvas, DXGI_ALPHA_MODE_PREMULTIPLIED, 5) == DXGI_ALPHA_MODE_PREMULTIPLIED,
           "fullscreen, the swap chain's alpha is premultiplied (kept composed)");
    // Painted again, it stays as it is.
    SendMessageW(canvas, WM_PAINT, 0, 0);
    expect(g_alpha_mode(canvas) == DXGI_ALPHA_MODE_PREMULTIPLIED, "fullscreen, still premultiplied a frame later");

    win->set_fullscreen(false);
    expect(!win->fullscreen(), "not fullscreen");
    expect(paint_until(canvas, DXGI_ALPHA_MODE_IGNORE, 5) == DXGI_ALPHA_MODE_IGNORE,
           "back from fullscreen, the swap chain's alpha is ignored again");

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
