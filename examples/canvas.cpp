// anyui-canvas: exercises Canvas, Image and Painter.
//
// Drag to pan, wheel to zoom, arrow keys to nudge, Esc to quit. A circle
// orbits on the frame clock; a timer reports the frame rate each second.
#include "anyui/anyui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace anyui;
using namespace anyui::layout;

namespace
{

// 64x48 test card: colour gradient with a 1-pixel checker in one corner, so
// filtering and 1:1 placement are visible.
std::vector<uint8_t> testCard(int w, int h)
{
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            const bool checker = x < 16 && y < 16 && ((x ^ y) & 1);
            p[0] = checker ? 255 : uint8_t(255 * y / (h - 1));  // B
            p[1] = checker ? 255 : uint8_t(128);                // G
            p[2] = checker ? 255 : uint8_t(255 * x / (w - 1));  // R
            p[3] = 255;
        }
    return px;
}

// Straight-alpha RGBA: a soft red disc on transparency.
std::vector<uint8_t> disc(int n)
{
    std::vector<uint8_t> px(size_t(n) * n * 4);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            const float d = std::hypot(x - n / 2.f, y - n / 2.f) / (n / 2.f);
            uint8_t* p = &px[(size_t(y) * n + x) * 4];
            p[0] = 230;
            p[1] = 40;
            p[2] = 40;
            p[3] = uint8_t(255 * std::clamp(1.f - d, 0.f, 1.f));
        }
    return px;
}

}  // namespace

int main(int argc, char** argv)
{
    auto ui = Ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "%s\n", ui.error().c_str());
        return 1;
    }

    const auto card = testCard(64, 48);
    const auto discPx = disc(32);
    auto cardImage =
        (*ui)->createImage(64, 48, PixelFormat::Bgra8Premultiplied, card.data(), 64 * 4);
    auto discImage = (*ui)->createImage(32, 32, PixelFormat::Rgba8, discPx.data(), 32 * 4);

    anyui::Canvas* canvas = nullptr;
    anyui::Label* status = nullptr;
    PointF pan{0, 0}, dragFrom{};
    bool dragging = false;
    float zoom = 4;
    double now = 0;
    int frames = 0;

    auto paint = [&](Painter& p, SizeF size)
    {
        const float scale = p.deviceScale();
        p.rect({0, 0, size.w, size.h}, 0, Color{40, 40, 44}, std::nullopt);

        // 1:1 device pixels, then zoomed with each filter.
        p.image(*cardImage, {0, 0, 64, 48}, {16, 16, 64 / scale, 48 / scale});
        const float zw = 64 * zoom / scale, zh = 48 * zoom / scale;
        p.image(*cardImage, {0, 0, 64, 48}, {16 + pan.x, 80 + pan.y, zw, zh}, Filter::Nearest);
        p.image(*cardImage, {0, 0, 64, 48}, {32 + zw + pan.x, 80 + pan.y, zw, zh}, Filter::Linear,
                0.75f);
        p.text({16, 64},
               "1:1, then nearest and linear (75%) at zoom " + std::to_string(int(zoom * 100)) +
                   "%",
               {Color{220, 220, 220}});

        // Vector overlay: play triangle, busy arc, rounded panel.
        p.rect({size.w - 150, 16, 134, 60}, 8, Color{0, 0, 0, 160}, Color{255, 255, 255, 80});
        p.polygon({{size.w - 136, 30}, {size.w - 136, 62}, {size.w - 110, 46}},
                  Color{255, 255, 255}, Color{0, 0, 0}, 2);
        const float spin = float(std::fmod(now * 2, 2 * std::numbers::pi));
        p.arc({size.w - 56, 46}, 18, spin, spin + 4.5f, Color{120, 180, 255}, 4);

        // Orbiting straight-alpha image, driven by the frame clock.
        const float cx = size.w / 2 + 120 * float(std::cos(now)),
                    cy = size.h - 80 + 30 * float(std::sin(now));
        p.image(*discImage, {0, 0, 32, 32}, {cx - 16, cy - 16, 32, 32});

        p.pushClip({16, size.h - 40, 120, 24});
        p.text({16, size.h - 40}, "clipped text that runs past its box", {Color{255, 210, 0}});
        p.popClip();
    };

    auto win = (*ui)->create(layout::Window{
        "anyui canvas",
        {layout::VBox{
            layout::Canvas{}
                .onPaint(paint)
                .onPointer(
                    [&](const PointerEvent& e)
                    {
                        using T = PointerEvent::Type;
                        if (e.type == T::Press && e.button == 1)
                        {
                            dragging = true;
                            dragFrom = {e.pos.x - pan.x, e.pos.y - pan.y};
                            canvas->setCursor(Cursor::Grabbing);
                            std::printf("press clicks=%d\n", e.clicks);
                        }
                        else if (e.type == T::Release && e.button == 1)
                        {
                            dragging = false;
                            canvas->setCursor(Cursor::Grab);
                        }
                        else if (e.type == T::Move && dragging)
                        {
                            pan = {e.pos.x - dragFrom.x, e.pos.y - dragFrom.y};
                            canvas->invalidate();
                        }
                        else if (e.type == T::Enter)
                            canvas->setCursor(Cursor::Grab);
                        return true;
                    })
                .onScroll(
                    [&](const ScrollEvent& e)
                    {
                        zoom = std::clamp(zoom * std::pow(1.15f, -e.dy), 0.25f, 32.f);
                        std::printf("scroll dy=%.2f precise=%d\n", e.dy, e.precise);
                        canvas->invalidate();
                        return true;
                    })
                .onKey(
                    [&](const KeyEvent& e)
                    {
                        if (!e.press) return false;
                        std::printf("key %d '%c' mods=%u repeat=%d\n", int(e.key),
                                    e.codepoint < 128 ? char(e.codepoint) : '?', e.modifiers,
                                    e.repeat);
                        if (e.key == Key::Escape) (*ui)->quit();
                        if (e.key == Key::Left) pan.x -= 10;
                        if (e.key == Key::Right) pan.x += 10;
                        if (e.key == Key::Up) pan.y -= 10;
                        if (e.key == Key::Down) pan.y += 10;
                        canvas->invalidate();
                        return true;
                    })
                .bind(canvas)
                .stretch(),
            layout::Label{"starting"}.bind(status),
        }}}.size(60_ch, 30_lines));
    win->onCloseRequest(
        [&]
        {
            (*ui)->quit();
            return true;
        });

    auto frame = canvas->onFrame(
        [&](double t)
        {
            now = t;
            ++frames;
            canvas->invalidate();
        });
    auto report = (*ui)->timer(1000, true,
                               [&]
                               {
                                   status->setText(std::to_string(frames) + " frames/s, scale " +
                                                   std::to_string(int(canvas->deviceScale())));
                                   frames = 0;
                               });

    win->show();
    return (*ui)->run();
}
