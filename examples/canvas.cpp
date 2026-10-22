// rapport-canvas: exercises Canvas, Image, Painter, icons and the toolbar /
// slider / spin / colour widgets, logging every input event to stdout.
//
// Canvas: drag an image to pan (left button), wheel to zoom, middle-click to
// reset, right-click logs. Click the drawn play/pause button (or the toolbar
// toggle) to pause the orbiting disc. Keys: arrows pan, F11 fullscreen,
// Ctrl+C copies the test card, Esc quits.
#include "rapport/rapport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

using namespace rap;

namespace
{

std::vector<uint8_t> test_card(int w, int h)
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
    if (m & mod_ctrl) s += "Ctrl+";
    if (m & mod_shift) s += "Shift+";
    if (m & mod_alt) s += "Alt+";
    if (m & mod_meta) s += "Meta+";
    return s.empty() ? "-" : s.substr(0, s.size() - 1);
}

const char* type_name(pointer_event_type t)
{
    switch (t)
    {
    case pointer_event_type::press: return "press";
    case pointer_event_type::release: return "release";
    case pointer_event_type::move: return "move";
    case pointer_event_type::enter: return "enter";
    case pointer_event_type::leave: return "leave";
    }
    return "?";
}

bool inside(rect_f r, point_f p)
{
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

// Drawn icons: follow the theme's text colour and the display scale.
icon play_pause_icon(const bool& paused)
{
    return icon::drawn(
        [&paused](painter& p, size_f s)
        {
            const paint ink = theme_color::window_text;
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

icon magnifier(bool plus)
{
    return icon::drawn(
        [plus](painter& p, size_f s)
        {
            const paint ink = theme_color::window_text;
            const float r = s.w * 0.3f;
            const point_f c{s.w * 0.42f, s.h * 0.42f};
            p.circle(c, r, std::nullopt, ink);
            p.line({c.x + r * 0.7f, c.y + r * 0.7f}, {s.w * 0.9f, s.h * 0.9f}, ink, s.w * 0.12f);
            p.line({c.x - r * 0.5f, c.y}, {c.x + r * 0.5f, c.y}, ink, 1.5f);
            if (plus) p.line({c.x, c.y - r * 0.5f}, {c.x, c.y + r * 0.5f}, ink, 1.5f);
        });
}

}  // namespace

int main(int argc, char** argv)
{
    auto ui = ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "%s\n", ui.error().c_str());
        return 1;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("backend: %s\n", std::string((*ui)->backend_name()).c_str());

    const auto card = test_card(64, 48);
    const auto disc_px = disc(32);
    auto card_image =
        (*ui)->create_image(64, 48, pixel_format::bgra8_premultiplied, card.data(), 64 * 4);
    auto disc_image = (*ui)->create_image(32, 32, pixel_format::rgba8, disc_px.data(), 32 * 4);

    rap::canvas* canvas = nullptr;
    rap::label* status = nullptr;
    rap::slider* zoom_slider = nullptr;
    rap::tool_button* play_toggle = nullptr;
    rap::widget* toolbar = nullptr;  // handles: hidden in fullscreen
    rap::widget* bottom_bar = nullptr;
    std::unique_ptr<rap::window> win;

    point_f pan{0, 0}, drag_from{};
    bool dragging = false, paused = false;
    float zoom = 4, padding = 16;
    color background{40, 40, 44};
    double clock = 0, last = -1;
    int frames = 0;
    size_f last_size{};

    // Geometry shared by painting and hit testing.
    auto play_rect = [&] { return rect_f{last_size.w - 150, padding, 60, 60}; };
    auto zoomed_rects = [&](float scale)
    {
        const float zw = 64 * zoom / scale, zh = 48 * zoom / scale;
        return std::pair{rect_f{padding + pan.x, 80 + pan.y, zw, zh},
                         rect_f{2 * padding + zw + pan.x, 80 + pan.y, zw, zh}};
    };
    auto set_zoom = [&](float z)
    {
        zoom = std::clamp(z, 0.25f, 32.f);
        if (zoom_slider) zoom_slider->set_value(zoom);
        canvas->invalidate();
    };
    auto set_paused = [&](bool p)
    {
        paused = p;
        if (play_toggle) play_toggle->set_checked(!paused);
        std::printf("animation %s\n", paused ? "paused" : "playing");
        canvas->invalidate();
    };

    auto paint = [&](painter& p, size_f size)
    {
        last_size = size;
        const float scale = p.device_scale();
        p.rect({0, 0, size.w, size.h}, 0, background, std::nullopt);

        p.image(*card_image, {0, 0, 64, 48}, {padding, padding, 64 / scale, 48 / scale});
        auto [nearest, linear] = zoomed_rects(scale);
        p.image(*card_image, {0, 0, 64, 48}, nearest, filter::nearest);
        p.image(*card_image, {0, 0, 64, 48}, linear, filter::linear, 0.75f);
        p.text({padding, 64},
               "1:1, then nearest and linear (75%) at " + std::to_string(int(zoom * 100)) + "%",
               {color{220, 220, 220}});

        // Drawn play/pause button (clickable) and a busy arc.
        const rect_f b = play_rect();
        p.rect(b, 8, color{0, 0, 0, 160}, color{255, 255, 255, 80});
        if (paused)
            p.polygon({{b.x + 22, b.y + 16}, {b.x + 22, b.y + 44}, {b.x + 44, b.y + 30}},
                      color{255, 255, 255}, color{0, 0, 0}, 2);
        else
        {
            p.rect({b.x + 19, b.y + 16, 8, 28}, 1, color{255, 255, 255}, color{0, 0, 0});
            p.rect({b.x + 33, b.y + 16, 8, 28}, 1, color{255, 255, 255}, color{0, 0, 0});
        }
        const float spin = float(std::fmod(clock * 2, 2 * std::numbers::pi));
        p.arc({size.w - 50, padding + 30}, 18, spin, spin + 4.5f, color{120, 180, 255}, 4);

        const float cx = size.w / 2 + 120 * float(std::cos(clock));
        const float cy = size.h - 80 + 30 * float(std::sin(clock));
        p.image(*disc_image, {0, 0, 32, 32}, {cx - 16, cy - 16, 32, 32});

        p.push_clip({padding, size.h - 40, 120, 24});
        p.text({padding, size.h - 40}, "clipped text that runs past its box", {color{255, 210, 0}});
        p.pop_clip();
    };

    auto cursor_for = [&](point_f pos)
    {
        if (dragging) return cursor::grabbing;
        if (inside(play_rect(), pos)) return cursor::pointer;
        auto [a, b] = zoomed_rects(canvas->device_scale());
        if (inside(a, pos) || inside(b, pos)) return cursor::grab;
        return cursor::default_;
    };

    auto on_pointer = [&](const pointer_event& e)
    {
        using t = pointer_event_type;
        if (e.type != t::move || dragging)  // moves only while dragging, or the log floods
            std::printf("pointer %-7s button=%d clicks=%d mods=%s at %.0f,%.0f\n", type_name(e.type),
                        e.button, e.clicks, mods(e.modifiers).c_str(), e.pos.x, e.pos.y);
        if (e.type == t::press && e.button == 1)
        {
            if (inside(play_rect(), e.pos))
                set_paused(!paused);
            else if (cursor_for(e.pos) == cursor::grab)
            {
                dragging = true;
                drag_from = {e.pos.x - pan.x, e.pos.y - pan.y};
            }
        }
        else if (e.type == t::press && e.button == 2)
        {
            pan = {0, 0};
            set_zoom(4);
        }
        else if (e.type == t::release && e.button == 1)
            dragging = false;
        else if (e.type == t::move && dragging)
        {
            pan = {e.pos.x - drag_from.x, e.pos.y - drag_from.y};
            canvas->invalidate();
        }
        canvas->set_cursor(e.type == t::leave ? cursor::default_ : cursor_for(e.pos));
        return true;
    };

    auto on_scroll = [&](const scroll_event& e)
    {
        std::printf("scroll dx=%.2f dy=%.2f precise=%d mods=%s at %.0f,%.0f\n", e.dx, e.dy,
                    e.precise, mods(e.modifiers).c_str(), e.pos.x, e.pos.y);
        set_zoom(zoom * std::pow(1.15f, -e.dy));
        return true;
    };

    // Window-level keys work whatever has focus (toolbar, slider, canvas).
    auto on_key = [&](const key_event& e)
    {
        const std::string ch =
            e.key == key::character && e.codepoint < 128 ? std::string(1, char(e.codepoint)) : "";
        std::printf("key %s %d '%s' mods=%s repeat=%d\n", e.press ? "press  " : "release",
                    int(e.key), ch.c_str(), mods(e.modifiers).c_str(), e.repeat);
        if (!e.press) return false;
        switch (e.key)
        {
        case key::escape: (*ui)->quit(); return true;
        case key::f11: win->set_fullscreen(!win->fullscreen()); return true;
        case key::left: pan.x -= 10; break;
        case key::right: pan.x += 10; break;
        case key::up: pan.y -= 10; break;
        case key::down: pan.y += 10; break;
        case key::character:
            if (e.codepoint == 'c' && (e.modifiers & mod_ctrl))
            {
                (*ui)->set_clipboard_image(64, 48, pixel_format::bgra8_premultiplied, card.data(),
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

    using namespace rap::layout;
    win = (*ui)->create(layout::window{
        "rapport canvas",
        {vbox{
            layout::toolbar{
                layout::tool_button{play_pause_icon(paused), "Play / pause"}
                    .toggle(true)
                    .on_toggled([&](bool on) { set_paused(!on); })
                    .bind(play_toggle),
                separator{},
                layout::tool_button{magnifier(false), "Zoom out"}.on_click(
                    [&] { set_zoom(zoom / 1.25f); }),
                layout::tool_button{magnifier(true), "Zoom in"}.on_click([&]
                                                                       { set_zoom(zoom * 1.25f); }),
                layout::tool_button{icon::named("zoom-original"), "Reset"}.on_click(
                    [&]
                    {
                        pan = {0, 0};
                        set_zoom(4);
                    }),
                spacer{},
                layout::label{"Zoom"},
                layout::slider{0.25, 32, zoom}
                    .step(0.25, 1)
                    .mark(1, "1×")
                    .mark(4)
                    .mark(16, "16×")
                    .on_changed([&](double v) { set_zoom(float(v)); })
                    .bind(zoom_slider)
                    .width(20_ch),
                layout::label{"Padding"},
                layout::spin{0, 64, padding}.on_changed(
                    [&](double v)
                    {
                        padding = float(v);
                        canvas->invalidate();
                    }),
                layout::color_button{background, "Background"}.on_changed(
                    [&](color c)
                    {
                        background = c;
                        std::printf("background %d,%d,%d\n", c.r, c.g, c.b);
                        canvas->invalidate();
                    }),
            }
                .handle(toolbar),
            layout::canvas{}
                .on_paint(paint)
                .on_pointer(on_pointer)
                .on_scroll(on_scroll)
                .bind(canvas)
                .stretch(),
            hbox{
                layout::label{"starting"}.bind(status).stretch(),
                layout::button{"Copy image"}
                    .icon(icon::named("edit-copy"))
                    .on_click(
                        [&]
                        {
                            (*ui)->set_clipboard_image(64, 48, pixel_format::bgra8_premultiplied,
                                                     card.data(), 64 * 4);
                            std::printf("copied the test card\n");
                        }),
                layout::button{"Fullscreen"}
                    .icon(icon::named("view-fullscreen"))
                    .icon_only()
                    .on_click([&] { win->set_fullscreen(!win->fullscreen()); }),
            }
                .handle(bottom_bar),
        }}}.size(80_ch, 34_lines));
    win->on_close_request(
        [&]
        {
            (*ui)->quit();
            return true;
        });
    win->on_key(on_key);
    win->on_fullscreen_changed(
        [&](bool fs)
        {
            std::printf("fullscreen %s\n", fs ? "on" : "off");
            toolbar->set_visible(!fs);
            bottom_bar->set_visible(!fs);
        });
    std::printf("handles %s\n", toolbar && bottom_bar ? "filled" : "MISSING");
    win->set_icon(magnifier(true));
    play_toggle->set_checked(true);

    // The frame clock drives the orbit; paused time doesn't advance it.
    auto frame = canvas->on_frame(
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
            const size_f ws = win->size();
            const rect_f wa = win->work_area();
            const auto pos = win->position();
            const std::string where =
                pos ? " at " + std::to_string(int(pos->x)) + "," + std::to_string(int(pos->y))
                    : " (position unknown)";
            char buf[200];
            std::snprintf(buf, sizeof buf,
                          "%d frames/s, scale %.0f, window %.0f×%.0f%s, work area %.0f×%.0f",
                          frames, canvas->device_scale(), ws.w, ws.h, where.c_str(), wa.w, wa.h);
            status->set_text(buf);
            frames = 0;
        });

    win->show();
    canvas->focus();  // keys go to the view, not the first toolbar button
    return (*ui)->run();
}
