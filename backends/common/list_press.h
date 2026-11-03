// A primary-button press on a row of a list whose rows drag (on_reorder,
// drag_out): what it does if it's let go where it was, and what it does if
// it moves past the drag threshold. One rule for the backends that track
// the press by hand (gtk3, gtk4, qt6), as file managers have it, and as
// win32's list view does natively:
//  - Shift: a range selection, never a drag.
//  - Ctrl: a possible drag. Moved, the rows drag (with the pressed row
//    added to the selection) and Ctrl at the drop makes it a copy; let go,
//    it's the Ctrl+click toggle of that row, at the release.
//  - Plain, inside a multi-selection: the selection drags whole; let go,
//    only that row is selected, at the release.
//  - Plain, elsewhere: the toolkit's own click; a drag takes that row alone.
// No toolkit here, so tests/list_press_test.cpp can check it.
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace rap::list_press
{

// Let go without moving.
enum class click
{
    native,       // the toolkit's own click has it
    select_only,  // that row alone selected, and current
    toggle,       // that row's selection flipped, and current
};
// Past the threshold, before the rows drag.
enum class drag
{
    keep,         // the selection as it is
    select_only,  // that row alone
    add,          // that row added to the selection
};
struct plan
{
    bool defer =
        false;  // the press is held back from the toolkit (which would change the selection)
    click on_click = click::native;
    drag on_drag = drag::keep;
};

// The plan for a press on a row; nullopt: no drag, the toolkit's click.
inline std::optional<plan> plan_press(bool shift, bool ctrl, bool row_selected,
                                      size_t selected_count)
{
    if (shift) return std::nullopt;
    if (ctrl) return plan{true, click::toggle, row_selected ? drag::keep : drag::add};
    if (row_selected && selected_count > 1) return plan{true, click::select_only, drag::keep};
    return plan{false, click::native, row_selected ? drag::keep : drag::select_only};
}

// Whether the pointer, (dx, dy) from the press, is past the threshold.
inline bool past_threshold(double dx, double dy, int threshold)
{
    return std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy) >= threshold;
}

// The selection (sorted rows) after `row`'s press ends in `c` or `d`, for
// the backends to apply as their toolkit allows, and for the tests.
inline std::vector<size_t> after_click(std::vector<size_t> sel, size_t row, click c)
{
    auto at = std::lower_bound(sel.begin(), sel.end(), row);
    const bool in = at != sel.end() && *at == row;
    switch (c)
    {
    case click::native: break;
    case click::select_only: return {row};
    case click::toggle:
        if (in)
            sel.erase(at);
        else
            sel.insert(at, row);
        break;
    }
    return sel;
}
inline std::vector<size_t> after_drag(std::vector<size_t> sel, size_t row, drag d)
{
    auto at = std::lower_bound(sel.begin(), sel.end(), row);
    const bool in = at != sel.end() && *at == row;
    switch (d)
    {
    case drag::keep: break;
    case drag::select_only: return {row};
    case drag::add:
        if (!in) sel.insert(at, row);
        break;
    }
    return sel;
}

}  // namespace rap::list_press
