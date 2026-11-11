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
    };
    kind what = none;
    int index = -1;
};

// Ctrl+T, Ctrl+W, Ctrl+Tab and Ctrl+Shift+Tab (round), Ctrl+1..8 and Ctrl+9
// for the last, as browsers and file managers have them. Presses only.
inline action for_key(const key_event& e, int current, int count)
{
    if (!e.press || (e.modifiers & (mod_alt | mod_meta)) || !(e.modifiers & mod_ctrl)) return {};
    const bool shift = e.modifiers & mod_shift;
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

// The bookkeeping: the tabs in order, which is current, and the app's
// handlers, which only the user's own clicks and keys fire (through picked,
// close_asked, new_asked), once each.
class state
{
public:
    std::vector<document_tab> tabs;
    int current = -1;
    std::function<void(const std::string&)> on_select, on_close;
    std::function<void()> on_new;

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
        if (on_select) on_select(std::string(tabs[size_t(i)].id));  // a copy: the app may remove it
    }
    void close_asked(int i)
    {
        if (i < 0 || i >= count()) return;
        if (on_close) on_close(std::string(tabs[size_t(i)].id));
    }
    void new_asked()
    {
        if (on_new) on_new();
    }
    // A key, done; true if it was one of the tabs'. `show(i)` makes the
    // native control show tab i as current, without telling the app.
    template <class Show>
    bool key(const key_event& e, Show&& show)
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
        }
        return false;
    }
};

}  // namespace rap::doc_tabs
