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
    const bool ordering = argc == 2 && std::string_view(argv[1]) == "--ordering";
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
    enum class step { red, cleared, cpu, gpu, loss };
    step phase = step::red;
    bool delivered = false;
    // Queue a UI notification after this frame's GPU worker callback and
    // window message. The next sample then includes any stale publication.
    const auto after_readback = [&]
    {
        delivered = false;
        dev->readback(texture, retired, 4, 0, [&] { (*ui)->post([&] { delivered = true; }); });
    };
    std::shared_ptr<const rap::image> green;
    COLORREF background = CLR_INVALID;
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
            background = first_pixel(h);
            texture = dev->create({1, 1, rap::gpu::format::rgba8});
            if (!texture) { (*ui)->quit(1); return; }
            const std::array red{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
            pane->present(texture, dev->upload(texture, red, {}));
        }
        else if (phase == step::red && first_pixel(h) == RGB(255, 0, 0))
        {
            if (ordering)
            {
                pane->present(texture, {});
                pane->clear_frame();
                phase = step::cleared;
            }
            else
            {
                rap::gpu::fail_next_readback_map();
                pane->present(texture, {});
                phase = step::loss;
            }
            after_readback();
        }
        else if (delivered)
        {
            const COLORREF actual = first_pixel(h);
            switch (phase)
            {
            case step::cleared:
            {
                if (background == CLR_INVALID || actual != background) { (*ui)->quit(1); return; }
                const uint8_t pixel[]{0, 255, 0, 255};
                green = (*ui)->create_image(1, 1, rap::pixel_format::rgba8, pixel, 4);
                pane->present(texture, {});
                pane->present(green);
                phase = step::cpu;
                after_readback();
                break;
            }
            case step::cpu:
                if (actual != RGB(0, 255, 0)) { (*ui)->quit(1); return; }
                pane->present(texture, {});
                phase = step::gpu;
                after_readback();
                break;
            case step::gpu: (*ui)->quit(actual == RGB(255, 0, 0) ? 0 : 1); break;
            case step::loss: (*ui)->quit(dev->lost() && actual == RGB(255, 0, 0) ? 0 : 1); break;
            case step::red: break;
            }
        }
    });
    return (*ui)->run();
}
