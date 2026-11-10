// Loading a tree's children on demand, on the headless backend: when a node
// asks, and what an answer does.
//   rapport-tree-lazy-test
#include <rapport/rapport.h>

#include "lazy_tree.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
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

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

using rap::tree_item;
using list_of = std::vector<std::string>;

tree_item lazy(std::string id, bool expanded = false)
{
    tree_item t;
    t.label = id;
    t.id = std::move(id);
    t.has_children = true;
    t.expanded = expanded;
    return t;
}

tree_item plain(std::string id)
{
    tree_item t;
    t.label = id;
    t.id = std::move(id);
    return t;
}

// A fresh tree, recording what it asks for. `answer`, if set, runs inside the
// handler.
struct fixture
{
    fixture(rap::ui& ui, std::vector<tree_item> items)
    {
        using namespace rap::layout;
        win = ui.create(window{"t", {tree{std::move(items)}
                                         .bind(t)
                                         .on_expanding(
                                             [this](const std::string& id)
                                             {
                                                 asked.push_back(id);
                                                 if (answer) answer(id);
                                             })}});
    }
    list_of asked;
    std::function<void(const std::string&)> answer;
    rap::tree* t = nullptr;
    std::unique_ptr<rap::window> win;
};

void tracker_rules()
{
    using rap::lazy_tree::tracker;
    list_of asked;
    tracker t;
    t.mark("a");
    t.opened("a");
    check(asked.empty(), "an ask with no handler waits for one");
    t.on_request([&](const std::string& id) { asked.push_back(id); });
    check(asked == list_of{"a"}, "and goes when there is one");
    t.opened("a");
    check(asked.size() == 1, "once until answered");

    asked.clear();
    t.mark("b");
    {
        tracker::batch b(t);
        t.opened("b");
        check(asked.empty(), "not while the tree is changing");
    }
    check(asked == list_of{"b"}, "after it's finished");

    asked.clear();
    t.mark("c");
    {
        tracker::batch b(t);
        t.opened("c");
        t.forget("c");
        t.mark("c");
    }
    check(asked.empty(), "an ask overtaken by the node being replaced is dropped");

    asked.clear();
    t.forget("a");
    t.opened("a");
    check(asked.empty(), "a node that isn't waiting doesn't ask");

    // A handler that destroys the tree it's handling.
    {
        auto owner = std::make_unique<tracker>();
        int calls = 0;
        owner->mark("x");
        owner->mark("y");
        {
            tracker::batch b(*owner);
            owner->opened("x");
            owner->opened("y");
            owner->on_request(
                [&](const std::string&)
                {
                    ++calls;
                    owner.reset();
                });
        }
        check(calls == 1 && !owner, "a handler can destroy its tree, and nothing more is asked");
    }

    // A handler holding the last owner of its tree, removing itself: the tree
    // goes when the running copy of the handler does.
    {
        auto owner = std::make_shared<tracker>();
        const std::weak_ptr<tracker> watch = owner;
        int calls = 0;
        owner->mark("x");
        owner->mark("y");
        {
            tracker::batch b(*owner);
            owner->opened("x");
            owner->opened("y");
        }
        owner->on_request(
            [&calls, &owner, keep = owner](const std::string&)
            {
                ++calls;
                owner->on_request({});
                owner.reset();
            });
        check(calls == 1 && watch.expired(), "a handler holding the last owner can remove itself");
    }

    // A handler that removes itself: the rest wait for the next one.
    {
        tracker u;
        list_of got;
        u.mark("x");
        u.mark("y");
        u.opened("x");
        u.opened("y");
        u.on_request(
            [&](const std::string& id)
            {
                got.push_back(id);
                u.on_request({});
            });
        check(got == list_of{"x"}, "a handler removed part way stops the delivery");
        u.on_request([&](const std::string& id) { got.push_back(id); });
        check((got == list_of{"x", "y"}), "and what's left goes to the next handler");
    }
}

}  // namespace

