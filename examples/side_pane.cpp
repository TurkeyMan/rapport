// rapport-side-pane: a side pane beside a list, as an app shows a pull
// request beside its commits (pengit's log), for checking how a window lays
// out and paints:
//   - Drag the bar between the columns: nothing flickers, and no pixels are
//     left behind where the bar or a pane's edge was.
//   - A picture (a circle in a square) beside the text at top left, in a box
//     96 px square that's laid out shorter than that: it shrinks whole,
//     never squashed out of shape.
//   - The side pane scrolls. "More..." swaps its five-line description for a
//     sixteen-line one, taller than the pane, so its scroll bar shows: the
//     rows stay inside its width (the list's own scroll bar and the "Hide"
//     link at the Build row's end in view), with no need to drag the bar.
//   rapport-side-pane --more    presses "More..." after a second
//   rapport-side-pane --tall    gives the picture its whole 96 px
//   rapport-side-pane --thin    thin dividers (splitter::thin), as
//                               Explorer's: a line on the lists' white
#include <rapport/rapport.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{

class rows_model : public rap::list_model
{
public:
    explicit rows_model(size_t n, std::string what)
        : n_(n)
        , what_(std::move(what))
    {
    }
    size_t row_count() const override { return n_; }
    std::string text(size_t row, size_t col) const override
    {
        if (col == 0) return what_ + " " + std::to_string(row + 1);
        return row % 4 ? "passed" : "failed after 3 minutes";
    }

private:
    size_t n_;
    std::string what_;
};

constexpr const char* description =
    "This pull request's description runs on: what changed, why, the issue it fixes, how it was "
    "tested, and what's left for later.\n\nIt has paragraphs, too, more than the short view "
    "holds, so More... shows a taller view of it.\n\nLine after line after line, until the "
    "taller view makes the pane scroll.\n\nAnd a little more.\n\nAnd more still, to be sure.";

// A circle in a square: squashed, it's an ellipse.
std::vector<uint8_t> picture(int n)
{
    std::vector<uint8_t> px(size_t(n) * n * 4);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            const float dx = x + 0.5f - n / 2.f, dy = y + 0.5f - n / 2.f;
            const bool in = std::sqrt(dx * dx + dy * dy) < n * 0.42f;
            uint8_t* p = &px[(size_t(y) * n + x) * 4];
            p[0] = in ? 220 : 40;
            p[1] = in ? 120 : 110;
            p[2] = in ? 30 : 200;
            p[3] = 255;
        }
    return px;
}

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 1;
    }
    bool more = false, tall = false, thin = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--more") more = true;
        if (std::string_view(argv[i]) == "--tall") tall = true;
        if (std::string_view(argv[i]) == "--thin") thin = true;
    }

    rows_model commits(200, "Commit"), checks(14, "Job");
    const int n = 96;
    const std::vector<uint8_t> pixels = picture(n);
    auto image = (*ui)->create_image(n, n, rap::pixel_format::rgba8, pixels.data(), size_t(n) * 4);

    rap::text_view* details = nullptr;
    rap::text_view* body = nullptr;
    rap::text_view* body_all = nullptr;
    rap::label* more_link = nullptr;
    rap::list* checks_view = nullptr;
    auto press_more = [&]
    {
        body->set_visible(false);
        body_all->set_visible(true);
        more_link->set_visible(false);
    };

    using namespace rap::layout;
    auto win = (*ui)->create(window{
        "Side pane",
        {splitter{
            horizontal,
            list{{{"Commit", {}, true}, {"Status", 20_ch}}, &commits}.stretch(3),
            splitter{
                horizontal,
                splitter{
                    vertical,
                    hbox{
                        text_view{}.read_only().bind(details).stretch(),
                        vbox{
                            canvas{}
                                .focusable(false)
                                .on_paint(
                                    [&](rap::painter& p, rap::size_f size)
                                    {
                                        // The box, outlined, and the picture
                                        // in it, whole.
                                        p.rect({0, 0, size.w, size.h}, 0, std::nullopt,
                                               rap::paint{rap::theme_color::window_text});
                                        p.image(*image, {0, 0, float(n), float(n)},
                                                rap::fit_inside({0, 0, size.w, size.h}, float(n),
                                                                float(n), 1, 0));
                                    })
                                .width(rap::extent{float(n), rap::extent::unit::px})
                                .height(rap::extent{float(n), rap::extent::unit::px}),
                            spacer{},
                        },
                    }
                        .height(tall ? 9_lines : 3_lines),
                    list{{{"Path", {}, true}}, &commits}.height(4_lines).stretch(2),
                }
                    .thin(thin)
                    .width(30_ch)
                    .stretch(3),
                vbox{
                    scroll{
                        vbox{
                            label{"Wrap long lines in the side pane, the pull request's title"}
                                .wrap()
                                .selectable(),
                            label{"TurkeyMan"}.selectable(),
                            label{"No reviews yet"}.wrap().selectable(),
                            text_view{}.read_only().compact().bind(body).height(5_lines),
                            text_view{}.read_only().compact().visible(false).bind(body_all).height(
                                16_lines),
                            label{"More..."}.link(press_more).bind(more_link),
                            hbox{button{"Merge"}, button{"Close"}, spacer{}},
                            separator{},
                            hbox{
                                label{"Build: 13 of 14 jobs passed, one failed after three "
                                      "minutes"}
                                    .ellipsize()
                                    .stretch(),
                                label{"Hide"}.link([&] { checks_view->set_visible(false); }),
                            },
                            list{{{"Job", {}, true}, {"Result", 16_ch}}, &checks}
                                .headers(false)
                                .compact()
                                .bind(checks_view)
                                .height(10_lines),
                            spacer{},
                        }}
                        .stretch(),
                }
                    .width(30_ch),
            }
                .thin(thin)
                .stretch(2),
        }
             .thin(thin)}}.size(150_ch, 36_lines));
    details->set_text("The commit's message, beside its author's picture (a circle in a square).",
                      {});
    body->set_text(description, {});
    body_all->set_text(description, {});

    std::vector<rap::subscription> timers;
    if (more) timers.push_back((*ui)->timer(1000, false, press_more));

    win->on_close_request(
        [&]
        {
            (*ui)->quit();
            return true;
        });
    win->show();
    return (*ui)->run();
}
