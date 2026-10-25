// rapport-frame: exercises canvas::present, a frame under the canvas's
// painting. The frame is made at exactly target()'s size each time it
// changes: a one-pixel checker in the top left (1:1, or it shimmers into
// grey), a one-pixel red border round the edge (all four sides visible, or
// the size is off), and a gradient. The overlay is painted over it. Keys:
// C clears the frame (back to on_paint alone) and shows it again; Esc quits.
#include "rapport/rapport.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace rap;

namespace
{

std::vector<uint8_t> make_frame(int w, int h)
{
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];  // BGRA
            const bool edge = x == 0 || y == 0 || x == w - 1 || y == h - 1;
            const bool checker = x < 128 && y < 128;
            if (edge)
            {
                p[0] = 0, p[1] = 0, p[2] = 255;
            }
            else if (checker)
            {
                const uint8_t v = (x ^ y) & 1 ? 255 : 0;
                p[0] = p[1] = p[2] = v;
            }
            else
            {
                p[0] = uint8_t(255 * y / std::max(1, h - 1));
                p[1] = 96;
                p[2] = uint8_t(255 * x / std::max(1, w - 1));
            }
            p[3] = 255;
        }
    return px;
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

    rap::canvas* canvas = nullptr;
    std::unique_ptr<rap::window> win;
    bool shown = true;
    int presented = 0;

    auto present = [&]
    {
        const frame_target t = canvas->target();
        if (!shown || t.width <= 0 || t.height <= 0) return;
        const auto px = make_frame(t.width, t.height);
        std::shared_ptr<const image> frame =
            (*ui)->create_image(t.width, t.height, pixel_format::bgra8_premultiplied, px.data(), size_t(t.width) * 4);
        canvas->present(std::move(frame));
        ++presented;
    };

    using namespace rap::layout;
    win = (*ui)->create(layout::window{
        "rapport frame",
        {vbox{
            layout::canvas{}
                .on_paint(
                    [&](painter& p, size_f size)
                    {
                        const frame_target t = canvas->target();
                        char buf[200];
                        std::snprintf(buf, sizeof buf, "%s: target %dx%d at %.3g, canvas %.1fx%.1f, device %.3g, #%d",
                                      shown ? "frame" : "no frame", t.width, t.height, double(t.scale),
                                      double(size.w), double(size.h), double(canvas->device_scale()), presented);
                        p.rect({140, 20, 520, 28}, 4, color{0, 0, 0, 160}, std::nullopt);
                        p.text({150, 26}, buf, {.fg = color{255, 255, 255}});
                        p.rect({140, 60, 120, 60}, 0, std::nullopt, color{0, 255, 0});
                    })
                .on_key(
                    [&](const key_event& e)
                    {
                        if (!e.press) return false;
                        if (e.key == key::escape) return (*ui)->quit(), true;
                        if (e.key == key::character && e.codepoint == U'c')
                        {
                            shown = !shown;
                            if (shown)
                                present();
                            else
                                canvas->clear_frame();
                            return true;
                        }
                        return false;
                    })
                .bind(canvas)
                .stretch(),
        }},
    });
    win->resize({720, 480});
    canvas->on_frame_target([&](const frame_target&) { present(); });
    win->show();
    canvas->focus();
    present();
    return (*ui)->run();
}
