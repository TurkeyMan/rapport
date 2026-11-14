// A window's document tabs (backends/common/document_tabs.h): the keys,
// which tab is current once one goes, the tabs' widths, a tab moved (the
// current one staying current, on_reorder for the user's moves only), and
// the handlers firing for the user's doings only; then the same through the
// headless backend, from the window spec to the bind.
#include "document_tabs.h"

#include <rapport/rapport.h>

#include <cstdio>
#include <string>
#include <vector>

#ifdef RAPPORT_TEST_BUILTIN_NULL
RAPPORT_BUILTIN_BACKEND(null);
#endif

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}

rap::key_event ctrl(char32_t c, bool shift = false)
{
    rap::key_event e;
    e.key = rap::key::character;
    e.codepoint = c;
    e.modifiers = rap::mod_ctrl | (shift ? rap::mod_shift : 0);
    return e;
}
rap::key_event ctrl_shift(rap::key k)
{
    rap::key_event e;
    e.key = k;
    e.modifiers = rap::mod_ctrl | rap::mod_shift;
    return e;
}
rap::key_event ctrl_tab(bool shift = false)
{
    rap::key_event e;
    e.key = rap::key::tab;
    e.modifiers = rap::mod_ctrl | (shift ? rap::mod_shift : 0);
    return e;
}

void keys()
{
    using rap::doc_tabs::action;
    using rap::doc_tabs::for_key;
    expect(for_key(ctrl(U't'), 0, 3).what == action::new_tab, "Ctrl+T: a new tab");
    expect(for_key(ctrl(U'w'), 1, 3).what == action::close, "Ctrl+W: close the current one");
    expect(for_key(ctrl(U'w'), -1, 0).what == action::none, "Ctrl+W with no tabs: nothing");
    expect(for_key(ctrl_tab(), 2, 3).index == 0, "Ctrl+Tab from the last: round to the first");
    expect(for_key(ctrl_tab(true), 0, 3).index == 2,
           "Ctrl+Shift+Tab from the first: round to the last");
    expect(for_key(ctrl_tab(), 0, 1).what == action::none,
           "Ctrl+Tab with one tab: left to the focus");
    expect(for_key(ctrl(U'1'), 2, 3).index == 0, "Ctrl+1: the first");
    expect(for_key(ctrl(U'3'), 0, 3).index == 2, "Ctrl+3: the third");
    expect(for_key(ctrl(U'5'), 0, 3).what == action::none, "Ctrl+5 of three: nothing");
    expect(for_key(ctrl(U'9'), 0, 12).index == 11, "Ctrl+9: the last, however many");
    expect(for_key(ctrl(U't', true), 0, 3).what == action::none, "Ctrl+Shift+T: not ours");
    rap::key_event alt = ctrl(U't');
    alt.modifiers |= rap::mod_alt;
    expect(for_key(alt, 0, 3).what == action::none, "Ctrl+Alt+T: not ours (AltGr is Ctrl+Alt)");
    rap::key_event plain = ctrl(U't');
    plain.modifiers = 0;
    expect(for_key(plain, 0, 3).what == action::none, "T alone: typing");
    rap::key_event up = ctrl(U't');
    up.press = false;
    expect(for_key(up, 0, 3).what == action::none, "a release: nothing");
    using rap::key;
    const action right = for_key(ctrl_shift(key::page_down), 1, 3), left = for_key(ctrl_shift(key::page_up), 1, 3);
    expect(right.what == action::move && right.index == 2, "Ctrl+Shift+PgDn: the current tab one right");
    expect(left.what == action::move && left.index == 0, "Ctrl+Shift+PgUp: one left");
    expect(for_key(ctrl_shift(key::page_down), 2, 3).index == 2,
           "Ctrl+Shift+PgDn at the end: still the tabs' key, no round");
    expect(for_key(ctrl_shift(key::page_up), -1, 0).what == action::none, "with no tabs: nothing");
    rap::key_event plain_page = ctrl_shift(key::page_down);
    plain_page.modifiers = rap::mod_ctrl;
    expect(for_key(plain_page, 1, 3).what == action::none, "Ctrl+PgDn: not ours (a notebook's own)");
}

