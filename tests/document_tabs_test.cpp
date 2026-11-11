// A window's document tabs (backends/common/document_tabs.h): the keys,
// which tab is current once one goes, the tabs' widths, and the handlers
// firing for the user's doings only; then the same through the headless
// backend, from the window spec to the bind.
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
    int added = 0;
    using namespace rap::layout;
    auto win = (*ui)->create(window{"t", {label{"content"}}}.tabs(document_tabs{
        {{"a", "A", {}, {}}, {"b", "B", {}, "/b"}}}.current("b")
                                                                      .on_new([&] { ++added; })
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
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    keys();
    removal_and_widths();
    handlers();
    if (backend(argc, argv)) ++fails;
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
