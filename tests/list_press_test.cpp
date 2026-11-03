// A press on a draggable list's row (backends/common/list_press.h): what a
// click and a drag do to the selection, with and without Ctrl and Shift.
//   rapport-list-press-test
#include "list_press.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{

int fails = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

using rows = std::vector<size_t>;
namespace lp = rap::list_press;

// The selection after a press on `row` of `sel`, let go (or dragged).
rows clicked(const rows& sel, size_t row, bool shift, bool ctrl)
{
    auto plan = lp::plan_press(shift, ctrl, std::ranges::contains(sel, row), sel.size());
    return plan ? lp::after_click(sel, row, plan->on_click) : sel;
}
rows dragged(const rows& sel, size_t row, bool ctrl)
{
    auto plan = lp::plan_press(false, ctrl, std::ranges::contains(sel, row), sel.size());
    return plan ? lp::after_drag(sel, row, plan->on_drag) : sel;
}

}  // namespace

int main()
{
    // Ctrl: a possible drag, held back from the toolkit.
    auto ctrl_in = lp::plan_press(false, true, true, 3);
    check(ctrl_in && ctrl_in->defer && ctrl_in->on_click == lp::click::toggle &&
              ctrl_in->on_drag == lp::drag::keep,
          "Ctrl on a selected row: held back, a toggle let go, the selection dragged");
    auto ctrl_out = lp::plan_press(false, true, false, 2);
    check(ctrl_out && ctrl_out->defer && ctrl_out->on_drag == lp::drag::add,
          "Ctrl on an unselected row: held back, added when it drags");
    check(!lp::plan_press(true, false, false, 1) && !lp::plan_press(true, true, true, 2),
          "Shift (with Ctrl or not): the toolkit's range, no drag");
    auto plain_multi = lp::plan_press(false, false, true, 2);
    check(plain_multi && plain_multi->defer && plain_multi->on_click == lp::click::select_only,
          "plain in a multi-selection: held back, the row alone let go");
    auto plain = lp::plan_press(false, false, false, 2);
    check(plain && !plain->defer && plain->on_click == lp::click::native &&
              plain->on_drag == lp::drag::select_only,
          "plain elsewhere: the toolkit's click, that row alone dragged");

    // Ctrl+click toggles at the release; Ctrl+drag carries the selection.
    check(clicked({1, 3}, 3, false, true) == rows{1}, "Ctrl+click on a selected row unselects it");
    check(clicked({1, 3}, 2, false, true) == rows{1, 2, 3},
          "Ctrl+click on an unselected row adds it");
    check(dragged({1, 3}, 3, true) == rows{1, 3},
          "Ctrl+drag from a selected row drags the selection");
    check(dragged({1, 3}, 5, true) == rows{1, 3, 5}, "Ctrl+drag from an unselected row adds it");
    check(dragged({1, 3}, 5, false) == rows{5},
          "a plain drag from an unselected row drags it alone");
    check(dragged({1, 3}, 1, false) == rows{1, 3},
          "a plain drag inside the selection drags it whole");
    check(clicked({1, 3}, 1, false, false) == rows{1},
          "a plain click inside the selection selects that row");

    // The threshold: either axis, as the toolkits measure it.
    check(!lp::past_threshold(7, -7, 8) && lp::past_threshold(-8, 0, 8) &&
              lp::past_threshold(0, 9, 8),
          "past the threshold on either axis");

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