void moving()
{
    using rap::doc_tabs::index_after_move;
    std::vector<int> v{0, 1, 2, 3};
    rap::doc_tabs::move_element(v, 0, 2);
    expect(v == std::vector<int>{1, 2, 0, 3}, "one moved right: those between close up");
    rap::doc_tabs::move_element(v, 3, 0);
    expect(v == std::vector<int>{3, 1, 2, 0}, "one moved left");
    expect(index_after_move(0, 0, 2) == 2 && index_after_move(1, 0, 2) == 0 && index_after_move(3, 0, 2) == 3,
           "where each is once one's moved right");
    expect(index_after_move(0, 2, 0) == 1 && index_after_move(2, 2, 0) == 0, "and left");

    rap::doc_tabs::state s;
    std::vector<std::pair<std::string, int>> told;
    std::vector<std::string> selected;
    s.on_reorder = [&](const std::string& id, int i) { told.emplace_back(id, i); };
    s.on_select = [&](const std::string& id) { selected.push_back(id); };
    for (const char* id : {"a", "b", "c"}) s.insert(-1, {id, id, {}, {}});
    s.current = 1;
    expect(s.move(1, 2) && s.ids() == std::vector<std::string>{"a", "c", "b"} && s.current == 2 && told.empty(),
           "moved quietly: the current tab with it, no one told");
    expect(s.move(0, 2) && s.current == 1 && s.current_id() == "b", "another past it: the current one shifts");
    expect(!s.move(1, 1) && !s.move(0, 3) && !s.move(-1, 0), "nowhere, or out of range: no move");
    s.reordered(1);
    expect(told == std::vector<std::pair<std::string, int>>{{"b", 1}}, "reordered: on_reorder(id, index) once");
    told.clear();
    std::vector<std::pair<int, int>> native;
    int shown = -1;
    auto show = [&](int i) { shown = i; };
    auto moved = [&](int from, int to) { native.emplace_back(from, to); };
    // c, b, a with b current.
    expect(s.key(ctrl_shift(rap::key::page_down), show, moved) && s.ids() == std::vector<std::string>{"c", "a", "b"} &&
               s.current == 2 && native == std::vector<std::pair<int, int>>{{1, 2}} &&
               told == std::vector<std::pair<std::string, int>>{{"b", 2}},
           "Ctrl+Shift+PgDn: moved, the control told, then on_reorder once");
    expect(s.key(ctrl_shift(rap::key::page_down), show, moved) && told.size() == 1 && native.size() == 1,
           "at the end: taken, nothing moved or told");
    expect(s.key(ctrl_shift(rap::key::page_up), show) && s.current == 1 && told.size() == 2 && told[1].second == 1,
           "Ctrl+Shift+PgUp, with no control to tell: on_reorder once");
    expect(selected.empty() && shown == -1, "no on_select, nothing shown");
    // A handler that replaces itself as it runs keeps its own captures.
    std::string seen;
    const std::string label(64, 'x');
    s.on_reorder = [&s, &seen, label](const std::string&, int)
    {
        s.on_reorder = {};
        seen = label;
    };
    s.reordered(0);
    expect(seen == label, "an on_reorder that replaces itself as it runs");
}

void removal_and_widths()
{
    using rap::doc_tabs::current_after_removal;
    using rap::doc_tabs::tab_width;
    expect(current_after_removal(1, 1, 2) == 1, "the current one goes: the one after it");
    expect(current_after_removal(2, 2, 2) == 1, "the current last one goes: the one before it");
    expect(current_after_removal(0, 2, 2) == 1,
           "one before the current goes: the same tab, one on");
    expect(current_after_removal(2, 0, 2) == 0, "one after the current goes: the same tab");
    expect(current_after_removal(0, 0, 0) == -1, "the last tab goes: none");
    expect(tab_width(1000, 2, 80, 220) == 220, "two tabs in a wide row: the widest");
    expect(tab_width(1000, 8, 80, 220) == 125, "eight: shrunk to share the row");
    expect(tab_width(1000, 20, 80, 220) == 80, "twenty: the narrowest, and the row scrolls");
}

