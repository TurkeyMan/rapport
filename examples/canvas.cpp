// anyui-canvas: exercises Canvas, Image, Painter, icons and the toolbar /
// slider / spin / colour widgets, logging every input event to stdout.
//
// Canvas: drag an image to pan (left button), wheel to zoom, middle-click to
// reset, right-click logs. Click the drawn play/pause button (or the toolbar
// toggle) to pause the orbiting disc. Keys: arrows pan, F11 fullscreen,
// Ctrl+C copies the test card, Esc quits.
#include "anyui/anyui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

using namespace anyui;

namespace
{

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

std::string mods(unsigned m)
{
    std::string s;
    if (m & ModCtrl) s += "Ctrl+";
    if (m & ModShift) s += "Shift+";
    if (m & ModAlt) s += "Alt+";
    if (m & ModMeta) s += "Meta+";
    return s.empty() ? "-" : s.substr(0, s.size() - 1);
}

const char* typeName(PointerEvent::Type t)
{
    switch (t)
    {
    case PointerEvent::Type::Press: return "press";
    case PointerEvent::Type::Release: return "release";
    case PointerEvent::Type::Move: return "move";
    case PointerEvent::Type::Enter: return "enter";
    case PointerEvent::Type::Leave: return "leave";
    }
    return "?";
}

bool inside(RectF r, PointF p)
{
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

// Drawn icons: follow the theme's text colour and the display scale.
Icon playPauseIcon(const bool& paused)
{
    return Icon::drawn(
        [&paused](Painter& p, SizeF s)
        {
            const Paint ink = ThemeColor::WindowText;
            if (paused)
                p.polygon({{s.w * 0.25f, s.h * 0.15f},
                           {s.w * 0.25f, s.h * 0.85f},
                           {s.w * 0.85f, s.h * 0.5f}},
                          ink, std::nullopt);
            else
            {
                p.rect({s.w * 0.2f, s.h * 0.15f, s.w * 0.22f, s.h * 0.7f}, 1, ink, std::nullopt);
                p.rect({s.w * 0.58f, s.h * 0.15f, s.w * 0.22f, s.h * 0.7f}, 1, ink, std::nullopt);
            }
        });
}

Icon magnifier(bool plus)
{
    return Icon::drawn(
        [plus](Painter& p, SizeF s)
        {
            const Paint ink = ThemeColor::WindowText;
            const float r = s.w * 0.3f;
            const PointF c{s.w * 0.42f, s.h * 0.42f};
            p.circle(c, r, std::nullopt, ink);
            p.line({c.x + r * 0.7f, c.y + r * 0.7f}, {s.w * 0.9f, s.h * 0.9f}, ink, s.w * 0.12f);
            p.line({c.x - r * 0.5f, c.y}, {c.x + r * 0.5f, c.y}, ink, 1.5f);
            if (plus) p.line({c.x, c.y - r * 0.5f}, {c.x, c.y + r * 0.5f}, ink, 1.5f);
        });
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
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("backend: %s\n", std::string((*ui)->backendName()).c_str());

    const auto card = testCard(64, 48);
    const auto discPx = disc(32);
    auto cardImage =
        (*ui)->createImage(64, 48, PixelFormat::Bgra8Premultiplied, card.data(), 64 * 4);
    auto discImage = (*ui)->createImage(32, 32, PixelFormat::Rgba8, discPx.data(), 32 * 4);

    anyui::Canvas* canvas = nullptr;
    anyui::Label* status = nullptr;
    anyui::Slider* zoomSlider = nullptr;
    anyui::ToolButton* playToggle = nullptr;
    std::unique_ptr<anyui::Window> win;

    PointF pan{0, 0}, dragFrom{};
    bool dragging = false, paused = false;
    float zoom = 4, padding = 16;
    Color background{40, 40, 44};
    double clock = 0, last = -1;
    int frames = 0;
    SizeF lastSize{};

    // Geometry shared by painting and hit testing.
    auto playRect = [&] { return RectF{lastSize.w - 150, padding, 60, 60}; };
    auto zoomedRects = [&](float scale)
    {
        const float zw = 64 * zoom / scale, zh = 48 * zoom / scale;
        return std::pair{RectF{padding + pan.x, 80 + pan.y, zw, zh},
                         RectF{2 * padding + zw + pan.x, 80 + pan.y, zw, zh}};
    };
    auto setZoom = [&](float z)
    {
        zoom = std::clamp(z, 0.25f, 32.f);
        if (zoomSlider) zoomSlider->setValue(zoom);
        canvas->invalidate();
    };
    auto setPaused = [&](bool p)
    {
        paused = p;
        if (playToggle) playToggle->setChecked(!paused);
        std::printf("animation %s\n", paused ? "paused" : "playing");
        canvas->invalidate();
    };

    auto paint = [&](Painter& p, SizeF size)
    {
        lastSize = size;
        const float scale = p.deviceScale();
        p.rect({0, 0, size.w, size.h}, 0, background, std::nullopt);

        p.image(*cardImage, {0, 0, 64, 48}, {padding, padding, 64 / scale, 48 / scale});
        auto [nearest, linear] = zoomedRects(scale);
        p.image(*cardImage, {0, 0, 64, 48}, nearest, Filter::Nearest);
        p.image(*cardImage, {0, 0, 64, 48}, linear, Filter::Linear, 0.75f);
        p.text({padding, 64},
               "1:1, then nearest and linear (75%) at " + std::to_string(int(zoom * 100)) + "%",
               {Color{220, 220, 220}});

        // Drawn play/pause button (clickable) and a busy arc.
        const RectF b = playRect();
        p.rect(b, 8, Color{0, 0, 0, 160}, Color{255, 255, 255, 80});
        if (paused)
            p.polygon({{b.x + 22, b.y + 16}, {b.x + 22, b.y + 44}, {b.x + 44, b.y + 30}},
                      Color{255, 255, 255}, Color{0, 0, 0}, 2);
        else
        {
            p.rect({b.x + 19, b.y + 16, 8, 28}, 1, Color{255, 255, 255}, Color{0, 0, 0});
            p.rect({b.x + 33, b.y + 16, 8, 28}, 1, Color{255, 255, 255}, Color{0, 0, 0});
        }
        const float spin = float(std::fmod(clock * 2, 2 * std::numbers::pi));
        p.arc({size.w - 50, padding + 30}, 18, spin, spin + 4.5f, Color{120, 180, 255}, 4);

        const float cx = size.w / 2 + 120 * float(std::cos(clock));
        const float cy = size.h - 80 + 30 * float(std::sin(clock));
        p.image(*discImage, {0, 0, 32, 32}, {cx - 16, cy - 16, 32, 32});

        p.pushClip({padding, size.h - 40, 120, 24});
        p.text({padding, size.h - 40}, "clipped text that runs past its box", {Color{255, 210, 0}});
        p.popClip();
    };

    auto cursorFor = [&](PointF pos)
    {
        if (dragging) return Cursor::Grabbing;
        if (inside(playRect(), pos)) return Cursor::Pointer;
        auto [a, b] = zoomedRects(canvas->deviceScale());
        if (inside(a, pos) || inside(b, pos)) return Cursor::Grab;
        return Cursor::Default;
    };

    auto onPointer = [&](const PointerEvent& e)
    {
        using T = PointerEvent::Type;
        if (e.type != T::Move || dragging)  // moves only while dragging, or the log floods
            std::printf("pointer %-7s button=%d clicks=%d mods=%s at %.0f,%.0f\n", typeName(e.type),
                        e.button, e.clicks, mods(e.modifiers).c_str(), e.pos.x, e.pos.y);
        if (e.type == T::Press && e.button == 1)
        {
            if (inside(playRect(), e.pos))
                setPaused(!paused);
            else if (cursorFor(e.pos) == Cursor::Grab)
            {
                dragging = true;
                dragFrom = {e.pos.x - pan.x, e.pos.y - pan.y};
            }
        }
        else if (e.type == T::Press && e.button == 2)
        {
            pan = {0, 0};
            setZoom(4);
        }
        else if (e.type == T::Release && e.button == 1)
            dragging = false;
        else if (e.type == T::Move && dragging)
        {
            pan = {e.pos.x - dragFrom.x, e.pos.y - dragFrom.y};
            canvas->invalidate();
        }
        canvas->setCursor(e.type == T::Leave ? Cursor::Default : cursorFor(e.pos));
        return true;
    };

    auto onScroll = [&](const ScrollEvent& e)
    {
        std::printf("scroll dx=%.2f dy=%.2f precise=%d mods=%s at %.0f,%.0f\n", e.dx, e.dy,
                    e.precise, mods(e.modifiers).c_str(), e.pos.x, e.pos.y);
        setZoom(zoom * std::pow(1.15f, -e.dy));
        return true;
    };

    // Window-level keys work whatever has focus (toolbar, slider, canvas).
    auto onKey = [&](const KeyEvent& e)
    {
        const std::string ch =
            e.key == Key::Character && e.codepoint < 128 ? std::string(1, char(e.codepoint)) : "";
        std::printf("key %s %d '%s' mods=%s repeat=%d\n", e.press ? "press  " : "release",
                    int(e.key), ch.c_str(), mods(e.modifiers).c_str(), e.repeat);
        if (!e.press) return false;
        switch (e.key)
        {
        case Key::Escape: (*ui)->quit(); return true;
        case Key::F11: win->setFullscreen(!win->fullscreen()); return true;
        case Key::Left: pan.x -= 10; break;
        case Key::Right: pan.x += 10; break;
        case Key::Up: pan.y -= 10; break;
        case Key::Down: pan.y += 10; break;
        case Key::Character:
            if (e.codepoint == 'c' && (e.modifiers & ModCtrl))
            {
                (*ui)->setClipboardImage(64, 48, PixelFormat::Bgra8Premultiplied, card.data(),
                                         64 * 4);
                std::printf("copied the test card\n");
                return true;
            }
            return false;
        default: return false;
        }
        canvas->invalidate();
        return true;
    };

    using namespace anyui::layout;
    win = (*ui)->create(layout::Window{
        "anyui canvas",
        {VBox{
            layout::Toolbar{
                layout::ToolButton{playPauseIcon(paused), "Play / pause"}
                    .toggle(true)
                    .onToggled([&](bool on) { setPaused(!on); })
                    .bind(playToggle),
                Separator{},
                layout::ToolButton{magnifier(false), "Zoom out"}.onClick(
                    [&] { setZoom(zoom / 1.25f); }),
                layout::ToolButton{magnifier(true), "Zoom in"}.onClick([&]
                                                                       { setZoom(zoom * 1.25f); }),
                layout::ToolButton{Icon::named("zoom-original"), "Reset"}.onClick(
                    [&]
                    {
                        pan = {0, 0};
                        setZoom(4);
                    }),
                Spacer{},
                layout::Label{"Zoom"},
                layout::Slider{0.25, 32, zoom}
                    .step(0.25, 1)
                    .mark(1, "1×")
                    .mark(4)
                    .mark(16, "16×")
                    .onChanged([&](double v) { setZoom(float(v)); })
                    .bind(zoomSlider)
                    .width(20_ch),
                layout::Label{"Padding"},
                layout::Spin{0, 64, padding}.onChanged(
                    [&](double v)
                    {
                        padding = float(v);
                        canvas->invalidate();
                    }),
                layout::ColorButton{background, "Background"}.onChanged(
                    [&](Color c)
                    {
                        background = c;
                        std::printf("background %d,%d,%d\n", c.r, c.g, c.b);
                        canvas->invalidate();
                    }),
            },
            layout::Canvas{}
                .onPaint(paint)
                .onPointer(onPointer)
                .onScroll(onScroll)
                .bind(canvas)
                .stretch(),
            HBox{
                layout::Label{"starting"}.bind(status).stretch(),
                layout::Button{"Copy image"}
                    .icon(Icon::named("edit-copy"))
                    .onClick(
                        [&]
                        {
                            (*ui)->setClipboardImage(64, 48, PixelFormat::Bgra8Premultiplied,
                                                     card.data(), 64 * 4);
                            std::printf("copied the test card\n");
                        }),
                layout::Button{"Fullscreen"}
                    .icon(Icon::named("view-fullscreen"))
                    .iconOnly()
                    .onClick([&] { win->setFullscreen(!win->fullscreen()); }),
            },
        }}}.size(80_ch, 34_lines));
    win->onCloseRequest(
        [&]
        {
            (*ui)->quit();
            return true;
        });
    win->onKey(onKey);
    win->onFullscreenChanged([](bool fs) { std::printf("fullscreen %s\n", fs ? "on" : "off"); });
    win->setIcon(magnifier(true));
    playToggle->setChecked(true);

    // The frame clock drives the orbit; paused time doesn't advance it.
    auto frame = canvas->onFrame(
        [&](double t)
        {
            if (last >= 0 && !paused) clock += t - last;
            last = t;
            ++frames;
            canvas->invalidate();
        });
    auto report = (*ui)->timer(
        1000, true,
        [&]
        {
            const SizeF ws = win->size();
            const RectF wa = win->workArea();
            const auto pos = win->position();
            const std::string where =
                pos ? " at " + std::to_string(int(pos->x)) + "," + std::to_string(int(pos->y))
                    : " (position unknown)";
            char buf[200];
            std::snprintf(buf, sizeof buf,
                          "%d frames/s, scale %.0f, window %.0f×%.0f%s, work area %.0f×%.0f",
                          frames, canvas->deviceScale(), ws.w, ws.h, where.c_str(), wa.w, wa.h);
            status->setText(buf);
            frames = 0;
        });

    win->show();
    canvas->focus();  // keys go to the view, not the first toolbar button
    return (*ui)->run();
}
