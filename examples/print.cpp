// A test page through the platform's print dialog: text, theme colours (on
// paper, the print palette), shapes and an image, centred on the page.
//   RAPPORT_PRINT_TO=page.png ./rapport-print   no dialog: the pages as PNGs
#include <rapport/rapport.h>

#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    auto loaded = rap::ui::load(argc, argv);
    if (!loaded)
    {
        std::fprintf(stderr, "%s\n", loaded.error().c_str());
        return 1;
    }
    rap::ui& ui = **loaded;

    // A 64x64 gradient, to see images reach the paper.
    std::vector<uint8_t> px(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            uint8_t* p = &px[(y * 64 + x) * 4];
            p[0] = uint8_t(x * 4), p[1] = uint8_t(y * 4), p[2] = 160, p[3] = 255;
        }
    std::unique_ptr<rap::image> gradient =
        ui.create_image(64, 64, rap::pixel_format::rgba8, px.data(), 64 * 4);

    rap::print_job job;
    job.title = "rapport test page";
    job.pages = 2;
    job.draw_page = [&](rap::painter& p, int page, rap::size_f area)
    {
        const rap::size_f s = area;
        p.rect({0, 0, s.w, s.h}, 0, std::nullopt, rap::paint{rap::theme_color::mid});
        rap::text_style title;
        title.size = 18;
        title.bold = true;
        p.text({36, 36}, "rapport test page " + std::to_string(page + 1), title);
        rap::text_style body;
        p.text({36, 64}, "Theme text on paper: black, whatever the screen's theme.", body);
        body.fg = rap::paint{rap::theme_color::link};
        p.text({36, 82}, "A link's colour.", body);
        p.rect({36, 110, 120, 40}, 6, rap::paint{rap::theme_color::positive}, std::nullopt);
        p.rect({170, 110, 120, 40}, 6, rap::paint{rap::theme_color::negative}, std::nullopt);
        p.circle({s.w / 2, s.h / 2}, 72, std::nullopt, rap::paint{rap::theme_color::window_text});
        p.line({0, 0}, {s.w, s.h}, rap::paint{rap::theme_color::mid}, 0.5f);
        p.image(*gradient, {0, 0, 64, 64}, {s.w / 2 - 72, s.h / 2 + 100, 144, 144});
    };
    int code = 0;
    job.done = [&](bool printed, std::string error)
    {
        std::printf("%s%s\n",
                    printed         ? "printed"
                    : error.empty() ? "cancelled"
                                    : "failed: ",
                    error.c_str());
        code = printed ? 0 : 1;
        ui.quit(code);
    };
    ui.print(nullptr, std::move(job));
    return ui.run();
}