void handlers()
{
    rap::doc_tabs::state s;
    std::vector<std::string> selected, closed;
    int added = 0;
    s.on_select = [&](const std::string& id) { selected.push_back(id); };
    s.on_close = [&](const std::string& id) { closed.push_back(id); };
    s.on_new = [&] { ++added; };
    for (const char* id : {"a", "b", "c"}) s.insert(-1, {id, id, {}, {}});
    expect(s.current == 0 && selected.empty(), "the first tab added is current, quietly");
    expect(s.insert(1, {"x", "x", {}, {}}) == 1 && s.current == 0,
           "one put before the current leaves it current");
    s.remove("x");
    int shown = -1;
    auto show = [&](int i) { shown = i; };
    expect(s.key(ctrl_tab(), show) && shown == 1 && selected == std::vector<std::string>{"b"},
           "Ctrl+Tab: the next shown, on_select once");
    expect(s.key(ctrl(U'2'), show) && selected.size() == 1,
           "Ctrl+2 to the current one: no on_select");
    expect(s.key(ctrl(U't'), show) && added == 1, "Ctrl+T: on_new once");
    expect(s.key(ctrl(U'w'), show) && closed == std::vector<std::string>{"b"} && s.count() == 3,
           "Ctrl+W: on_close once, the tab left to the app");
    s.remove("b");
    expect(s.current_id() == "c" && selected.size() == 1,
           "the app closes it: the next is current, quietly");

    // A handler that replaces itself as it runs keeps its own captures (it
    // runs as a copy): under ASan, reading them was a use after free.
    std::string seen;
    const std::string label(64, 'x');  // on the heap, not in the closure's small buffer
    s.on_select = [&s, &seen, label](const std::string&)
    {
        s.on_select = {};
        seen = label;
    };
    s.on_close = [&s, &seen, label](const std::string&)
    {
        s.on_close = {};
        seen += label;
    };
    s.on_new = [&s, &seen, label]
    {
        s.on_new = {};
        seen += label;
    };
    s.picked(0);
    s.close_asked(0);
    s.new_asked();
    expect(seen == label + label + label, "handlers that replace themselves as they run");
}

