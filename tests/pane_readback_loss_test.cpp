#include "gpu_internal.h"
#include <rapport/rapport.h>

#include <windows.h>
#include <array>
#include <chrono>
#include <cstdio>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{
HWND pane_canvas(HWND window)
{
    HWND result = nullptr;
    EnumChildWindows(window, [](HWND child, LPARAM arg) -> BOOL
    {
        wchar_t cls[64]{};
        GetClassNameW(child, cls, 64);
        if (wcscmp(cls, L"rapportCanvas")) return TRUE;
        for (HWND p = GetParent(child); p; p = GetParent(p))
        {
            if (!GetPropW(p, L"rapport-pane")) continue;
            *reinterpret_cast<HWND*>(arg) = child;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

COLORREF first_pixel(HWND canvas)
{
    RECT rect{};
    GetClientRect(canvas, &rect);
    HDC screen = GetDC(canvas);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, rect.right, rect.bottom);
    HGDIOBJ old = SelectObject(memory, bitmap);
    SendMessageW(canvas, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
    COLORREF color = GetPixel(memory, 0, 0);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(canvas, screen);
    return color;
}
}

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui) return 1;
    rap::canvas* primary = nullptr;
    rap::canvas* pane = nullptr;
    using namespace rap::layout;
    auto win = (*ui)->create(window{"Rapport pane readback loss", {
        vbox{canvas{}.bind(primary).height(3_lines),
             scroll{canvas{}.bind(pane).height(3_lines)}.height(4_lines)}}});
    win->show();
    std::shared_ptr<rap::gpu::device> dev;
    rap::gpu::texture_ptr texture;
    std::array<std::byte, 4> retired{};
    bool failed_frame_submitted = false;
    const auto start = std::chrono::steady_clock::now();
    auto tick = (*ui)->timer(20, true, [&]
    {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10))
        {
            std::fprintf(stderr, "pane readback timed out\n");
            (*ui)->quit(dev ? 1 : 77);
            return;
        }
        HWND h = pane_canvas(FindWindowW(nullptr, L"Rapport pane readback loss"));
        if (!h) return;
        if (!dev)
        {
            primary->invalidate();
            dev = pane->gpu_device();
            if (!dev) return;
            texture = dev->create({1, 1, rap::gpu::format::rgba8});
            if (!texture) { (*ui)->quit(1); return; }
            const std::array red{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
            pane->present(texture, dev->upload(texture, red, {}));
        }
        else if (!failed_frame_submitted && first_pixel(h) == RGB(255, 0, 0))
        {
            rap::gpu::fail_next_readback_map();
            pane->present(texture, {});
            failed_frame_submitted = true;
            // The device worker completes queued reads in order.
            dev->readback(texture, retired, 4, 0, [&, h]
            {
                (*ui)->post([&, h]
                {
                    (*ui)->quit(dev->lost() && first_pixel(h) == RGB(255, 0, 0) ? 0 : 1);
                });
            });
        }
    });
    return (*ui)->run();
}
