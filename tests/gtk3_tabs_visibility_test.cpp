// Tab page visibility on GTK 3 (rap::tabs::set_page_visible,
// layout::page::visible, tabs::hide_single_tab), read back from the
// widgets: a GtkNotebook's hidden page is a hidden child (its tab goes with
// it) and keeps its index; a sidebar's hidden page is a hidden GtkListBox
// row and stack child. With hide_single_tab and one page visible, the
// notebook has no tabs (and no frame), the sidebar no list, and neither
// measures them. Also #92: a canvas of a fixed size as a tab page and a
// stack page, measured. And Ctrl+Tab with the focus in a page's tabs: round
// their visible pages (a hidden one skipped), nothing with no pages or none
// visible, and never stuck. No window is shown but Ctrl+Tab's.
//   rapport-tabs-visibility-test-gtk3   (through offscreen.sh gtk3)
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include <unistd.h>

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
}

std::string str(long v)
{
    return std::to_string(v);
}

void settle()
{
    for (int i = 0; i < 10; ++i)
    {
        while (gtk_events_pending()) gtk_main_iteration_do(false);
        g_usleep(2000);
    }
}

void find_all(GtkWidget* w, GType type, std::vector<GtkWidget*>& out)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE(w, type)) out.push_back(w);
    if (!GTK_IS_CONTAINER(w)) return;
    GList* kids = gtk_container_get_children(GTK_CONTAINER(w));
    for (GList* l = kids; l; l = l->next) find_all(GTK_WIDGET(l->data), type, out);
    g_list_free(kids);
}
GtkWidget* window_titled(const char* title)
{
    GtkWidget* found = nullptr;
    GList* tops = gtk_window_list_toplevels();
    for (GList* l = tops; l && !found; l = l->next)
    {
        const char* t = GTK_IS_WINDOW(l->data) ? gtk_window_get_title(GTK_WINDOW(l->data)) : nullptr;
        if (t && std::string(t) == title) found = GTK_WIDGET(l->data);
    }
    g_list_free(tops);
    return found;
}
// The first widget of `type` in the window titled `title`.
GtkWidget* first_in(const char* title, GType type)
{
    std::vector<GtkWidget*> found;
    if (GtkWidget* top = window_titled(title)) find_all(top, type, found);
    return found.empty() ? nullptr : found[0];
}

// The notebook's tabs as shown: the titles of its visible pages, in order.
std::string notebook_tabs(GtkWidget* nb)
{
    std::string out;
    for (int i = 0; i < gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)); ++i)
    {
        GtkWidget* page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), i);
        if (!gtk_widget_get_visible(page)) continue;
        out += gtk_notebook_get_tab_label_text(GTK_NOTEBOOK(nb), page);
    }
    return out;
}
// The sidebar's rows as shown: the visible rows' labels, in order.
std::string sidebar_rows(GtkWidget* list)
{
    std::string out;
    for (int i = 0; GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), i); ++i)
    {
        if (!gtk_widget_get_visible(GTK_WIDGET(row))) continue;
        out += gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(row))));
    }
    return out;
}
int row_count(GtkWidget* list)
{
    int n = 0;
    while (gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), n)) ++n;
    return n;
}
// The page widget the sidebar's stack shows ("" with none).
std::string stack_shown(GtkWidget* stack)
{
    const char* name = gtk_stack_get_visible_child_name(GTK_STACK(stack));
    GtkWidget* child = gtk_stack_get_visible_child(GTK_STACK(stack));
    return child && gtk_widget_get_visible(child) && name ? name : "";
}

GtkRequisition natural(GtkWidget* w)
{
    GtkRequisition min{}, nat{};
    gtk_widget_get_preferred_size(w, &min, &nat);
    return nat;
}
GtkRequisition minimum(GtkWidget* w)
{
    GtkRequisition min{}, nat{};
    gtk_widget_get_preferred_size(w, &min, &nat);
    return min;
}

