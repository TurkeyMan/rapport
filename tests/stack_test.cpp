// A stack's pages (layout::stack, rap::stack) through the headless backend:
// the first shown, pages added (built, binds set) and removed (the shown one
// giving way to the next, or the previous when it was last), duplicates and
// unknown ids ignored, and a page's widgets destroyed only once the event
// loop is reached, with the page gone from ids() at once.
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

using ids = std::vector<std::string>;

// Counts the lists reading it: a list observing it reads its row count when
// it's reset, and one destroyed observes no more.
class probe_model : public rap::list_model
{
public:
    size_t row_count() const override
    {
        ++reads;
        return 0;
    }
    std::string text(size_t, size_t) const override { return {}; }
    // How many lists observe it now.
    int readers()
    {
        reads = 0;
        notify_reset();
        return reads;
    }
    mutable int reads = 0;
};

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options opts;
    opts.backend = "null";
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::printf("FAIL no null backend: %s\n", ui.error().c_str());
        return 1;
    }
    using namespace rap::layout;

    rap::stack* st = nullptr;
    rap::label* first = nullptr;
    auto win = (*ui)->create(window{
        "t",
        {stack{{"a", label{"A"}.bind(first)}, {"b", label{"B"}}, {"a", label{"dup"}}}.bind(st)}});
    expect(st != nullptr, "bind: the stack");
    if (!st) return 1;
    expect(first != nullptr, "a page's binds are set with the window's");
    expect(st->ids() == ids{"a", "b"}, "pages in order, a repeated id in the spec left out");
    expect(st->shown() == "a", "the first page is shown");

    st->show("b");
    expect(st->shown() == "b", "show: another page");
    st->show("nope");
    expect(st->shown() == "b", "show: an unknown id is ignored");

    probe_model model;
    rap::button* late = nullptr;
    rap::list* late_list = nullptr;
    st->add("c", vbox{button{"Late"}.bind(late), list{{{"Name"}}, &model}.bind(late_list)});
    expect(late != nullptr && late_list != nullptr, "add: the page is built and its binds set");
    expect(model.readers() == 1, "add: its list observes its model");
    expect(st->ids() == ids{"a", "b", "c"}, "add: last");
    expect(st->shown() == "b", "add: the page added is hidden");

    rap::button* dup = nullptr;
    st->add("c", button{"Again"}.bind(dup));
    expect(dup == nullptr && st->ids() == ids{"a", "b", "c"},
           "add: a repeated id is ignored, nothing built");

    st->remove("nope");
    expect(st->ids() == ids{"a", "b", "c"}, "remove: an unknown id is ignored");

    st->remove("c");
    expect(st->ids() == ids{"a", "b"}, "remove: gone from ids() at once");
    expect(st->shown() == "b", "remove: another page than the one shown leaves it shown");
    expect(model.readers() == 1, "remove: its widgets aren't destroyed yet");
    (*ui)->run();  // the queue, until it's empty
    expect(model.readers() == 0, "remove: its widgets are destroyed once the event loop runs");

    // The shown page removed: the next shows, or the previous if it was last.
    st->add("c", label{"C"});
    st->add("d", label{"D"});
    st->show("b");
    st->remove("b");
    expect(st->shown() == "c", "the shown page removed: the next shows");
    st->remove("d");
    expect(st->shown() == "c", "a page after the shown one removed: the shown one stays");
    st->remove("c");
    expect(st->shown() == "a", "the shown page removed, the last: the previous shows");
    st->remove("a");
    expect(st->shown().empty() && st->ids().empty(), "the last page removed: none shown");

    // Removed from inside the event loop (as from a callback) and added
    // again under the same id at once: the new page is separate.
    probe_model again;
    st->add("x", list{{{"Name"}}, &again});
    expect(st->shown() == "x", "added to an empty stack: shown");
    (*ui)->post(
        [&]
        {
            st->remove("x");
            expect(st->ids().empty() && again.readers() == 1,
                   "removed from a task: out of ids(), its widgets still there while it runs");
            st->add("x", label{"X again"});
            expect(st->ids() == ids{"x"} && st->shown() == "x", "the id added again at once");
        });
    (*ui)->run();
    expect(again.readers() == 0, "and destroyed after the task returned");

    // Not homogeneous: built the same, sized differently by real toolkits.
    rap::stack* sized = nullptr;
    auto win2 =
        (*ui)->create(window{"u", {stack{{"one", label{"1"}}}.homogeneous(false).bind(sized)}});
    expect(sized && sized->shown() == "one", "homogeneous(false): built");

    // An empty stack.
    rap::stack* empty = nullptr;
    auto win3 = (*ui)->create(window{"v", {stack{}.bind(empty)}});
    expect(empty && empty->ids().empty() && empty->shown().empty(),
           "an empty stack: no pages, none shown");
    empty->show("a");
    empty->remove("a");
    expect(empty->shown().empty(), "an empty stack: show and remove of unknown ids ignored");

    // A window closed with a page waiting to be destroyed.
    probe_model gone;
    rap::stack* doomed = nullptr;
    {
        auto w4 = (*ui)->create(window{"w", {stack{{"p", list{{{"Name"}}, &gone}}}.bind(doomed)}});
        doomed->remove("p");
    }
    expect(gone.readers() == 0, "a window closed: its stack's removed pages go with it");
    (*ui)->run();  // the clearing posted, finding the stack gone

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
