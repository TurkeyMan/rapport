// Tab page visibility (rap::tabs::set_page_visible, layout::page::visible,
// tabs::hide_single_tab) through the headless backend: indices stable, the
// current page moving to the nearest visible one without on_changed, -1 with
// none visible, set_current on a hidden page ignored, pages hidden from the
// start (strips and sidebars), and a hidden lazy page left unbuilt.
#include <rapport/rapport.h>

#include <cstdio>
#include <string>

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

    for (const bool sidebar : {false, true})
    {
        const std::string kind = sidebar ? "sidebar: " : "strip: ";
        rap::tabs* t = nullptr;
        rap::label* lazy_label = nullptr;
        int changes = 0;
        auto win = (*ui)->create(window{
            "Tabs",
            {tabs{{"A", label{"a"}},
                  {"B", label{"b"}},
                  {"C", label{"c"}},
                  {"D", lazy{[&] { return rap::node(label{"d"}.bind(lazy_label)); }}}}
                 .sidebar(sidebar)
                 .on_changed([&](int) { ++changes; })
                 .bind(t)}});
        expect(t != nullptr, kind + "bound");
        if (!t) return 1;
        expect(t->current() == 0, kind + "the first page current");

        // A middle page hidden: the others keep their indices.
        t->set_page_visible(1, false);
        expect(!t->page_visible(1) && t->page_visible(0) && t->page_visible(2), kind + "page 1 hidden, 0 and 2 not");
        t->set_current(2);
        expect(t->current() == 2, kind + "page 2 is still 2");
        // set_current on a hidden page: nothing.
        t->set_current(1);
        expect(t->current() == 2, kind + "set_current(hidden 1): still 2");

        // The current page hidden: the nearest visible after it (the lazy D,
        // built now), without on_changed.
        t->set_page_visible(2, false);
        expect(t->current() == 3, kind + "page 2 (current) hidden: 3 current, " + std::to_string(t->current()));
        expect(lazy_label != nullptr, kind + "the lazy page, made current, built");
        // The last page hidden while current: the nearest before it.
        t->set_page_visible(3, false);
        expect(t->current() == 0, kind + "page 3 (current, last) hidden: 0 current, " + std::to_string(t->current()));
        // All hidden: -1.
        t->set_page_visible(0, false);
        expect(t->current() == -1, kind + "every page hidden: current -1, " + std::to_string(t->current()));
        // Shown again: current again (it was -1); another shown keeps it.
        t->set_page_visible(2, true);
        expect(t->current() == 2, kind + "page 2 shown again with none current: 2 current");
        t->set_page_visible(1, true);
        expect(t->current() == 2 && t->page_visible(1), kind + "page 1 shown again: current stays 2");
        // The same again: nothing.
        t->set_page_visible(1, true);
        expect(t->current() == 2, kind + "page 1 shown again, twice: current stays 2");
        expect(changes == 0, kind + "no on_changed from any of it, " + std::to_string(changes));
        // Out of range: nothing.
        t->set_page_visible(9, false);
        expect(!t->page_visible(9) && !t->page_visible(-1), kind + "pages out of range: not visible");
    }

    // Hidden from the start: no current page there, and a lazy page hidden
    // stays unbuilt.
    for (const bool sidebar : {false, true})
    {
        const std::string kind = sidebar ? "sidebar, " : "strip, ";
        rap::tabs* t = nullptr;
        rap::label* lazy_label = nullptr;
        auto win = (*ui)->create(window{
            "Tabs hidden",
            {tabs{page{"A", lazy{[&] { return rap::node(label{"a"}.bind(lazy_label)); }}}.visible(false),
                  page{"B", label{"b"}}.visible(false),
                  {"C", label{"c"}}}
                 .sidebar(sidebar)
                 .hide_single_tab()
                 .bind(t)}});
        expect(t != nullptr, kind + "hidden from the start: bound");
        if (!t) return 1;
        expect(!t->page_visible(0) && !t->page_visible(1) && t->page_visible(2),
               kind + "hidden from the start: A and B hidden, C not");
        expect(t->current() == 2, kind + "hidden from the start: C current, " + std::to_string(t->current()));
        expect(lazy_label == nullptr, kind + "hidden from the start: the lazy A unbuilt");
        // #110: the option read back, set at runtime; current() stays.
        expect(t->hide_single_tab(), kind + "hide_single_tab on from the build");
        t->set_hide_single_tab(false);
        expect(!t->hide_single_tab() && t->current() == 2,
               kind + "set_hide_single_tab(false): off, C current");
        t->set_hide_single_tab(false);
        expect(!t->hide_single_tab(), kind + "set_hide_single_tab(false) again: off");
        t->set_hide_single_tab(true);
        expect(t->hide_single_tab() && t->current() == 2,
               kind + "set_hide_single_tab(true): on, C current");
        t->set_page_visible(0, true);
        expect(t->current() == 2 && lazy_label == nullptr, kind + "A shown: C stays current, A still unbuilt");
        t->set_current(0);
        expect(t->current() == 0 && lazy_label != nullptr, kind + "A made current: built");
    }

    // A page's icon and title (#101): at build time and at runtime, pages
    // out of range ignored, neither firing on_changed (the null backend
    // prints them; nothing to read back but that nothing breaks).
    for (const bool sidebar : {false, true})
    {
        const std::string kind = sidebar ? "sidebar, icons: " : "strip, icons: ";
        rap::tabs* t = nullptr;
        int changes = 0;
        auto win = (*ui)->create(window{
            "Tabs icons",
            {tabs{page{"A", label{"a"}}.icon(rap::icon::named("go-up")),
                  page{"B", label{"b"}}.icon(rap::icon::drawn([](rap::painter&, rap::size_f) {})),
                  {"C", label{"c"}}}
                 .sidebar(sidebar)
                 .on_changed([&](int) { ++changes; })
                 .bind(t)}});
        expect(t != nullptr, kind + "bound");
        if (!t) return 1;
        t->set_page_icon(2, rap::icon::named("go-down"));
        t->set_page_icon(0, rap::icon{});
        t->set_page_title(1, "Build");
        t->set_page_title(1, "Build");
        t->set_page_icon(9, rap::icon::named("go-up"));
        t->set_page_title(-1, "nothing");
        t->set_page_visible(1, false);
        t->set_page_title(1, "Build (hidden)");
        t->set_page_visible(1, true);
        expect(changes == 0 && t->current() == 0, kind + "no on_changed, A still current, " + std::to_string(changes));
    }

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