void run_api(rap::ui& ui)
{
    using namespace rap::layout;
    for (const bool sidebar : {false, true})
    {
        const std::string kind = sidebar ? "sidebar: " : "strip: ";
        const char* title = sidebar ? "Side API" : "Strip API";
        rap::tabs* t = nullptr;
        rap::label* lazy_label = nullptr;
        int changes = 0;
        auto win = ui.create(window{title,
                                    {tabs{{"A", label{"a"}},
                                          {"B", label{"b"}},
                                          {"C", label{"c"}},
                                          {"D", lazy{[&] { return rap::node(label{"d"}.bind(lazy_label)); }}}}
                                         .sidebar(sidebar)
                                         .on_changed([&](int) { ++changes; })
                                         .bind(t)}});
        settle();
        GtkWidget* nb = sidebar ? nullptr : first_in(title, GTK_TYPE_NOTEBOOK);
        GtkWidget* list = sidebar ? first_in(title, GTK_TYPE_LIST_BOX) : nullptr;
        GtkWidget* stack = sidebar ? first_in(title, GTK_TYPE_STACK) : nullptr;
        expect(t && (sidebar ? list && stack : nb != nullptr), kind + "bound, and its widgets found");
        if (!t || (sidebar ? !list || !stack : !nb)) return;
        auto shown = [&] { return sidebar ? sidebar_rows(list) : notebook_tabs(nb); };
        auto count = [&] { return sidebar ? row_count(list) : gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)); };
        auto native_current = [&]() -> std::string
        {
            if (sidebar) return stack_shown(stack);
            const int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
            return i >= 0 && gtk_widget_get_visible(gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), i)) ? str(i) : "";
        };
        expect(t->current() == 0 && shown() == "ABCD", kind + "four tabs, the first current: " + shown());
        const int tall = sidebar ? natural(list).height : 0;

        // A middle page hidden: its tab gone, the others keep their indices.
        t->set_page_visible(1, false);
        settle();
        expect(shown() == "ACD" && count() == 4, kind + "page 1 hidden: tabs ACD, still 4 pages: " + shown());
        expect(!t->page_visible(1) && t->page_visible(0) && t->page_visible(2), kind + "page_visible: 1 hidden, 0 and 2 not");
        if (sidebar)
        {
            const int now = natural(list).height;
            expect(now < tall, kind + "measured with a row fewer: shorter, " + str(tall) + " to " + str(now));
        }
        t->set_current(2);
        expect(t->current() == 2 && native_current() == "2", kind + "set_current(2): page 2, " + native_current());
        // set_current on a hidden page: nothing.
        t->set_current(1);
        expect(t->current() == 2 && native_current() == "2", kind + "set_current(hidden 1): still 2");
        changes = 0;

        // The current page hidden: the nearest visible after it (the lazy D,
        // built now), without on_changed.
        t->set_page_visible(2, false);
        settle();
        expect(t->current() == 3 && native_current() == "3",
               kind + "page 2 (current) hidden: 3 current, " + str(t->current()) + ", shown " + native_current());
        expect(lazy_label != nullptr, kind + "the lazy page, made current, built");
        expect(shown() == "AD", kind + "tabs AD: " + shown());
        // The last page hidden while current: the nearest before it.
        t->set_page_visible(3, false);
        settle();
        expect(t->current() == 0 && native_current() == "0",
               kind + "page 3 (current, last) hidden: 0 current, " + str(t->current()));
        // Every page hidden: -1, nothing shown.
        t->set_page_visible(0, false);
        settle();
        expect(t->current() == -1, kind + "every page hidden: current -1, " + str(t->current()));
        expect(shown().empty() && native_current().empty(), kind + "every page hidden: no tab, no page shown");
        // Shown again: current again (none was); another shown keeps it, and
        // its tab goes back in its place.
        t->set_page_visible(2, true);
        settle();
        expect(t->current() == 2 && native_current() == "2", kind + "page 2 shown with none current: 2 current");
        t->set_page_visible(0, true);
        t->set_page_visible(1, true);
        settle();
        expect(t->current() == 2 && native_current() == "2", kind + "pages 0 and 1 shown: current stays 2");
        expect(shown() == "ABC", kind + "B back between A and C: " + shown());
        t->set_page_visible(1, true);
        expect(t->current() == 2, kind + "page 1 shown twice: current stays 2");
        expect(changes == 0, kind + "no on_changed from any of it, " + str(changes));
        t->set_page_visible(9, false);
        expect(!t->page_visible(9) && !t->page_visible(-1), kind + "out of range: not visible");
        // The user's own choice still says so.
        t->set_current(0);
        expect(changes == 1 && t->current() == 0, kind + "set_current(0): on_changed as before, " + str(changes));
    }
}

