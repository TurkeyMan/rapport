// A splitter's sizes(), read from a pane's own size callback (a canvas's
// on_resize), are the sizes being laid out, every pane's, not the layout
// before (widgets.h, splitter::sizes; #105). Three canvases in a splitter:
// each one's on_resize reads all the sizes; its own must be the size it's
// told, and the others must be where they're laid out once the layout's
// done. After set_sizes, and after the window's resized.
//   rapport-splitter-sizes-in-resize-test   RAPPORT_BACKEND=gtk3|gtk4|qt6
// (GTK on Broadway, Qt offscreen: offscreen.sh or CI.)
#include <rapport/rapport.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}

std::string joined(const std::vector<float>& v)
{
    std::string s;
    for (float f : v) s += (s.empty() ? "" : ",") + std::to_string(int(std::lround(f)));
    return s.empty() ? "none" : s;
}

// What each pane's on_resize saw, in one layout: its size along the split,
// and every pane's sizes() then.
struct seen
{
    size_t pane;
    float told;
    std::vector<float> sizes;
};

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::printf("no backend: %s\n", ui.error().c_str());
        return 77;
    }
    using namespace rap::layout;
    rap::splitter* split = nullptr;
    std::vector<seen> calls;
    // What the first pane's on_resize does, once, after reading the sizes;
    // how deep on_resize calls went (one inside another's: a delivery
    // inside a delivery); calls after the window's gone.
    std::function<void()> then;
    int depth = 0, deepest = 0, after_gone = 0;
    size_t set_at = size_t(-1);  // the call that set new sizes
    bool gone = false;
    auto pane = [&](size_t i)
    {
        return canvas{}
            .on_resize(
                [&, i](rap::size_f s)
                {
                    if (gone) ++after_gone;
                    if (!split) return;
                    ++depth;
                    deepest = std::max(deepest, depth);
                    calls.push_back({i, s.w, split->sizes()});
                    if (i == 0 && then)
                    {
                        auto f = std::move(then);
                        then = nullptr;
                        f();
                    }
                    --depth;
                })
            .stretch();
    };
    auto win = (*ui)->create(window{
        "Splitter sizes in on_resize",
        {splitter{rap::orientation::horizontal, pane(0), pane(1), pane(2)}.bind(split).stretch()}}
                                 .size(600_px, 300_px));
    if (!split) return 2;
    win->show();

    // The calls come a layout at a time (one delivery: each pane told once,
    // its latest), and a layout may come in more than one pass. In each
    // delivery every pane reads the same sizes, the layout being told, each
    // its own the size it's told; the last delivery's are where the layout
    // ended. An on_resize's set_sizes (the call at `set_at`) asks for a new
    // layout: sizes() is what it asked for until that's laid out
    // (splitter::sizes), so the rest of that delivery doesn't count.
    auto check = [&](const std::string& what, size_t set_at = size_t(-1))
    {
        const std::vector<float> final = split->sizes();
        auto near = [](const std::vector<float>& a, const std::vector<float>& b)
        {
            if (a.size() != b.size()) return false;
            for (size_t j = 0; j < a.size(); ++j)
                if (std::abs(a[j] - b[j]) >= 1.5f) return false;
            return true;
        };
        auto said = [](const seen& c)
        {
            return "pane " + std::to_string(c.pane) + " told " +
                   std::to_string(int(std::lround(c.told))) + ", read " + joined(c.sizes);
        };
        int own = 0, mixed = 0, deliveries = 0;
        std::string first_own, first_mixed;
        std::vector<float> last;  // the last delivery's layout
        for (size_t i = 0; i < calls.size();)
        {
            // One delivery: up to a pane told again.
            size_t end = i;
            std::vector<bool> told(3, false);
            while (end < calls.size() && !told[calls[end].pane]) told[calls[end++].pane] = true;
            const size_t counted = set_at >= i && set_at < end ? set_at + 1 : end;
            for (size_t k = i; k < counted; ++k)
            {
                const seen& c = calls[k];
                if (c.sizes.size() != 3 || std::abs(c.sizes[c.pane] - c.told) >= 1.5f)
                    if (!own++) first_own = "; first: " + said(c);
                if (!near(c.sizes, calls[i].sizes))
                    if (!mixed++) first_mixed = "; first: " + said(calls[i]) + ", then " + said(c);
            }
            if (counted == end) last = calls[i].sizes;
            ++deliveries;
            i = end;
        }
        if (std::getenv("RAPPORT_TEST_VERBOSE"))
            for (const seen& c : calls) std::printf("     %s\n", said(c).c_str());
        expect(!calls.empty(), what + ": the panes were told their sizes (" +
                                   std::to_string(calls.size()) + " on_resize calls, " +
                                   std::to_string(deliveries) + " layouts)");
        expect(own == 0, what + ": each on_resize's own size in sizes() is the size it's told (" +
                             std::to_string(own) + " not" + first_own + ")");
        expect(mixed == 0,
               what +
                   ": in each layout every pane's on_resize reads the same sizes, that layout's (" +
                   std::to_string(mixed) + " not" + first_mixed + ")");
        expect(near(last, final), what + ": the last layout told is where it ended, " +
                                      joined(final) + " (told " + joined(last) + ")");
    };

    // The steps, each once the last has been laid out and delivered.
    // (One subscription each: none is dropped from inside its own timer.)
    rap::subscription steps[5];
    auto after = [&](int i, std::function<void()> f)
    { steps[i] = (*ui)->timer(400, false, std::move(f)); };
    steps[0] = (*ui)->timer(
        500, false,
        [&]
        {
            calls.clear();
            split->set_sizes({120, 300, 0});
            after(
                1,
                [&]
                {
                    check("after set_sizes");
                    calls.clear();
                    win->resize({800, 300});
                    after(
                        2,
                        [&]
                        {
                            check("after a resize");
                            // The first pane's on_resize sets new sizes: the
                            // backend's usual path, no delivery inside this one.
                            calls.clear();
                            deepest = 0;
                            then = [&]
                            {
                                set_at = calls.size() - 1;
                                split->set_sizes({200, 250, 0});
                            };
                            win->resize({700, 300});
                            after(
                                3,
                                [&]
                                {
                                    const auto s = split->sizes();
                                    expect(!then,
                                           "a resize: the first pane's on_resize set new sizes");
                                    check("its on_resize setting new sizes", set_at);
                                    // The first divider: GTK 3 moves a
                                    // chain's later ones as an earlier
                                    // one moves (set_sizes alone, too).
                                    expect(s.size() == 3 && std::abs(s[0] - 200) < 1.5f,
                                           "and they were laid out: " + joined(s) +
                                               ", the first 200");
                                    expect(deepest == 1, "no on_resize inside another (" +
                                                             std::to_string(deepest) + " deep)");
                                    // The first pane's on_resize destroys the
                                    // window: the others, waiting, aren't told.
                                    then = [&]
                                    {
                                        split = nullptr;
                                        win.reset();
                                        gone = true;
                                    };
                                    win->resize({650, 300});
                                    after(
                                        4,
                                        [&]
                                        {
                                            expect(
                                                gone,
                                                "the first pane's on_resize destroyed the window");
                                            expect(after_gone == 0, "the panes waiting weren't "
                                                                    "told after it was gone (" +
                                                                        std::to_string(after_gone) +
                                                                        ")");
                                            std::printf(fails ? "%d failed\n" : "all passed\n",
                                                        fails);
                                            (*ui)->quit(fails ? 1 : 0);
                                        });
                                });
                        });
                });
        });
    return (*ui)->run();
}
