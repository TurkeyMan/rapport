// What a window's document tabs (rap::document_tabs) do, the same on every
// backend: the tabs' ids beside the native control's own items, the keys a
// browser has, which tab is current once one goes, and how wide the tabs
// are. No toolkit here, so tests/document_tabs_test.cpp can check it.
#pragma once

#include "rapport/widgets.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace rap::doc_tabs
{

// What a key does to the tabs.
struct action
{
    enum kind
    {
        none,
        new_tab,
        close,   // the current tab
        select,  // `index`
        move,    // the current tab, to `index` (where it is already at either end)
    };
    kind what = none;
    int index = -1;
};

// Ctrl+T, Ctrl+W, Ctrl+Tab and Ctrl+Shift+Tab (round), Ctrl+1..8 and Ctrl+9
// for the last, as browsers and file managers have them; Ctrl+Shift+PgUp and
// Ctrl+Shift+PgDn move the current tab left and right, as browsers', and are
// the tabs' even at either end (no round). Presses only.
inline action for_key(const key_event& e, int current, int count)
{
    if (!e.press || (e.modifiers & (mod_alt | mod_meta)) || !(e.modifiers & mod_ctrl)) return {};
    const bool shift = e.modifiers & mod_shift;
    if (shift && (e.key == key::page_up || e.key == key::page_down))
    {
        if (current < 0 || current >= count) return {};
        const int to = e.key == key::page_up ? std::max(current - 1, 0) : std::min(current + 1, count - 1);
        return {action::move, to};
    }
    if (e.key == key::tab)
    {
        if (count < 2 || current < 0) return {};
        return {action::select, (current + (shift ? count - 1 : 1)) % count};
    }
    if (e.key != key::character || shift) return {};
    if (e.codepoint == U't') return {action::new_tab};
    if (e.codepoint == U'w') return current >= 0 ? action{action::close} : action{};
    if (e.codepoint >= U'1' && e.codepoint <= U'9')
    {
        const int i = e.codepoint == U'9' ? count - 1 : int(e.codepoint - U'1');
        return i >= 0 && i < count ? action{action::select, i} : action{};
    }
    return {};
}

// Ctrl+Tab or Ctrl+Shift+Tab, pressed: the one key a focused control may
// have a use of its own for (a page's tabs switching, a text view's tabs).
inline bool is_ctrl_tab(const key_event& e)
{
    return e.press && e.key == key::tab && (e.modifiers & mod_ctrl) &&
           !(e.modifiers & (mod_alt | mod_meta));
}

// The tab current once the one at `removed` goes, of `count_after` left:
// the one after it, else the one before, as browsers do; -1 for none.
inline int current_after_removal(int removed, int current, int count_after)
{
    if (count_after <= 0) return -1;
    if (current > removed) return current - 1;
    if (current < removed) return current;
    return std::min(removed, count_after - 1);
}

// The tabs' widths, in logical pixels: a browser's, about.
inline constexpr float narrowest_tab = 80, widest_tab = 220;

// Each tab's width in a row `available` wide: as wide as `widest` while all
// fit, narrower as they're added down to `narrowest`, then that and the
// row scrolls (clipped at its end), as browsers' tabs do.
inline int tab_width(int available, int count, int narrowest, int widest)
{
    if (count <= 0) return widest;
    return std::clamp(available / count, narrowest, widest);
}

// The element at `from` moved to `to`, the others between closing up: as a
// tab dragged along a row. Both must be in range.
template <class T>
void move_element(std::vector<T>& v, int from, int to)
{
    if (from < to)
        std::rotate(v.begin() + from, v.begin() + from + 1, v.begin() + to + 1);
    else if (to < from)
        std::rotate(v.begin() + to, v.begin() + from, v.begin() + from + 1);
}

// Where the tab at `i` is once the one at `from` has moved to `to`.
inline int index_after_move(int i, int from, int to)
{
    if (i == from) return to;
    if (from < to && i > from && i <= to) return i - 1;
    if (to < from && i >= to && i < from) return i + 1;
    return i;
}

// The bookkeeping: the tabs in order, which is current, and the app's
// handlers, which only the user's own clicks, drags and keys fire (through
// picked, close_asked, new_asked, reordered), once each.
class state
{
public:
    std::vector<document_tab> tabs;
    int current = -1;
    std::function<void(const std::string&)> on_select, on_close;
    std::function<void()> on_new;
    // The tab's id and the index it's at now, once it's been moved there.
    std::function<void(const std::string&, int)> on_reorder;

    int count() const { return int(tabs.size()); }
    int index_of(const std::string& id) const
    {
        for (size_t i = 0; i < tabs.size(); ++i)
            if (tabs[i].id == id) return int(i);
        return -1;
    }
    std::string current_id() const
    {
        return current >= 0 && current < count() ? tabs[size_t(current)].id : std::string();
    }
    std::vector<std::string> ids() const
    {
        std::vector<std::string> out;
        for (const document_tab& t : tabs) out.push_back(t.id);
        return out;
    }
    // Where a new tab goes for insert(index): the index it's at once in.
    int insert(int index, const document_tab& t)
    {
        if (index < 0 || index > count()) index = count();
        tabs.insert(tabs.begin() + index, t);
        if (current >= index) ++current;
        if (current < 0) current = 0;  // the first tab is current
        return index;
    }
    // The tab's index before it went, or -1 if there's none by that id.
    int remove(const std::string& id)
    {
        const int i = index_of(id);
        if (i < 0) return -1;
        tabs.erase(tabs.begin() + i);
        current = current_after_removal(i, current, count());
        return i;
    }

    // The user made tab `i` current (a click, or a key).
    void picked(int i)
    {
        if (i < 0 || i >= count() || i == current) return;
        current = i;
        // Copies, the handler's and the id's: the handler may replace itself
        // (destroying the closure that's running) or remove the tab.
        if (auto f = on_select) f(std::string(tabs[size_t(i)].id));
    }
    void close_asked(int i)
    {
        if (i < 0 || i >= count()) return;
        if (auto f = on_close) f(std::string(tabs[size_t(i)].id));
    }
    void new_asked()
    {
        if (auto f = on_new) f();
    }

    // The tab at `from` moved to `to`, telling no one (a drag's live steps,
    // and putting it back when one's cancelled): the current tab stays the
    // same tab. False if there's nothing to move.
    bool move(int from, int to)
    {
        if (from < 0 || from >= count() || to < 0 || to >= count() || from == to) return false;
        move_element(tabs, from, to);
        if (current >= 0) current = index_after_move(current, from, to);
        return true;
    }
    // The user moved the tab now at `index` there (a drag let go, a key):
    // on_reorder, once. Only when its index changed; the caller knows.
    void reordered(int index)
    {
        if (index < 0 || index >= count()) return;
        if (auto f = on_reorder) f(std::string(tabs[size_t(index)].id), index);
    }

    // What a key's move needs of the native control: nothing.
    struct no_move
    {
        void operator()(int, int) const {}
    };
    // A key, done; true if it was one of the tabs'. `show(i)` makes the
    // native control show tab i as current, without telling the app;
    // `moved(from, to)` moves its tab at `from` to `to` likewise, the
    // bookkeeping moved already.
    template <class Show, class Move = no_move>
    bool key(const key_event& e, Show&& show, Move&& moved = Move{})
    {
        const action a = for_key(e, current, count());
        switch (a.what)
        {
        case action::none: return false;
        case action::new_tab: new_asked(); return true;
        case action::close: close_asked(current); return true;
        case action::select:
            if (a.index != current)
            {
                show(a.index);
                picked(a.index);
            }
            return true;
        case action::move:
        {
            const int from = current;
            if (move(from, a.index))
            {
                moved(from, a.index);
                reordered(a.index);  // last: the app may change the tabs
            }
            return true;
        }
        }
        return false;
    }
};

}  // namespace rap::doc_tabs