void run_hidden_from_start(rap::ui& ui)
{
    using namespace rap::layout;
    for (const bool sidebar : {false, true})
    {
        const std::string kind = sidebar ? "sidebar, hidden from the start: " : "strip, hidden from the start: ";
        const char* title = sidebar ? "Side hidden" : "Strip hidden";
        rap::tabs* t = nullptr;
        rap::label* lazy_label = nullptr;
        int changes = 0;
        auto win = ui.create(window{
            title,
            {tabs{page{"A", lazy{[&] { return rap::node(label{"a"}.bind(lazy_label)); }}}.visible(false),
                  page{"B", label{"b"}}.visible(false),
                  {"C", label{"c"}}}
                 .sidebar(sidebar)
                 .hide_single_tab()
                 .on_changed([&](int) { ++changes; })
                 .bind(t)}});
        settle();
        GtkWidget* nb = sidebar ? nullptr : first_in(title, GTK_TYPE_NOTEBOOK);
        GtkWidget* list = sidebar ? first_in(title, GTK_TYPE_LIST_BOX) : nullptr;
        GtkWidget* stack = sidebar ? first_in(title, GTK_TYPE_STACK) : nullptr;
        GtkWidget* box = sidebar && list ? gtk_widget_get_parent(list) : nullptr;
        expect(t && (sidebar ? list && stack : nb != nullptr), kind + "bound, and its widgets found");
        if (!t || (sidebar ? !list || !stack : !nb)) return;
        auto shown = [&] { return sidebar ? sidebar_rows(list) : notebook_tabs(nb); };
        auto strip = [&]
        {
            return sidebar ? bool(gtk_widget_get_visible(list)) : bool(gtk_notebook_get_show_tabs(GTK_NOTEBOOK(nb)));
        };
        GtkWidget* page_c = sidebar ? gtk_stack_get_child_by_name(GTK_STACK(stack), "2")
                                    : gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), 2);
        GtkWidget* page_b = sidebar ? gtk_stack_get_child_by_name(GTK_STACK(stack), "1")
                                    : gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), 1);
        expect(!t->page_visible(0) && !t->page_visible(1) && t->page_visible(2), kind + "A and B hidden, C not");
        expect(shown() == "C", kind + "C's tab alone: " + shown());
        expect(!gtk_widget_get_visible(page_b), kind + "B's page never shown");
        expect(t->current() == 2, kind + "C current, " + str(t->current()));
        expect(lazy_label == nullptr, kind + "the lazy A unbuilt");
        expect(changes == 0, kind + "no on_changed at build, " + str(changes));

        // hide_single_tab: one page visible, no strip, the page filling it.
        // The page's margins made 10 px, unlike the tabs' own 6 px (the window's
        // round its content): equal, counting them twice or not at all would
        // measure the same.
        gtk_widget_set_margin_start(page_c, 10);
        gtk_widget_set_margin_end(page_c, 10);
        gtk_widget_set_margin_top(page_c, 10);
        gtk_widget_set_margin_bottom(page_c, 10);
        settle();
        expect(!strip(), kind + "one page visible: no strip");
        GtkWidget* outer = sidebar ? box : nb;
        // Preferred sizes count a widget's own margins: the tabs' (the
        // window's margin round its content) taken off, they're the page's
        // own preferred size, its margins already in it.
        GtkRequisition single = natural(outer), single_min = minimum(outer);
        const int mx = gtk_widget_get_margin_start(outer) + gtk_widget_get_margin_end(outer);
        const int my = gtk_widget_get_margin_top(outer) + gtk_widget_get_margin_bottom(outer);
        single.width -= mx, single_min.width -= mx, single.height -= my, single_min.height -= my;
        const GtkRequisition page = natural(page_c), page_min = minimum(page_c);
        if (sidebar)
        {
            expect(single.width == page.width && single_min.width == page_min.width,
                   kind + "no list: as wide as its page, " + str(single.width) + " for " + str(page.width) +
                       " (minimum " + str(single_min.width) + " for " + str(page_min.width) + ")");
        }
        else
        {
            expect(single.height == page.height && single.width == page.width,
                   kind + "no tabs: measured as its page, " + str(single.width) + "x" + str(single.height) + " for " +
                       str(page.width) + "x" + str(page.height));
            expect(single_min.height == page_min.height,
                   kind + "no tabs: its minimum height its page's, " + str(single_min.height) + " for " +
                       str(page_min.height));
        }

        // A second page shown: the strip's back.
        t->set_page_visible(0, true);
        settle();
        expect(strip(), kind + "A shown: the strip back");
        expect(shown() == "AC", kind + "tabs AC: " + shown());
        expect(t->current() == 2 && lazy_label == nullptr, kind + "A shown: C stays current, A still unbuilt");
        GtkRequisition two = natural(outer), two_min = minimum(outer);
        two.width -= mx, two_min.width -= mx, two.height -= my, two_min.height -= my;
        if (sidebar)
            expect(two.width > single.width && two_min.width > single_min.width,
                   kind + "the list measured again: wider, " + str(single.width) + " to " + str(two.width));
        else
            expect(two.height > single.height && two_min.height > single_min.height,
                   kind + "the tabs measured again: taller, " + str(single.height) + " to " + str(two.height));
        // Hidden again: gone again.
        t->set_page_visible(0, false);
        settle();
        expect(!strip(), kind + "A hidden again: no strip");
        GtkRequisition again = natural(outer);
        again.width -= mx, again.height -= my;  // (its own margins off, as single's)
        expect(again.width == single.width && again.height == single.height,
               kind + "measured as before, " + str(again.width) + "x" + str(again.height));
        t->set_page_visible(0, true);
        t->set_current(0);
        expect(t->current() == 0 && lazy_label != nullptr, kind + "A made current: built");
        expect(changes == 1, kind + "on_changed once, for set_current, " + str(changes));
    }
}