// Tearing off and dropping, as the backends share it: what a drag does
// where, the tear-off distance, where a dropped tab lands, the handlers'
// contract (on_detach once, null refusing; on_receive's transfer, false
// changing nothing; the group), and the right-click menu.
void tear_off_and_drop()
{
    using namespace rap::doc_tabs;
    expect(step_for({.only_tab = true, .over_other = true, .past_tear = true, .can_detach = true}) ==
               drag_step::move_window,
           "the only tab: its window moves, wherever it's dragged");
    expect(step_for({.over_other = true, .past_tear = true, .can_detach = true}) == drag_step::over_other,
           "over another window's row: dropped there, not torn off");
    expect(step_for({.past_tear = true, .can_detach = true}) == drag_step::tear_off, "out past the distance: torn off");
    expect(step_for({.past_tear = true, .can_detach = false}) == drag_step::reorder,
           "no on_detach, or refused: along the row still");
    expect(step_for({.can_detach = true}) == drag_step::reorder, "near the row: along it");
    expect(!past_tear(50, 10, 0, 40, 0, 500, 24) && !past_tear(50, 63, 0, 40, 0, 500, 24),
           "in the row, and just under it: not torn");
    expect(past_tear(50, 64, 0, 40, 0, 500, 24) && past_tear(50, -25, 0, 40, 0, 500, 24),
           "past the distance below or above: torn");
    expect(past_tear(-25, 10, 0, 40, 0, 500, 24) && past_tear(524, 10, 0, 40, 0, 500, 24) &&
               !past_tear(523, 10, 0, 40, 0, 500, 24),
           "past the window's sides by the distance: torn");
    const std::vector<int> middles{50, 150, 250};
    expect(drop_index(middles, 0) == 0 && drop_index(middles, 49) == 0 && drop_index(middles, 50) == 1 &&
               drop_index(middles, 200) == 2 && drop_index(middles, 900) == 3 && drop_index({}, 10) == 0,
           "a dropped tab lands before the first tab whose middle is past it");

    state src, dst;
    for (const char* id : {"a", "b"}) src.insert(-1, {id, std::string("T") + id, rap::icon::named("folder"), "/p"});
    dst.insert(-1, {"x", "x", {}, {}});
    expect(!src.same_group(dst), "no group: no drops");
    src.group = "files";
    expect(!src.same_group(dst), "one in a group: none");
    dst.group = "other";
    expect(!src.same_group(dst), "different groups: none");
    dst.group = "files";
    expect(src.same_group(dst) && dst.same_group(src), "the same group: both ways");

    // on_detach: asked with the id and the point, null refusing.
    std::vector<std::string> asked;
    std::optional<rap::point_f> where;
    rap::window* answer = nullptr;
    expect(src.detach_asked(0, rap::point_f{1, 2}) == nullptr, "no on_detach: nothing");
    src.on_detach = [&](const std::string& id, std::optional<rap::point_f> at) -> rap::window*
    {
        asked.push_back(id);
        where = at;
        return answer;
    };
    expect(src.detach_asked(1, rap::point_f{10, 20}) == nullptr && asked == std::vector<std::string>{"b"} &&
               where && where->x == 10 && where->y == 20,
           "on_detach(b, the point), once; null refuses");
    expect(src.ids() == std::vector<std::string>{"a", "b"}, "refused: the tabs as they were");
    src.detach_asked(0, std::nullopt);
    expect(asked.size() == 2 && !where, "the menu's: no point");
    state lone;
    lone.insert(-1, {"z", "z", {}, {}});
    lone.on_detach = src.on_detach;
    expect(lone.detach_asked(0, std::nullopt) == nullptr && asked.size() == 2, "a window's only tab: never asked");

    // on_receive: the transfer, and the app's answer.
    std::vector<rap::document_tabs::transfer> got;
    bool accept = false;
    dst.on_receive = [&](const rap::document_tabs::transfer& t)
    {
        got.push_back(t);
        return accept;
    };
    auto* source = reinterpret_cast<rap::window*>(&src);  // an address to compare, never used
    const rap::document_tabs::transfer t = src.transfer_of(1, 1, source);
    expect(t.id == "b" && t.title == "Tb" && t.icon.name == "folder" && t.tooltip == "/p" && t.index == 1 &&
               t.source == source,
           "the transfer: id, title, icon, tooltip, index and source");
    expect(!dst.received(t) && got.size() == 1 && dst.ids() == std::vector<std::string>{"x"} &&
               src.ids() == std::vector<std::string>{"a", "b"},
           "on_receive false: nothing changes");
    accept = true;
    expect(dst.received(t) && got.size() == 2, "true: the app's taken it");
    state none;
    expect(!none.received(t), "no on_receive: refused");

    // The menu: New tab; Close tab, Move to new window; the app's.
    rap::menu m = src.context_menu(-1);
    expect(m.size() == 1 && m[0].label == rap::i18n::tr("rapport", "_New tab"), "the row's empty part: New tab alone");
    src.on_context_menu = [](const std::string& id) { return rap::menu{{"_Rename " + id, {}}}; };
    m = src.context_menu(1);
    expect(m.size() == 6 && m[1].is_separator() && m[2].label == rap::i18n::tr("rapport", "_Close tab") &&
               m[3].label == rap::i18n::tr("rapport", "_Move to new window") && m[3].enabled && m[4].is_separator() &&
               m[5].label == "_Rename b",
           "a tab's: New tab, Close tab, Move to new window, then the app's after a separator");
    std::vector<rap::window*> shown;
    src.show_moved = [&](rap::window* w) { shown.push_back(w); };
    m[3].action();
    expect(asked.size() == 3 && asked.back() == "b" && !where, "Move to new window: on_detach(b) with no point");
    expect(shown.empty(), "refused (null): nothing shown");
    answer = reinterpret_cast<rap::window*>(&dst);  // an address to compare, never used
    m[3].action();
    expect(asked.size() == 4 && shown == std::vector<rap::window*>{answer},
           "made: the app's window shown, as a torn-off tab's is");
    answer = nullptr;
    src.on_context_menu = [](const std::string&) { return rap::menu{}; };
    expect(src.context_menu(0).size() == 4, "the app's items empty: no separator for them");
    src.on_detach = {};
    expect(src.context_menu(0).size() == 3, "no on_detach: no Move to new window");
    lone.on_detach = [](const std::string&, std::optional<rap::point_f>) -> rap::window* { return nullptr; };
    expect(!lone.context_menu(0)[3].enabled, "the only tab: Move to new window disabled");
}

