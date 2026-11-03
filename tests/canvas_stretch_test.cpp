// A canvas whose size changes in a burst, through a real backend
// (canvas::stretch_while_resizing): its splitter's divider moved on a timer
// as a drag would, the canvas shows its last frame stretched rather than painting at
// each size, hears of each size, and once the changes stop is told so and
// paints anew. Off the desktop on Linux (offscreen.sh), with
// RAPPORT_NO_ACTIVATE=1 on Windows.
//   rapport-canvas-stretch-test [backend]
#include <rapport/rapport.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{

using clock_type = std::chrono::steady_clock;

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
}

long long ms_between(clock_type::time_point a, clock_type::time_point b)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

struct state
{
    int paints = 0, resizes = 0, settles = 0;
    rap::size_f painted, resized, settled;
    clock_type::time_point settled_at;
};

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options opts;
    if (argc > 1) opts.backend = argv[1];
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 2;
    }
    state st;
    rap::canvas* canvas = nullptr;
    rap::splitter* split = nullptr;
    using namespace rap::layout;
    auto win = (*ui)->create(window{
        "Canvas stretch",
        {splitter{horizontal,
                  rap::layout::canvas{}
             .on_paint(
                 [&](rap::painter& p, rap::size_f size)
                 {
                     ++st.paints;
                     st.painted = size;
                     p.rect({0, 0, size.w, size.h}, 0, rap::color{50, 100, 200, 255}, std::nullopt);
                 })
             .on_resize(
                 [&](rap::size_f size)
                 {
                     ++st.resizes;
                     st.resized = size;
                 })
             .on_resize_settled(
                 [&](rap::size_f size)
                 {
                     ++st.settles;
                     st.settled = size;
                     st.settled_at = clock_type::now();
                 })
             .stretch_while_resizing(rap::resize_stretch::uniform, rap::color{0, 0, 0, 255})
             .bind(canvas),
                  label{"Info"}}
             .bind(split)}}
                                 .margins(false));
    win->resize({480, 240});
    split->set_sizes({240, 240});
    win->show();

    // Shown and painted, and the first size settled, before the drag.
    rap::subscription waiting, steps, after;
    int waited = 0, step = 0;
    int paints_before = 0, resizes_before = 0, settles_before = 0;
    clock_type::time_point last_step;
    const auto finish = [&]
    {
        std::printf(fails ? "%d failed\n" : "all passed\n", fails);
        std::fflush(stdout);
        (*ui)->quit(fails ? 1 : 0);
    };

    // Then a canvas that turns stretching off from its own on_paint, while
    // it paints, and closes its own window from on_resize_settled at the end
    // of a stretched burst: neither may touch what's gone.
    std::unique_ptr<rap::window> win2;
    rap::canvas* canvas2 = nullptr;
    rap::splitter* split2 = nullptr;
    int paints2 = 0, settles2 = 0, waited2 = 0, step2 = 0, before2 = 0;
    bool off_in_paint = false, close_on_settle = false, closed = false;
    rap::subscription waiting2, check2, steps2, end2;
    const auto second = [&]
    {
        win2 = (*ui)->create(window{
            "Canvas stretch, closed",
            {splitter{horizontal,
                      rap::layout::canvas{}
                          .on_paint(
                              [&](rap::painter& p, rap::size_f size)
                              {
                                  ++paints2;
                                  if (off_in_paint)
                                  {
                                      off_in_paint = false;
                                      canvas2->stretch_while_resizing(rap::resize_stretch::none);
                                  }
                                  p.rect({0, 0, size.w, size.h}, 0, rap::color{200, 100, 50, 255}, std::nullopt);
                              })
                          .on_resize_settled(
                              [&](rap::size_f)
                              {
                                  ++settles2;
                                  if (!close_on_settle) return;
                                  close_on_settle = false;
                                  closed = true;
                                  win2.reset();  // the canvas with it
                              })
                          .stretch_while_resizing(rap::resize_stretch::fill)
                          .bind(canvas2),
                      label{"Info"}}
                 .bind(split2)}}
                                  .margins(false));
        win2->resize({480, 240});
        split2->set_sizes({240, 240});
        win2->show();
        waiting2 = (*ui)->timer(
            50, true,
            [&]
            {
                if (++waited2 < 40 && (paints2 == 0 || settles2 == 0)) return;
                waiting2 = {};
                before2 = paints2;
                off_in_paint = true;
                canvas2->invalidate();
                check2 = (*ui)->timer(
                    250, false,
                    [&]
                    {
                        expect(paints2 > before2 && !off_in_paint, "stretching turned off from on_paint, as it paints");
                        canvas2->stretch_while_resizing(rap::resize_stretch::fill);
                        canvas2->invalidate();
                        close_on_settle = true;
                        steps2 = (*ui)->timer(
                            30, true,
                            [&]
                            {
                                if (closed || ++step2 > 4)
                                {
                                    steps2 = {};
                                    return;
                                }
                                std::vector<float> sizes = split2->sizes();
                                const float total = sizes.size() == 2 ? sizes[0] + sizes[1] : 400;
                                const float left = 200.f + 20.f * float(step2);
                                split2->set_sizes({left, total - left});
                            });
                        end2 = (*ui)->timer(
                            800, false,
                            [&]
                            {
                                expect(closed && !win2, "the window closed from on_resize_settled, in a burst");
                                finish();
                            });
                    });
            });
    };
    waiting = (*ui)->timer(
        50, true,
        [&]
        {
            if (++waited < 40 && (st.paints == 0 || st.settles == 0)) return;
            waiting = {};
            expect(st.paints > 0, "the canvas painted before the resize (" + std::to_string(canvas->size().w) + " x " +
                                      std::to_string(canvas->size().h) + ")");
            if (st.paints == 0) return finish();
            expect(st.settles >= 1, "a canvas's first size settles");
            paints_before = st.paints, resizes_before = st.resizes, settles_before = st.settles;
            if (std::getenv("RAPPORT_TEST_VERBOSE"))
            {
                const auto sz = split->sizes();
                std::printf("     window %.0f x %.0f, panes %.0f + %.0f, canvas %.0f x %.0f\n", win->size().w,
                            win->size().h, sz.size() > 0 ? sz[0] : -1.f, sz.size() > 1 ? sz[1] : -1.f,
                            canvas->size().w, canvas->size().h);
            }
            // The divider at eight places 30 ms apart, as a drag gives them.
            steps = (*ui)->timer(
                30, true,
                [&]
                {
                    ++step;
                    std::vector<float> sizes = split->sizes();
                    const float total = sizes.size() == 2 ? sizes[0] + sizes[1] : 400;
                    const float left = 200.f + 15.f * float(step);
                    split->set_sizes({left, total - left});
                    last_step = clock_type::now();
                    if (step < 8) return;
                    steps = {};
                    after = (*ui)->timer(
                        600, false,
                        [&]
                        {
                            if (std::getenv("RAPPORT_TEST_VERBOSE"))
                            {
                                const auto sz = split->sizes();
                                std::printf("     after: panes %.0f + %.0f, canvas %.0f x %.0f, %d resizes\n",
                                            sz.size() > 0 ? sz[0] : -1.f, sz.size() > 1 ? sz[1] : -1.f,
                                            canvas->size().w, canvas->size().h, st.resizes - resizes_before);
                            }
                            // GTK 4 on Broadway with no viewer lays nothing
                            // out after its first frame: there's no burst
                            // to see.
                            if (st.resizes == resizes_before && split->sizes()[0] != canvas->size().w)
                            {
                                std::printf("skip: the splitter moved but nothing was laid out (GTK 4 off "
                                            "screen)\n");
                                return finish();
                            }
                            expect(st.resizes > resizes_before, "on_resize is called as the size changes");
                            expect(st.settles == settles_before + 1,
                                   "on_resize_settled is called once after the burst (" +
                                       std::to_string(st.settles - settles_before) + ")");
                            const long long late = ms_between(last_step, st.settled_at);
                            expect(late >= 100 && late < 500,
                                   "it's called once no change has come for a while (" + std::to_string(late) +
                                       " ms after the last)");
                            expect(st.settled.w == canvas->size().w && st.settled.h == canvas->size().h,
                                   "it's told the size the canvas has");
                            const int painted = st.paints - paints_before;
                            expect(painted >= 1 && painted <= 2,
                                   "the burst is shown stretched, painted anew only once it settles (" +
                                       std::to_string(painted) + " paints)");
                            expect(st.painted.w == canvas->size().w && st.painted.h == canvas->size().h,
                                   "the last paint is at the settled size");
                            second();
                        });
                });
        });
    return (*ui)->run();
}