// #92: a canvas of a fixed size as a page's content, the page's container
// measured to hold it.
void run_fixed_canvas(rap::ui& ui)
{
    using namespace rap::layout;
    using namespace rap::literals;
    auto win = ui.create(window{"Fixed canvas",
                                {hbox{tabs{{"Canvas", canvas{}.width(300_px).height(220_px)}, {"Other", label{"x"}}},
                                      tabs{{"Canvas", canvas{}.width(300_px).height(220_px)}}.sidebar(),
                                      stack{{"canvas", canvas{}.width(300_px).height(220_px)}, {"other", label{"y"}}}}}});
    settle();
    GtkWidget* nb = first_in("Fixed canvas", GTK_TYPE_NOTEBOOK);
    std::vector<GtkWidget*> stacks;
    if (GtkWidget* top = window_titled("Fixed canvas")) find_all(top, GTK_TYPE_STACK, stacks);
    expect(nb && stacks.size() == 2, "#92: a notebook, a sidebar's stack and a stack");
    if (!nb || stacks.size() != 2) return;
    for (auto [what, w] : {std::pair{"a tab page", nb}, std::pair{"a sidebar page", stacks[0]},
                          std::pair{"a stack page", stacks[1]}})
    {
        const GtkRequisition n = natural(w), m = minimum(w);
        expect(n.width >= 300 && n.height >= 220 && m.width >= 300 && m.height >= 220,
               std::string("#92: a 300x220 canvas as ") + what + ": measured " + str(n.width) + "x" + str(n.height) +
                   " (minimum " + str(m.width) + "x" + str(m.height) + ")");
    }
}

