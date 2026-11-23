// backends/common/pane_sizes.h, the queue a splitter's panes wait in to be
// told their sizes (#105, #138), driven directly: no toolkit.
// - A placing_scope tells what waited as the outermost ends, not an inner one.
// - Placed, then told later (GTK: once its own layout is done; held here
//   with placing_held): a pane placed again meanwhile, outside any placing,
//   waits behind what's already waiting, its latest only, so no pane is
//   told its older size after its newer one, or out of order; one never
//   placed by a splitter, likewise.
// - A pane told within a delivery, outside any placing, is told by that
//   delivery, after the others: never a delivery inside one.
// - One whose alive token is gone, or false, isn't told.
#include "pane_sizes.h"

#include <algorithm>
#include <cstdio>
#include <memory>
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

struct pane
{
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

std::string told;  // each telling, "A1 B1 ..."

void tell(const pane& p, const std::string& what)
{
    rap::pane_sizes::now_or_after(&p, p.alive,
                                  [what] { told += (told.empty() ? "" : " ") + what; });
}

}  // namespace

int main()
{
    namespace ps = rap::pane_sizes;
    pane a, b, c;

    // Outside any placing: at once.
    told.clear();
    tell(a, "A1");
    expect(told == "A1", "outside any placing, told at once (" + told + ")");

    // Nested placing: told as the outermost ends.
    told.clear();
    {
        ps::placing_scope outer;
        tell(a, "A1");
        {
            ps::placing_scope inner;
            tell(b, "B1");
        }
        expect(told.empty(), "an inner splitter placed: nothing told yet (" + told + ")");
        tell(c, "C1");
    }
    expect(told == "A1 B1 C1",
           "the outermost placed: all told, in the order placed (" + told + ")");

    // Placed, told later (GTK); meanwhile A placed again outside any placing,
    // and C, never a splitter's, resized.
    told.clear();
    {
        ps::placing_held held;
        tell(a, "A1");
        tell(b, "B1");
    }
    expect(told.empty(), "placed, to be told later: nothing told yet (" + told + ")");
    tell(a, "A2");
    tell(c, "C1");
    expect(told.empty(),
           "a pane placed again, and another resized, while some wait: they wait behind them (" +
               told + ")");
    ps::deliver();
    expect(told == "A2 B1 C1",
           "told once, in order, the first pane its latest size only (" + told + ")");
    expect(ps::queue.empty() && ps::next == 0, "nothing left waiting");

    // Within a delivery: what a callback resizes, outside any placing, is
    // told by that delivery, after the rest; no delivery inside it.
    told.clear();
    int deepest = 0, depth = 0;
    {
        ps::placing_scope scope;
        ps::now_or_after(&a, a.alive,
                         [&]
                         {
                             deepest = std::max(deepest, ++depth);
                             told += "A1";
                             tell(c, "C1");  // its callback resizes another
                             tell(a, "A2");  // and itself again
                             --depth;
                         });
        tell(b, "B1");
    }
    expect(told == "A1 B1 C1 A2",
           "a callback's resizes told after the rest, by the same delivery (" + told + ")");
    expect(deepest == 1, "no telling inside another (" + std::to_string(deepest) + " deep)");

    // Gone, or marked gone: not told.
    told.clear();
    {
        auto gone = std::make_unique<pane>();
        ps::placing_scope scope;
        tell(a, "A1");
        tell(*gone, "G1");
        tell(b, "B1");
        *b.alive = false;  // being destroyed
        gone.reset();
    }
    expect(told == "A1", "a pane gone, or being destroyed, isn't told (" + told + ")");

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