// The spec through the headless backend: the bar is made with its items,
// the current one, its handlers and its bind.
int backend(int argc, char** argv)
{
    rap::load_options opts;
    opts.backend = "null";
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::printf("FAIL %s\n", ui.error().c_str());
        return 1;
    }
    rap::document_tabs* tabs = nullptr;
    int added = 0, reordered = 0;
    using namespace rap::layout;
    // The builder's on_reorder in the spec, as given.
    const rap::spec::document_tabs spec =
        document_tabs{{{"a", "A", {}, {}}}}.on_reorder([&](const std::string& id, int i) { reordered += id == "a" && i == 3; });
    expect(bool(spec.on_reorder), "on_reorder: in the spec");
    if (spec.on_reorder) spec.on_reorder("a", 3);
    expect(reordered == 1, "and the app's own handler");
    reordered = 0;
    auto win = (*ui)->create(window{"t", {label{"content"}}}.tabs(document_tabs{
        {{"a", "A", {}, {}}, {"b", "B", {}, "/b"}}}.current("b")
                                                                      .on_new([&] { ++added; })
                                                                      .on_reorder([&](const std::string&, int) { ++reordered; })
                                                                      .bind(tabs)));
    expect(tabs != nullptr, "bind: the window's tabs");
    if (!tabs) return 1;
    expect(tabs->ids() == std::vector<std::string>{"a", "b"} && tabs->current() == "b",
           "made with its items and current one");
    tabs->add({"c", "C", {}, {}});
    tabs->select("c");
    tabs->remove("a");
    expect(tabs->ids() == std::vector<std::string>{"b", "c"} && tabs->current() == "c",
           "add, select, remove");
    tabs->on_reorder([&](const std::string&, int) { reordered += 10; });
    expect(reordered == 0 && added == 0, "nothing dragged or pressed: nothing told");

    // Tearing off and dropping: the builder's handlers and group in the spec,
    // and through the backend (null stores them, as every backend).
    int detached = 0, received = 0, menus = 0;
    const rap::spec::document_tabs torn =
        document_tabs{{{"a", "A", {}, {}}}}
            .group("files")
            .on_detach([&](const std::string&, std::optional<rap::point_f>) -> rap::window* { ++detached; return nullptr; })
            .on_receive([&](const rap::document_tabs::transfer&) { ++received; return false; })
            .on_context_menu([&](const std::string&) { ++menus; return rap::menu{}; });
    expect(torn.group == "files" && torn.on_detach && torn.on_receive && torn.on_context_menu,
           "group, on_detach, on_receive and on_context_menu: in the spec");
    rap::document_tabs* bound = nullptr;
    rap::spec::document_tabs with_bind = torn;
    with_bind.bind = &bound;
    auto win2 = (*ui)->create(window{"u", {label{"content"}}}.tabs(with_bind));
    expect(bound != nullptr && detached == 0 && received == 0 && menus == 0,
           "made with them, none called by the making");
    if (bound)
    {
        bound->set_group("other");
        bound->on_detach({});
        bound->on_receive({});
        bound->on_context_menu({});
        expect(detached == 0 && received == 0 && menus == 0, "and the setters call nothing");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    keys();
    moving();
    removal_and_widths();
    handlers();
    tear_off_and_drop();
    if (backend(argc, argv)) ++fails;
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