// Ctrl+Tab (Ctrl+Shift+Tab with `shift`) through the window's own key
// handling, as document_tabs_input_test sends it: a press and a release on
// the toplevel's GdkWindow, given to gtk_main_do_event.
void ctrl_tab(GtkWidget* top, bool shift)
{
    static guint32 time = 1000;
    const guint keyval = shift ? GDK_KEY_ISO_Left_Tab : GDK_KEY_Tab;
    GdkWindow* window = gtk_widget_get_window(top);
    for (GdkEventType type : {GDK_KEY_PRESS, GDK_KEY_RELEASE})
    {
        GdkEvent* e = gdk_event_new(type);
        e->key.window = GDK_WINDOW(g_object_ref(window));
        e->key.send_event = true;
        e->key.time = ++time;
        e->key.keyval = keyval;
        e->key.state = GDK_CONTROL_MASK | (shift ? GDK_SHIFT_MASK : 0);
        GdkKeymapKey* keys = nullptr;
        gint n = 0;
        if (gdk_keymap_get_entries_for_keyval(gdk_keymap_get_for_display(gdk_display_get_default()), keyval, &keys,
                                              &n) &&
            n > 0)
            e->key.hardware_keycode = guint16(keys[0].keycode);
        g_free(keys);
        e->key.string = g_strdup("");
        gdk_event_set_device(e, gdk_seat_get_keyboard(gdk_display_get_default_seat(gdk_display_get_default())));
        gtk_main_do_event(e);
        gdk_event_free(e);
    }
    settle();
}

// A key that never returns (Ctrl+Tab going round pages none of which it
// can go to) fails the run rather than hanging it.
void timed_out(int)
{
    static const char what[] = "FAIL Ctrl+Tab still going after 20 s: it never returned\n";
    (void)!write(STDOUT_FILENO, what, sizeof what - 1);
    _exit(1);
}
struct bounded
{
    bounded()
    {
        std::signal(SIGALRM, timed_out);
        alarm(20);
    }
    ~bounded() { alarm(0); }
};

