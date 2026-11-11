// tree_item::separator_before, through rapport's own API on any backend
// (RAPPORT_TREE_TEST_BACKEND, the headless one by default): a line is only
// drawn, so ids, selection, opening and lazily loaded children are just as
// without it.
//   rapport-tree-separator-test
#include <rapport/rapport.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#ifdef RAPPORT_TEST_BUILTIN_NULL
RAPPORT_BUILTIN_BACKEND(null);
#endif
#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}

rap::tree_item item(std::string id, bool line, std::vector<rap::tree_item> children = {})
{
    rap::tree_item t;
    t.label = id;
    t.id = std::move(id);
    t.separator_before = line;
    t.children = std::move(children);
    t.expanded = !t.children.empty();
    return t;
}

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options options;
    const char* backend = std::getenv("RAPPORT_TREE_TEST_BACKEND");
    options.backend = backend ? backend : "null";
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui)
    {
        std::printf("FAIL no %s backend: %s\n", options.backend.c_str(), ui.error().c_str());
        return 1;
    }

    // a, then lines before b, c (open: c1 first under it, c2 with a line),
    // and d (lazy, opened later).
    rap::tree_item lazy = item("d", true);
    lazy.has_children = true;
    std::vector<std::string> asked, changes;
    rap::tree* t = nullptr;
    using namespace rap::layout;
    auto win = (*ui)->create(window{
        "t",
        {tree{{item("a", false), item("b", true), item("c", true, {item("c1", true), item("c2", true)}), lazy}}
             .bind(t)
             .on_selection_changed([&] { changes.push_back(t->selected().value_or("")); })
             .on_expanding([&](const std::string& id) { asked.push_back(id); })}});
    win->show();

    for (const char* id : {"a", "b", "c", "c1", "c2", "d"})
    {
        t->select(id);
        expect(t->selected() == std::optional<std::string>(id), std::string("selects ") + id + " by its id");
    }
    expect(changes.empty(), "selecting from the app calls nothing back");

    t->set_expanded("d", true);
    expect(asked == std::vector<std::string>{"d"}, "opening the lazy item asks for its children");
    t->set_children("d", {item("d1", false), item("d2", true), item("d3", true)});
    for (const char* id : {"d1", "d2", "d3"})
    {
        t->select(id);
        expect(t->selected() == std::optional<std::string>(id), std::string("a lazily loaded ") + id + ", by its id");
    }

    // Lines on and off in place: nothing else moves.
    t->select("c2");
    t->set_separator_before("c2", false);
    t->set_separator_before("b", false);
    t->set_separator_before("a", true);   // the first row: no line, and no harm
    t->set_separator_before("c1", false); // first under its parent: had none
    t->set_separator_before("nope", true);
    expect(t->selected() == std::optional<std::string>("c2"), "turning lines on and off keeps the selection");
    t->set_separator_before("c2", true);
    t->set_separator_before("b", true);
    for (const char* id : {"a", "b", "c2", "d3"})
    {
        t->select(id);
        expect(t->selected() == std::optional<std::string>(id), std::string("and ") + id + " still selects");
    }

    // A refresh keeps the selection on its id, lines and all.
    t->select("d2");
    t->set_children("d", {item("d0", true), item("d2", true)});
    expect(t->selected() == std::optional<std::string>("d2"), "set_children keeps the selection across lines");
    t->set_items({item("a", false), item("b", true), item("d2", true)});
    expect(t->selected() == std::optional<std::string>("d2"), "and so does set_items");
    std::string reported;
    for (const std::string& c : changes) reported += " [" + c + "]";
    expect(changes.empty(), "and still nothing was called back" + reported);

    win->close();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