int main(int argc, char** argv)
{
    tracker_rules();

    // The headless backend by default; any other to check it the same way.
    rap::load_options options;
    const char* backend = std::getenv("RAPPORT_TREE_TEST_BACKEND");
    options.backend = backend ? backend : "null";
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui)
    {
        std::printf("FAIL no null backend: %s\n", ui.error().c_str());
        return 1;
    }

    {
        fixture f(**ui, {lazy("a")});
        check(f.asked.empty(), "a closed node doesn't ask");
        f.t->set_expanded("a", true);
        check(f.asked == list_of{"a"}, "opened by the app, it asks");
        f.t->set_expanded("a", false);
        f.t->set_expanded("a", true);
        check(f.asked.size() == 1, "closing and reopening with an answer outstanding doesn't ask again");
        f.t->set_children("a", {plain("b")});
        f.t->set_expanded("a", false);
        f.t->set_expanded("a", true);
        check(f.asked.size() == 1, "once loaded, reopening never asks");
    }
    {
        fixture f(**ui, {lazy("a", true)});
        check(f.asked == list_of{"a"}, "expanded in the items, it asks");
    }
    {
        tree_item parent = plain("p");
        parent.children = {lazy("a")};
        fixture f(**ui, {parent});
        f.t->select("a");
        check(f.asked.empty(), "selecting a node doesn't open it");
    }
    {
        tree_item top = lazy("p");
        fixture f(**ui, {top});
        f.t->set_expanded("p", true);
        f.t->set_children("p", {lazy("q")});
        f.asked.clear();
        f.t->select("q");
        check(f.asked.empty(), "selecting opens ancestors, which are loaded");
    }

    // Answering from inside the handler.
    {
        fixture f(**ui, {lazy("a")});
        f.answer = [&](const std::string& id) { f.t->set_children(id, {plain("b")}); };
        f.t->set_expanded("a", true);
        f.t->select("b");
        check(f.t->selected() == "b", "a synchronous answer lands");
        check(f.asked == list_of{"a"}, "and asks once");
    }
    // A request raised during set_children.
    {
        fixture f(**ui, {lazy("a")});
        f.t->set_expanded("a", true);
        f.asked.clear();
        f.answer = [&](const std::string& id) { f.t->set_children(id, {plain("c")}); };
        tree_item b = lazy("b", true);
        f.t->set_children("a", {plain("kept"), b});
        check(f.asked == list_of{"b"}, "a new open node asks");
        f.t->select("kept");
        f.t->select("c");
        check(f.t->selected() == "c", "and its synchronous answer lands");
    }

    // A handler that destroys the window, from a call the app made.
    {
        fixture f(**ui, {lazy("a")});
        f.answer = [&](const std::string&) { f.win.reset(); };
        f.t->set_expanded("a", true);
        check(!f.win && f.asked == list_of{"a"}, "a handler can destroy the window it's in");
    }
    // A real item with what could look like an internal name.
    {
        const std::string odd = "\x1fplaceholder:a";
        fixture f(**ui, {lazy("a"), plain(odd)});
        f.t->set_expanded("a", true);
        f.t->set_children("a", {});
        f.t->select(odd);
        check(f.t->selected() == odd, "no id the app uses collides with a placeholder");
    }

    // Edge cases.
    {
        fixture f(**ui, {lazy("a")});
        f.t->set_expanded("a", true);
        f.t->set_expanded("a", false);
        f.t->set_children("a", {plain("b")});
        f.asked.clear();
        f.t->set_expanded("a", true);
        check(f.asked.empty(), "a late answer to a closed node applies");
        f.t->select("b");
        check(f.t->selected() == "b", "with its children there");
    }
    {
        fixture f(**ui, {lazy("a")});
        f.t->set_expanded("a", true);
        f.t->set_children("a", {});
        f.asked.clear();
        f.t->set_expanded("a", true);
        check(f.asked.empty(), "an empty answer: nothing more to ask");
    }
    {
        fixture f(**ui, {lazy("a")});
        f.t->set_expanded("a", true);
        f.t->update_item("a", "a", {}, false);
        f.asked.clear();
        f.t->update_item("a", "a", {}, false);
        f.t->set_expanded("a", true);
        check(f.asked.empty(), "has_children false with an answer outstanding is an empty answer");
    }
    {
        fixture f(**ui, {lazy("a")});
        f.t->set_expanded("a", true);
        f.t->set_children("a", {});
        f.asked.clear();
        f.t->update_item("a", "a", {}, true);
        check(f.asked.empty(), "the chevron back doesn't ask by itself");
        f.t->set_expanded("a", true);
        check(f.asked == list_of{"a"}, "opening it does");
    }
    {
        fixture f(**ui, {lazy("a")});
        f.t->set_expanded("a", true);
        f.t->set_children("a", {plain("b")});
        f.asked.clear();
        f.t->update_item("a", "a", {}, true);
        f.t->set_expanded("a", true);
        check(f.asked.empty(), "has_children on a loaded node does nothing");
    }
    {
        fixture f(**ui, {lazy("a")});
        f.t->set_expanded("a", true);
        f.asked.clear();
        f.t->set_items({lazy("a")});
        check(f.asked == list_of{"a"}, "set_items replaces it, and what's still open asks again");
    }
    {
        fixture f(**ui, {lazy("a")});
        f.t->set_expanded("a", true);
        f.t->set_children("a", {lazy("b", true)});
        f.asked.clear();
        f.t->set_children("a", {lazy("b")});
        check(f.asked == list_of{"b"}, "a retained open node with nothing loaded asks again");
    }
    {
        fixture f(**ui, {lazy("a")});
        f.t->set_children("nope", {plain("b")});
        f.t->update_item("nope", "x", {}, true);
        check(f.asked.empty(), "ids that aren't there are ignored");
    }
    {
        fixture f(**ui, {lazy("a")});
        f.t->set_expanded("a", true);
        f.t->set_children("a", {plain("b"), plain("c")});
        f.t->select("b");
        f.t->set_children("a", {plain("b"), plain("d")});
        check(f.t->selected() == "b", "the selection stays on an id still there");
        f.t->set_children("a", {plain("d")});
        check(!f.t->selected(), "and clears if it's gone");
    }

    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