// Ctrl+Tab with the focus in a page's tabs (layout::tabs) goes round their
// visible pages: none with no pages, a hidden one skipped, nothing with
// every one hidden. The window has document tabs (their Ctrl+Tab is what
// the page's tabs take it from), and is shown: GTK 3 gives a key only to a
// window that's viewable.
void run_ctrl_tab(rap::ui& ui)
{
    using namespace rap::layout;
    int selected = 0;  // the document tabs' on_select: Ctrl+Tab gone to them
    auto docs = [&]
    {
        return document_tabs{{{"a", "Alpha", {}, {}}, {"b", "Beta", {}, {}}}}.on_select([&](const std::string&)
                                                                                         { ++selected; });
    };
    // Shown, the notebook found and focused: the toplevel, or null.
    auto ready = [](rap::window& win, const char* title, GtkWidget*& nb) -> GtkWidget*
    {
        win.show();
        settle();
        GtkWidget* top = window_titled(title);
        // The document tabs' notebook comes first; the page's tabs' is the other.
        std::vector<GtkWidget*> all;
        if (top) find_all(top, GTK_TYPE_NOTEBOOK, all);
        nb = all.size() == 2 ? all[1] : nullptr;
        if (!nb) return nullptr;
        gtk_widget_grab_focus(nb);
        settle();
        return gtk_widget_get_window(top) && gtk_window_get_focus(GTK_WINDOW(top)) == nb ? top : nullptr;
    };

    // 1. No pages at all: nothing, and no crash.
    {
        rap::tabs* t = nullptr;
        int changes = 0;
        auto win = ui.create(window{"Ctrl+Tab none", {tabs{}.on_changed([&](int) { ++changes; }).bind(t)}}.tabs(docs()));
        GtkWidget* nb = nullptr;
        GtkWidget* top = ready(*win, "Ctrl+Tab none", nb);
        expect(t && top, "Ctrl+Tab, no pages: shown, the page's notebook focused");
        if (t && top)
        {
            selected = 0;
            {
                bounded guard;
                ctrl_tab(top, false);
                ctrl_tab(top, true);
            }
            expect(gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)) == 0 &&
                       gtk_notebook_get_current_page(GTK_NOTEBOOK(nb)) == -1 && t->current() == -1,
                   "Ctrl+Tab and Ctrl+Shift+Tab, no pages: nothing, current " + str(t->current()));
            expect(changes == 0 && selected == 0, "Ctrl+Tab, no pages: no on_changed, " + str(changes) +
                                                      ", and not the document tabs', " + str(selected));
        }
    }

    // 2. A, B (hidden), C, D, A current: Ctrl+Tab to C, Ctrl+Shift+Tab to D.
    {
        rap::tabs* t = nullptr;
        std::vector<int> changes;
        auto win = ui.create(window{"Ctrl+Tab skip", {tabs{{"A", label{"a"}},
                                                            page{"B", label{"b"}}.visible(false),
                                                            {"C", label{"c"}},
                                                            {"D", label{"d"}}}
                                                           .on_changed([&](int i) { changes.push_back(i); })
                                                           .bind(t)}}
                                 .tabs(docs()));
        GtkWidget* nb = nullptr;
        GtkWidget* top = ready(*win, "Ctrl+Tab skip", nb);
        expect(t && top, "Ctrl+Tab, B hidden: shown, the page's notebook focused");
        if (t && top)
        {
            expect(t->current() == 0 && notebook_tabs(nb) == "ACD",
                   "Ctrl+Tab, B hidden: tabs " + notebook_tabs(nb) + ", A current, " + str(t->current()));
            selected = 0;
            {
                bounded guard;
                ctrl_tab(top, false);
            }
            expect(t->current() == 2 && gtk_notebook_get_current_page(GTK_NOTEBOOK(nb)) == 2,
                   "Ctrl+Tab from A: C, B skipped, " + str(t->current()) + " (native " +
                       str(gtk_notebook_get_current_page(GTK_NOTEBOOK(nb))) + ")");
            t->set_current(0);
            settle();
            gtk_widget_grab_focus(nb);
            changes.clear();
            {
                bounded guard;
                ctrl_tab(top, true);
            }
            expect(t->current() == 3 && gtk_notebook_get_current_page(GTK_NOTEBOOK(nb)) == 3,
                   "Ctrl+Shift+Tab from A: D, " + str(t->current()) + " (native " +
                       str(gtk_notebook_get_current_page(GTK_NOTEBOOK(nb))) + ")");
            expect(changes == std::vector<int>{3}, "Ctrl+Shift+Tab from A: on_changed once, with D");
            expect(selected == 0, "Ctrl+Tab, B hidden: not the document tabs', " + str(selected));
        }
    }

    // 3. Every page hidden, none current: Ctrl+Tab returns, nothing changed.
    {
        rap::tabs* t = nullptr;
        int changes = 0;
        auto win = ui.create(window{"Ctrl+Tab all hidden",
                                    {tabs{page{"A", label{"a"}}.visible(false), page{"B", label{"b"}}.visible(false),
                                          page{"C", label{"c"}}.visible(false)}
                                         .on_changed([&](int) { ++changes; })
                                         .bind(t)}}
                                 .tabs(docs()));
        GtkWidget* nb = nullptr;
        GtkWidget* top = ready(*win, "Ctrl+Tab all hidden", nb);
        expect(t && top, "Ctrl+Tab, every page hidden: shown, the page's notebook focused");
        if (t && top)
        {
            const int native_before = gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
            expect(t->current() == -1 && notebook_tabs(nb).empty(),
                   "every page hidden: current -1, " + str(t->current()) + ", no tab (native current " +
                       str(native_before) + ")");
            selected = 0;
            {
                bounded guard;
                ctrl_tab(top, false);
                ctrl_tab(top, true);
            }
            const int native_after = gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
            expect(t->current() == -1 && native_after == native_before && notebook_tabs(nb).empty(),
                   "Ctrl+Tab and Ctrl+Shift+Tab, every page hidden: returned, nothing changed, current " +
                       str(t->current()) + " (native " + str(native_after) + ")");
            expect(changes == 0 && selected == 0, "every page hidden: no on_changed, " + str(changes) +
                                                      ", and not the document tabs', " + str(selected));
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options options;
    options.backend = "gtk3";
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui)
    {
        std::printf("FAIL no gtk3 backend: %s\n", ui.error().c_str());
        return 1;
    }
    run_api(**ui);
    run_hidden_from_start(**ui);
    run_fixed_canvas(**ui);
    run_ctrl_tab(**ui);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
