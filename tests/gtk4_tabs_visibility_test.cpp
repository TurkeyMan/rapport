// Tab page visibility on GTK 4 (rap::tabs::set_page_visible,
// layout::page::visible, tabs::hide_single_tab), read from the native
// widgets: a notebook's tabs and pages, a sidebar's rows and stack. Indices
// stay stable, the current page moves to the nearest visible one without
// on_changed, -1 with none visible, a page shown again is back in its place,
// set_current on a hidden page does nothing, pages hidden from the start
// never show (a lazy one unbuilt), a sidebar measures only its visible rows,
// and hide_single_tab takes the strip (or the list) away and back, measured.
// And #92: a fixed-size canvas as a tab page's, a stack page's and a
// window's root content measures to its fixed size. And Ctrl+Tab with the
// focus in a page's tabs: round their visible pages (a hidden one skipped),
// nothing with no pages or none visible, and never stuck.
// Never shown: windows are made and measured, not presented.
//   rapport-tabs-visibility-test-gtk4   (needs a display; run it through offscreen.sh)
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <unistd.h>

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}

std::string str(long v)
{
    return std::to_string(v);
}

GtkWidget* window_titled(const char* title)
{
    GListModel* tops = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(tops); ++i)
    {
        auto* w = GTK_WIDGET(g_list_model_get_item(tops, i));
        g_object_unref(w);
        const char* t = gtk_window_get_title(GTK_WINDOW(w));
        if (t && std::string(t) == title) return w;
    }
    return nullptr;
}

// The first widget under `w` (itself too) of type `type`.
GtkWidget* first_of(GtkWidget* w, GType type)
{
    if (!w) return nullptr;
    if (G_TYPE_CHECK_INSTANCE_TYPE(w, type)) return w;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* f = first_of(c, type)) return f;
    return nullptr;
}

int natural(GtkWidget* w, GtkOrientation o)
{
    int min = 0, nat = 0;
    gtk_widget_measure(w, o, -1, &min, &nat, nullptr, nullptr);
    return nat;
}
int nat_w(GtkWidget* w)
{
    return natural(w, GTK_ORIENTATION_HORIZONTAL);
}
int nat_h(GtkWidget* w)
{
    return natural(w, GTK_ORIENTATION_VERTICAL);
}
// Its natural size within its margins (rapport's page and window margins).
std::string inner(GtkWidget* w)
{
    return std::to_string(nat_w(w) - gtk_widget_get_margin_start(w) - gtk_widget_get_margin_end(w)) + " x " +
           std::to_string(nat_h(w) - gtk_widget_get_margin_top(w) - gtk_widget_get_margin_bottom(w));
}

// A notebook's tabs as its strip has them, in order: each visible tab's
// label's text ("ACD").
std::string notebook_headers(GtkWidget* nb)
{
    auto* n = GTK_NOTEBOOK(nb);
    if (gtk_notebook_get_n_pages(n) == 0) return "";
    GtkWidget* label = gtk_notebook_get_tab_label(n, gtk_notebook_get_nth_page(n, 0));
    GtkWidget* strip = label ? gtk_widget_get_parent(gtk_widget_get_parent(label)) : nullptr;
    std::string s;
    if (!strip) return "?";
    for (GtkWidget* t = gtk_widget_get_first_child(strip); t; t = gtk_widget_get_next_sibling(t))
    {
        GtkWidget* l = gtk_widget_get_first_child(t);
        if (std::string(gtk_widget_get_css_name(t)) == "tab" && gtk_widget_get_visible(t) && l &&
            gtk_widget_get_visible(l) && GTK_IS_LABEL(l))
            s += gtk_label_get_text(GTK_LABEL(l));
    }
    return s;
}
// The notebook's pages shown: the visible page children's indices.
std::string notebook_pages_shown(GtkWidget* nb)
{
    auto* n = GTK_NOTEBOOK(nb);
    std::string s;
    for (int i = 0; i < gtk_notebook_get_n_pages(n); ++i)
        if (gtk_widget_get_visible(gtk_notebook_get_nth_page(n, i))) s += char('0' + i);
    return s;
}
// A sidebar's rows, in order: each visible row's label's text.
std::string list_headers(GtkWidget* list)
{
    std::string s;
    for (int i = 0;; ++i)
    {
        GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), i);
        if (!row) break;
        GtkWidget* l = gtk_list_box_row_get_child(row);
        if (gtk_widget_get_visible(GTK_WIDGET(row)) && l && GTK_IS_LABEL(l)) s += gtk_label_get_text(GTK_LABEL(l));
    }
    return s;
}
int list_rows(GtkWidget* list)
{
    int n = 0;
    while (gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), n)) ++n;
    return n;
}
// The stack page the sidebar shows: its index, or -1.
int stack_shown(GtkWidget* stack)
{
    GtkWidget* c = gtk_stack_get_visible_child(GTK_STACK(stack));
    if (!c || !gtk_widget_get_visible(c) || !gtk_stack_page_get_visible(gtk_stack_get_page(GTK_STACK(stack), c)))
        return -1;
    const char* name = gtk_stack_get_visible_child_name(GTK_STACK(stack));
    return name ? std::atoi(name) : -1;
}
// The stack pages visible (the widget and GtkStackPage:visible), by name.
std::string stack_pages_shown(GtkWidget* stack)
{
    std::string s;
    for (int i = 0; i < 16; ++i)
        if (GtkWidget* c = gtk_stack_get_child_by_name(GTK_STACK(stack), std::to_string(i).c_str()))
            if (gtk_widget_get_visible(c) && gtk_stack_page_get_visible(gtk_stack_get_page(GTK_STACK(stack), c)))
                s += char('0' + i);
    return s;
}

// A sidebar's or a strip's native reads, either way.
struct native_tabs
{
    GtkWidget* nb = nullptr;     // a strip's notebook
    GtkWidget* list = nullptr;   // a sidebar's list
    GtkWidget* stack = nullptr;  // and its stack
    std::string headers() const { return nb ? notebook_headers(nb) : list_headers(list); }
    // Page `i`'s widget the only one the notebook or stack shows (visible,
    // and child-visible: GTK clears that on the pages it isn't showing).
    bool only_shown(int i) const
    {
        for (int j = 0;; ++j)
        {
            GtkWidget* page = nb ? (j < gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb))
                                        ? gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), j)
                                        : nullptr)
                                 : gtk_stack_get_child_by_name(GTK_STACK(stack), std::to_string(j).c_str());
            if (!page) return true;
            const bool on = gtk_widget_get_visible(page) && gtk_widget_get_child_visible(page);
            if (on != (j == i)) return false;
        }
    }
    std::string pages() const { return nb ? notebook_pages_shown(nb) : stack_pages_shown(stack); }
    int count() const { return nb ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)) : list_rows(list); }
    // The page natively current, or -1 when no page shows.
    int shown() const
    {
        if (!nb) return stack_shown(stack);
        const int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
        return i >= 0 && gtk_widget_get_visible(gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), i)) ? i : -1;
    }
};
native_tabs find_tabs(GtkWidget* top, bool sidebar)
{
    native_tabs n;
    if (sidebar)
    {
        n.list = first_of(top, GTK_TYPE_LIST_BOX);
        n.stack = first_of(top, GTK_TYPE_STACK);
    }
    else
        n.nb = first_of(top, GTK_TYPE_NOTEBOOK);
    return n;
}

void visibility(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "sidebar: " : "strip: ";
    const std::string title = sidebar ? "Tabs sidebar" : "Tabs strip";
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
    GtkWidget* top = window_titled(title.c_str());
    const native_tabs n = top ? find_tabs(top, sidebar) : native_tabs{};
    expect(t && top && (n.nb || (n.list && n.stack)), kind + "bound, its native widgets found");
    if (!t || !top || !(n.nb || (n.list && n.stack))) return;
    expect(n.headers() == "ABCD" && t->current() == 0 && n.shown() == 0,
           kind + "ABCD, the first current: " + n.headers() + ", " + str(t->current()));

    // A middle page hidden: its header gone, the others keep their indices.
    t->set_page_visible(1, false);
    expect(n.headers() == "ACD" && n.count() == 4, kind + "page 1 hidden: headers ACD, 4 pages still: " +
                                                       n.headers() + ", " + str(n.count()));
    expect(n.pages() == "023", kind + "page 1 hidden: its page too: " + n.pages());
    expect(!t->page_visible(1) && t->page_visible(0) && t->page_visible(2), kind + "page 1 hidden, 0 and 2 not");
    t->set_current(2);
    expect(t->current() == 2 && n.shown() == 2, kind + "page 2 is still 2: " + str(n.shown()));
    // set_current on a hidden page: nothing.
    t->set_current(1);
    expect(t->current() == 2 && n.shown() == 2, kind + "set_current(hidden 1): still 2, " + str(n.shown()));

    // The current page hidden: the nearest visible after it (the lazy D,
    // built now), without on_changed.
    changes = 0;
    expect(lazy_label == nullptr, kind + "the lazy page unbuilt yet");
    t->set_page_visible(2, false);
    expect(t->current() == 3 && n.shown() == 3,
           kind + "page 2 (current) hidden: 3 current, " + str(t->current()) + ", shown " + str(n.shown()));
    expect(n.headers() == "AD", kind + "headers AD: " + n.headers());
    expect(lazy_label != nullptr, kind + "the lazy page, made current, built");
    // The last page hidden while current: the nearest before it.
    t->set_page_visible(3, false);
    expect(t->current() == 0 && n.shown() == 0,
           kind + "page 3 (current, last) hidden: 0 current, " + str(t->current()) + ", shown " + str(n.shown()));
    // All hidden: -1, and no page shows.
    t->set_page_visible(0, false);
    expect(t->current() == -1, kind + "every page hidden: current -1, " + str(t->current()));
    expect(n.headers().empty() && n.pages().empty() && n.shown() == -1,
           kind + "every page hidden: no header, no page: " + n.headers() + ", " + n.pages() + ", " + str(n.shown()));
    // Shown again: current again (it was -1); another shown keeps it, its
    // header back in its place.
    t->set_page_visible(2, true);
    expect(t->current() == 2 && n.shown() == 2, kind + "page 2 shown again with none current: 2 current, " +
                                                    str(t->current()) + ", shown " + str(n.shown()));
    t->set_page_visible(3, true);
    t->set_page_visible(1, true);
    expect(t->current() == 2 && n.shown() == 2 && t->page_visible(1), kind + "pages 3 and 1 shown again: current stays 2");
    expect(n.headers() == "BCD", kind + "B back before C: BCD, " + n.headers());
    t->set_page_visible(0, true);
    expect(n.headers() == "ABCD" && n.pages() == "0123", kind + "A back first: ABCD, " + n.headers());
    // The same again: nothing.
    t->set_page_visible(1, true);
    expect(t->current() == 2 && n.headers() == "ABCD", kind + "page 1 shown again, twice: nothing");
    expect(changes == 0, kind + "no on_changed from any of it, " + str(changes));
    // Out of range: nothing.
    t->set_page_visible(9, false);
    t->set_page_visible(-1, false);
    expect(!t->page_visible(9) && !t->page_visible(-1) && n.headers() == "ABCD", kind + "pages out of range: nothing");
}

// Hidden from the start, with hide_single_tab: C current, no strip (or
// list), the page filling the area, A (lazy) unbuilt; the strip back with a
// second page, and gone again with it.
void hidden_from_start(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "sidebar, hidden from the start: " : "strip, hidden from the start: ";
    const std::string title = sidebar ? "Hidden sidebar" : "Hidden strip";
    rap::tabs* t = nullptr;
    rap::label* lazy_label = nullptr;
    int changes = 0;
    auto win = ui.create(
        window{title,
               {tabs{page{"A", lazy{[&] { return rap::node(label{"a"}.bind(lazy_label)); }}}.visible(false),
                     page{"B", label{"b"}}.visible(false),
                     {"C", label{"c"}}}
                    .sidebar(sidebar)
                    .hide_single_tab()
                    .on_changed([&](int) { ++changes; })
                    .bind(t)}});
    GtkWidget* top = window_titled(title.c_str());
    const native_tabs n = top ? find_tabs(top, sidebar) : native_tabs{};
    expect(t && top && (n.nb || (n.list && n.stack)), kind + "bound, its native widgets found");
    if (!t || !top || !(n.nb || (n.list && n.stack))) return;
    expect(!t->page_visible(0) && !t->page_visible(1) && t->page_visible(2), kind + "A and B hidden, C not");
    expect(t->current() == 2 && n.shown() == 2, kind + "C current, " + str(t->current()) + ", shown " + str(n.shown()));
    // (A notebook without its tabs hides their labels too: no header at all.)
    const std::string alone_headers = sidebar ? "C" : "";
    expect(n.headers() == alone_headers && n.pages() == "2",
           kind + "C's page alone, its header " + (sidebar ? "C" : "hidden too") + ": " + n.headers() + ", " + n.pages());
    expect(lazy_label == nullptr, kind + "the lazy A unbuilt");

    // The strip: hidden, and the page fills the area (the tabs measure to
    // the page and its margins, nothing more).
    GtkWidget* whole = n.nb ? n.nb : gtk_widget_get_parent(n.list);
    GtkWidget* page_c = n.nb ? gtk_notebook_get_nth_page(GTK_NOTEBOOK(n.nb), 2)
                             : gtk_stack_get_child_by_name(GTK_STACK(n.stack), "2");
    // The page's margins made 10 px, unlike the tabs' own 6 px (the window's
    // round its content): equal, counting them twice or not at all would
    // measure the same.
    gtk_widget_set_margin_start(page_c, 10);
    gtk_widget_set_margin_end(page_c, 10);
    gtk_widget_set_margin_top(page_c, 10);
    gtk_widget_set_margin_bottom(page_c, 10);
    auto strip_shown = [&]
    {
        return n.nb ? bool(gtk_notebook_get_show_tabs(GTK_NOTEBOOK(n.nb)))
                    : gtk_widget_get_visible(n.list) && gtk_widget_get_visible(gtk_widget_get_next_sibling(n.list));
    };
    // Across the strip: a notebook's tabs are above its page, a sidebar's
    // list beside it.
    // gtk_widget_measure counts a widget's own margins: the tabs' (the
    // window's margin round its content) taken off, they measure as the
    // page, its margins already in its size.
    auto across = [&](GtkWidget* w) { return n.nb ? nat_h(w) : nat_w(w); };
    const int whole_margins = n.nb ? gtk_widget_get_margin_top(whole) + gtk_widget_get_margin_bottom(whole)
                                   : gtk_widget_get_margin_start(whole) + gtk_widget_get_margin_end(whole);
    const int alone = across(whole) - whole_margins, page_size = across(page_c);
    expect(!strip_shown(), kind + "one page: no strip");
    expect(alone == page_size, kind + "one page: the page fills the tabs, " + str(page_size) + ": " + str(alone));
    if (n.nb)
        expect(!gtk_notebook_get_show_border(GTK_NOTEBOOK(n.nb)), kind + "one page: no frame either");

    // A second page: the strip back, measured.
    t->set_page_visible(1, true);
    const int two = across(whole) - whole_margins;
    expect(strip_shown(), kind + "B shown: the strip back");
    expect(two > alone, kind + "B shown: the strip measured, " + str(two) + " > " + str(alone));
    expect(n.headers() == "BC" && t->current() == 2, kind + "B shown: BC, C current");
    // And gone with it.
    t->set_page_visible(1, false);
    expect(!strip_shown(), kind + "B hidden again: no strip");
    expect(across(whole) - whole_margins == alone,
           kind + "B hidden again: measured as before, " + str(across(whole) - whole_margins));

    t->set_page_visible(0, true);
    expect(t->current() == 2 && lazy_label == nullptr, kind + "A shown: C stays current, A still unbuilt");
    expect(strip_shown() && n.headers() == "AC", kind + "A shown: the strip, AC: " + n.headers());
    expect(changes == 0, kind + "no on_changed from any of it, " + str(changes));
    t->set_current(0);
    expect(t->current() == 0 && n.shown() == 0 && lazy_label != nullptr, kind + "A made current: built, shown");
}

void spin();

// Whether `w` (itself, or under it) is a shown label reading `text`.
bool holds_label(GtkWidget* w, const char* text)
{
    if (!w || !gtk_widget_get_visible(w)) return false;
    if (GTK_IS_LABEL(w) && std::string(gtk_label_get_text(GTK_LABEL(w))) == text) return true;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (holds_label(c, text)) return true;
    return false;
}
std::string seq(const std::vector<int>& v)
{
    std::string s;
    for (int i : v) s += (s.empty() ? "" : ",") + std::to_string(i);
    return "[" + s + "]";
}

// #98: set_current is the app's own: no on_changed, the page made current
// natively, a lazy one built and shown; to the current page, nothing. A lazy
// page made current by hiding the current one: built and shown, no
// on_changed. A user's selection, made on the native widget as a click's
// reaches rapport: on_changed once, with the page.
void set_current_quiet(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "sidebar, set_current (#98): " : "strip, set_current (#98): ";
    const std::string title = sidebar ? "Quiet sidebar" : "Quiet strip";
    rap::tabs* t = nullptr;
    rap::label* built[4] = {};
    std::vector<int> changes;
    auto lazy_page = [&built](int i, const char* text)
    { return lazy{[&built, i, text] { return rap::node(label{text}.bind(built[i])); }}; };
    auto win = ui.create(window{title,
                                {tabs{{"A", label{"page a"}},
                                      {"B", lazy_page(1, "page b")},
                                      {"C", lazy_page(2, "page c")},
                                      {"D", lazy_page(3, "page d")}}
                                     .sidebar(sidebar)
                                     .on_changed([&](int i) { changes.push_back(i); })
                                     .bind(t)}});
    GtkWidget* top = window_titled(title.c_str());
    const native_tabs n = top ? find_tabs(top, sidebar) : native_tabs{};
    expect(t && top && (n.nb || (n.list && n.stack)), kind + "bound, its native widgets found");
    if (!t || !top || !(n.nb || (n.list && n.stack))) return;
    // The page natively current: the notebook's, or the selected row's when
    // the stack shows the same page (-2 when they disagree).
    auto native = [&]() -> int
    {
        if (n.nb) return gtk_notebook_get_current_page(GTK_NOTEBOOK(n.nb));
        GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(n.list));
        const int r = row ? gtk_list_box_row_get_index(row) : -1;
        return stack_shown(n.stack) == r ? r : -2;
    };
    // The page natively shown holds the label `text`: the notebook's current
    // page, or the stack's visible child.
    auto showing = [&](const char* text) -> bool
    {
        GtkWidget* page = nullptr;
        if (n.stack)
            page = gtk_stack_get_visible_child(GTK_STACK(n.stack));
        else if (const int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(n.nb)); i >= 0)
            page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(n.nb), i);
        return holds_label(page, text);
    };
    expect(t->current() == 0 && native() == 0 && showing("page a") && !built[1] && !built[2] && !built[3],
           kind + "A current and shown, the lazy B, C and D unbuilt");
    changes.clear();

    // To the lazy B: no on_changed; current, natively too; built, shown.
    t->set_current(1);
    spin();
    expect(changes.empty(), kind + "set_current(1): no on_changed, " + seq(changes));
    expect(t->current() == 1 && native() == 1,
           kind + "set_current(1): B current, natively too, " + str(t->current()) + "/" + str(native()));
    expect(built[1] && showing("page b"), kind + "set_current(1): the lazy B built and shown");
    // The current page again: nothing.
    t->set_current(1);
    spin();
    expect(changes.empty(), kind + "set_current(1) again: no on_changed, " + seq(changes));
    expect(t->current() == 1 && native() == 1 && showing("page b") && !built[2] && !built[3],
           kind + "set_current(1) again: nothing changed, " + str(t->current()) + "/" + str(native()));
    // B (current) hidden: the lazy C, the nearest after it, built and shown,
    // no on_changed.
    t->set_page_visible(1, false);
    spin();
    expect(t->current() == 2 && native() == 2,
           kind + "B (current) hidden: C current, natively too, " + str(t->current()) + "/" + str(native()));
    expect(built[2] && showing("page c"), kind + "B (current) hidden: the lazy C built and shown");
    expect(changes.empty(), kind + "B (current) hidden: no on_changed, " + seq(changes));

    // A user's selection of the lazy D, on the native widget: on_changed
    // once, with 3; built and shown.
    if (n.list)
        gtk_list_box_select_row(GTK_LIST_BOX(n.list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(n.list), 3));
    else
        gtk_notebook_set_current_page(GTK_NOTEBOOK(n.nb), 3);
    spin();
    expect(changes == std::vector<int>{3}, kind + "a user's selection of D: on_changed once, with 3, " + seq(changes));
    expect(t->current() == 3 && native() == 3 && built[3] && showing("page d"),
           kind + "a user's selection of D: D current, built and shown, " + str(t->current()) + "/" + str(native()));
    // set_current to the current page: nothing.
    changes.clear();
    t->set_current(3);
    spin();
    expect(changes.empty() && t->current() == 3 && native() == 3 && showing("page d"),
           kind + "set_current(3), current already: nothing, " + seq(changes));
    // Back to A, the app's: no on_changed.
    t->set_current(0);
    spin();
    expect(changes.empty() && t->current() == 0 && native() == 0 && showing("page a"),
           kind + "set_current(0): A current and shown, no on_changed, " + seq(changes));
}

// #98, re-entered: a lazy page's builder that calls the tabs back while the
// switch that built it is still being handled. The app's set_current to B,
// whose builder calls set_current(2): no on_changed, C current and shown. B
// (current) hidden, the lazy C made current, whose builder hides D: no
// on_changed, C current and shown, D's header gone. A user's switch to a
// lazy page: built, shown, on_changed once with it; to B whose builder calls
// set_current(2): C current and shown, on_changed once, with 2.
void reentrant(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    using hook = std::function<void(rap::tabs*)>;
    const std::string side = sidebar ? "sidebar" : "strip";
    // A window of A and the lazy B, C and D, page i's builder calling
    // then[i] first; `body` run on it.
    auto scenario = [&](const std::string& name, std::array<hook, 4> then,
                        const std::function<void(rap::tabs*, const native_tabs&, rap::label**, std::vector<int>&,
                                                 const std::string&)>& body)
    {
        const std::string kind = side + ", " + name + " (#98): ";
        const std::string title = "Reentrant " + side + " " + name;
        rap::tabs* t = nullptr;
        rap::label* built[4] = {};
        std::vector<int> changes;
        auto lazy_page = [&](int i, const char* text)
        {
            return lazy{[&, i, text]
                        {
                            if (then[size_t(i)]) then[size_t(i)](t);
                            return rap::node(label{text}.bind(built[i]));
                        }};
        };
        auto win = ui.create(window{title,
                                    {tabs{{"A", label{"page a"}},
                                          {"B", lazy_page(1, "page b")},
                                          {"C", lazy_page(2, "page c")},
                                          {"D", lazy_page(3, "page d")}}
                                         .sidebar(sidebar)
                                         .on_changed([&](int i) { changes.push_back(i); })
                                         .bind(t)}});
        GtkWidget* top = window_titled(title.c_str());
        const native_tabs n = top ? find_tabs(top, sidebar) : native_tabs{};
        expect(t && top && (n.nb || (n.list && n.stack)), kind + "bound, its native widgets found");
        if (!t || !top || !(n.nb || (n.list && n.stack))) return;
        spin();
        changes.clear();
        body(t, n, built, changes, kind);
    };
    // The page natively current: the notebook's, or the selected row's when
    // the stack shows the same page (-2 when they disagree).
    auto native = [](const native_tabs& n) -> int
    {
        if (n.nb) return gtk_notebook_get_current_page(GTK_NOTEBOOK(n.nb));
        GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(n.list));
        const int r = row ? gtk_list_box_row_get_index(row) : -1;
        return stack_shown(n.stack) == r ? r : -2;
    };
    // The page natively shown holds the label `text`.
    auto showing = [](const native_tabs& n, const char* text) -> bool
    {
        GtkWidget* page = nullptr;
        if (n.stack)
            page = gtk_stack_get_visible_child(GTK_STACK(n.stack));
        else if (const int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(n.nb)); i >= 0)
            page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(n.nb), i);
        return holds_label(page, text);
    };
    // A user's selection, on the native widget, as a click's reaches rapport.
    auto user_select = [](const native_tabs& n, int i)
    {
        if (n.list)
            gtk_list_box_select_row(GTK_LIST_BOX(n.list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(n.list), i));
        else
            gtk_notebook_set_current_page(GTK_NOTEBOOK(n.nb), i);
        spin();
    };
    const hook to_c = [](rap::tabs* t) { t->set_current(2); };
    const hook hide_d = [](rap::tabs* t) { t->set_page_visible(3, false); };

    scenario("set_current re-entered", {nullptr, to_c, nullptr, nullptr},
             [&](rap::tabs* t, const native_tabs& n, rap::label** built, std::vector<int>& changes,
                 const std::string& kind)
             {
                 t->set_current(1);
                 spin();
                 expect(changes.empty(), kind + "no on_changed, " + seq(changes));
                 expect(t->current() == 2 && native(n) == 2,
                        kind + "C current, natively too, " + str(t->current()) + "/" + str(native(n)));
                 expect(showing(n, "page c"), kind + "C shown");
                 expect(n.only_shown(2), kind + "C's page the only one shown, B's not");
                 expect(built[1] != nullptr, kind + "B built");
             });
    scenario("set_page_visible re-entered", {nullptr, nullptr, hide_d, nullptr},
             [&](rap::tabs* t, const native_tabs& n, rap::label**, std::vector<int>& changes, const std::string& kind)
             {
                 t->set_current(1);
                 spin();
                 changes.clear();
                 t->set_page_visible(1, false);
                 spin();
                 expect(changes.empty(), kind + "no on_changed, " + seq(changes));
                 expect(t->current() == 2 && native(n) == 2 && showing(n, "page c"),
                        kind + "C current and shown, " + str(t->current()) + "/" + str(native(n)));
                 expect(!t->page_visible(3) && n.headers() == "AC",
                        kind + "D hidden, its header gone: " + n.headers());
             });
    scenario("a user's switch to a lazy page", {},
             [&](rap::tabs* t, const native_tabs& n, rap::label** built, std::vector<int>& changes,
                 const std::string& kind)
             {
                 user_select(n, 1);
                 expect(changes == std::vector<int>{1}, kind + "on_changed once, with 1, " + seq(changes));
                 expect(t->current() == 1 && native(n) == 1 && built[1] && showing(n, "page b"),
                        kind + "B current, built and shown, " + str(t->current()) + "/" + str(native(n)));
             });
    scenario("a user's switch re-entered", {nullptr, to_c, nullptr, nullptr},
             [&](rap::tabs* t, const native_tabs& n, rap::label** built, std::vector<int>& changes,
                 const std::string& kind)
             {
                 user_select(n, 1);
                 expect(changes == std::vector<int>{2}, kind + "on_changed once, with 2, " + seq(changes));
                 expect(t->current() == 2 && native(n) == 2 && showing(n, "page c"),
                        kind + "C current, natively too, and shown, " + str(t->current()) + "/" + str(native(n)));
                 expect(built[1] != nullptr, kind + "B built");
             });
    // Every page hidden, then the lazy B shown: B made current and built,
    // and its builder shows C and switches to it. The page shown is the one
    // current after the build: C, natively too; no on_changed.
    const hook show_c = [](rap::tabs* t)
    {
        t->set_page_visible(2, true);
        t->set_current(2);
    };
    scenario("shown first, re-entered", {nullptr, show_c, nullptr, nullptr},
             [&](rap::tabs* t, const native_tabs& n, rap::label** built, std::vector<int>& changes,
                 const std::string& kind)
             {
                 for (int i : {3, 2, 1, 0}) t->set_page_visible(i, false);
                 spin();
                 expect(t->current() == -1 && !built[1], kind + "every page hidden: none, B unbuilt");
                 t->set_page_visible(1, true);
                 spin();
                 expect(changes.empty(), kind + "no on_changed, " + seq(changes));
                 expect(t->current() == 2 && native(n) == 2 && n.shown() == 2,
                        kind + "C current, natively too, " + str(t->current()) + "/" + str(native(n)) + "/" +
                            str(n.shown()));
                 expect(showing(n, "page c"), kind + "C shown");
                 expect(n.only_shown(2), kind + "C's page the only one shown, B's not");
                 expect(built[1] != nullptr, kind + "B built");
             });
    // A user's switch to the lazy B, whose builder hides every page: none
    // current, and no on_changed (never -1).
    const hook hide_all = [](rap::tabs* t)
    {
        for (int i : {0, 2, 3, 1}) t->set_page_visible(i, false);
    };
    scenario("a user's switch, every page hidden", {nullptr, hide_all, nullptr, nullptr},
             [&](rap::tabs* t, const native_tabs& n, rap::label**, std::vector<int>& changes, const std::string& kind)
             {
                 user_select(n, 1);
                 expect(changes.empty(), kind + "no on_changed, " + seq(changes));
                 expect(t->current() == -1 && n.shown() == -1,
                        kind + "none current, no page shown, " + str(t->current()) + "/" + str(n.shown()));
             });
    // Shown first with every page hidden, its builder hiding every page
    // again: none current, nothing shown, no on_changed.
    scenario("shown first, its builder hiding every page", {nullptr, hide_all, nullptr, nullptr},
             [&](rap::tabs* t, const native_tabs& n, rap::label**, std::vector<int>& changes, const std::string& kind)
             {
                 for (int i : {3, 2, 1, 0}) t->set_page_visible(i, false);
                 spin();
                 t->set_page_visible(1, true);  // B's builder hides every page again
                 spin();
                 expect(changes.empty(), kind + "no on_changed, " + seq(changes));
                 expect(t->current() == -1 && n.shown() == -1,
                        kind + "none current, no page shown, " + str(t->current()) + "/" + str(n.shown()));
             });
    // A user's switch to the lazy B, whose builder switches back to A, the
    // page current before: A current and shown, no on_changed.
    const hook to_a = [](rap::tabs* t) { t->set_current(0); };
    scenario("a user's switch back where it was", {nullptr, to_a, nullptr, nullptr},
             [&](rap::tabs* t, const native_tabs& n, rap::label** built, std::vector<int>& changes,
                 const std::string& kind)
             {
                 user_select(n, 1);
                 expect(changes.empty(), kind + "no on_changed, " + seq(changes));
                 expect(t->current() == 0 && native(n) == 0 && n.shown() == 0 && showing(n, "page a"),
                        kind + "A current, natively too, and shown, " + str(t->current()) + "/" + str(native(n)));
                 expect(built[1] != nullptr, kind + "B built");
             });
}

// A sidebar's natural height: its visible rows only.
void sidebar_measure(rap::ui& ui)
{
    using namespace rap::layout;
    rap::tabs* t = nullptr;
    auto win = ui.create(window{"Sidebar measure",
                                {tabs{{"General", label{"1"}},
                                      {"Appearance", label{"2"}},
                                      {"Fonts", label{"3"}},
                                      {"Advanced", label{"4"}},
                                      {"Extra", label{"5"}}}
                                     .sidebar()
                                     .bind(t)}});
    GtkWidget* top = window_titled("Sidebar measure");
    GtkWidget* list = first_of(top, GTK_TYPE_LIST_BOX);
    expect(t && list, "sidebar measure: bound, its list found");
    if (!t || !list) return;
    GtkWidget* box = gtk_widget_get_parent(list);
    const int list5 = nat_h(list), box5 = nat_h(box);
    t->set_page_visible(1, false);
    t->set_page_visible(3, false);
    const int list3 = nat_h(list), box3 = nat_h(box);
    expect(list3 < list5 && list3 * 5 == list5 * 3,
           "sidebar measure: three rows of five, " + str(list3) + " of " + str(list5));
    expect(box3 < box5, "sidebar measure: the sidebar shorter, " + str(box3) + " < " + str(box5));
    t->set_page_visible(1, true);
    t->set_page_visible(3, true);
    expect(nat_h(list) == list5, "sidebar measure: five rows again, " + str(nat_h(list)));
}

// A hidden page takes no layout: a wide page hidden, the notebook narrower.
void strip_measure(rap::ui& ui)
{
    using namespace rap::layout;
    using namespace rap::literals;
    rap::tabs* t = nullptr;
    auto win = ui.create(window{"Strip measure",
                                {tabs{{"A", label{"a"}}, {"Wide", canvas{}.width(500_px).height(50_px)}, {"C", label{"c"}}}
                                     .bind(t)}});
    GtkWidget* nb = first_of(window_titled("Strip measure"), GTK_TYPE_NOTEBOOK);
    expect(t && nb, "strip measure: bound, its notebook found");
    if (!t || !nb) return;
    const int with = nat_w(nb);
    t->set_page_visible(1, false);
    const int without = nat_w(nb);
    expect(with >= 500 && without < 500, "strip measure: the wide page hidden, no layout: " + str(with) + " -> " +
                                             str(without));
}

// #92: a canvas of a fixed size as a page's (and a window's) root content
// measures to that size, not to a canvas's own 64 x 64.
void fixed_canvas(rap::ui& ui)
{
    using namespace rap::layout;
    using namespace rap::literals;
    {
        auto win = ui.create(window{"Canvas tab", {tabs{{"Canvas", canvas{}.width(300_px).height(200_px)}}}});
        GtkWidget* nb = first_of(window_titled("Canvas tab"), GTK_TYPE_NOTEBOOK);
        expect(nb != nullptr, "#92 tab page: the notebook");
        if (nb)
        {
            GtkWidget* page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), 0);
            expect(inner(page) == "300 x 200", "#92 tab page: the canvas 300 x 200, " + inner(page));
            expect(nat_w(nb) >= nat_w(page) && nat_h(nb) >= nat_h(page),
                   "#92 tab page: the notebook holds it, " + str(nat_w(nb)) + " x " + str(nat_h(nb)));
        }
    }
    {
        auto win = ui.create(
            window{"Canvas sidebar", {tabs{{"Canvas", canvas{}.width(300_px).height(200_px)}, {"B", label{"b"}}}.sidebar()}});
        GtkWidget* stack = first_of(window_titled("Canvas sidebar"), GTK_TYPE_STACK);
        expect(stack != nullptr, "#92 sidebar page: the stack");
        if (stack)
        {
            GtkWidget* page = gtk_stack_get_child_by_name(GTK_STACK(stack), "0");
            expect(page && inner(page) == "300 x 200", "#92 sidebar page: the canvas 300 x 200, " +
                                                           (page ? inner(page) : std::string("none")));
            expect(page && nat_w(stack) >= nat_w(page) && nat_h(stack) >= nat_h(page),
                   "#92 sidebar page: the stack holds it, " + str(nat_w(stack)) + " x " + str(nat_h(stack)));
        }
    }
    {
        auto win = ui.create(window{"Canvas stack", {stack{{"c", canvas{}.width(300_px).height(200_px)}}}});
        GtkWidget* stack = first_of(window_titled("Canvas stack"), GTK_TYPE_STACK);
        expect(stack != nullptr, "#92 stack page: the stack");
        if (stack)
            expect(inner(stack) == "300 x 200", "#92 stack page: the stack 300 x 200, " + inner(stack));
    }
    {
        auto win = ui.create(window{"Canvas root", {canvas{}.width(300_px).height(200_px)}});
        GtkWidget* top = window_titled("Canvas root");
        GtkWidget* content = top ? gtk_window_get_child(GTK_WINDOW(top)) : nullptr;
        expect(content != nullptr, "#92 window root: its content");
        if (content)
            expect(inner(content) == "300 x 200", "#92 window root: 300 x 200, " + inner(content));
    }
}

void spin()
{
    while (g_main_context_iteration(nullptr, false)) {}
}

// Ctrl+Tab (Ctrl+Shift+Tab with `shift`) as document_tabs_input_test sends
// it: GTK 4 can't synthesize events, so the window's capture-phase key
// controllers are given it, the last added first (rapport's, after GTK's
// own): the first to take the key has it.
void ctrl_tab(GtkWidget* top, bool shift)
{
    const guint keyval = shift ? GDK_KEY_ISO_Left_Tab : GDK_KEY_Tab;
    const auto state = GdkModifierType(GDK_CONTROL_MASK | (shift ? GDK_SHIFT_MASK : 0));
    GListModel* controllers = gtk_widget_observe_controllers(top);
    const guint n = g_list_model_get_n_items(controllers);
    for (guint i = n; i-- > 0;)
    {
        auto* c = GTK_EVENT_CONTROLLER(g_list_model_get_item(controllers, i));
        gboolean handled = false;
        if (GTK_IS_EVENT_CONTROLLER_KEY(c) && gtk_event_controller_get_propagation_phase(c) == GTK_PHASE_CAPTURE)
        {
            g_signal_emit_by_name(c, "key-pressed", keyval, 0u, state, &handled);
            g_signal_emit_by_name(c, "key-released", keyval, 0u, state);
        }
        g_object_unref(c);
        if (handled) break;
    }
    g_object_unref(controllers);
    spin();
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
        std::fflush(stdout);
        std::signal(SIGALRM, timed_out);
        alarm(20);
    }
    ~bounded() { alarm(0); }
};

// Ctrl+Tab with the focus in a page's tabs (layout::tabs) goes round their
// visible pages: none with no pages, a hidden one skipped, nothing with
// every one hidden. The window has document tabs (their Ctrl+Tab is what
// the page's tabs take it from).
void ctrl_tab_pages(rap::ui& ui)
{
    using namespace rap::layout;
    int selected = 0;  // the document tabs' on_select: Ctrl+Tab gone to them
    auto docs = [&]
    {
        return document_tabs{{{"a", "Alpha", {}, {}}, {"b", "Beta", {}, {}}}}.on_select([&](const std::string&)
                                                                                         { ++selected; });
    };
    // The page's tabs' notebook (the document tabs' is the first) focused:
    // the toplevel, or null.
    auto ready = [](const char* title, GtkWidget*& nb) -> GtkWidget*
    {
        GtkWidget* top = window_titled(title);
        GtkWidget* docs_nb = first_of(top, GTK_TYPE_NOTEBOOK);
        nb = nullptr;
        if (GtkWidget* content = top && docs_nb ? gtk_widget_get_last_child(gtk_window_get_child(GTK_WINDOW(top)))
                                                : nullptr)
            nb = first_of(content, GTK_TYPE_NOTEBOOK);
        if (!nb || nb == docs_nb) return nullptr;
        gtk_widget_grab_focus(nb);
        spin();
        return gtk_window_get_focus(GTK_WINDOW(top)) == nb ? top : nullptr;
    };
    auto current = [](GtkWidget* nb) { return gtk_notebook_get_current_page(GTK_NOTEBOOK(nb)); };

    // 1. No pages at all: nothing, and no crash.
    {
        rap::tabs* t = nullptr;
        int changes = 0;
        auto win = ui.create(window{"Ctrl+Tab none", {tabs{}.on_changed([&](int) { ++changes; }).bind(t)}}.tabs(docs()));
        GtkWidget* nb = nullptr;
        GtkWidget* top = ready("Ctrl+Tab none", nb);
        expect(t && top, "Ctrl+Tab, no pages: the page's notebook focused");
        if (t && top)
        {
            selected = 0;
            {
                bounded guard;
                ctrl_tab(top, false);
                ctrl_tab(top, true);
            }
            expect(gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)) == 0 && current(nb) == -1 && t->current() == -1,
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
        GtkWidget* top = ready("Ctrl+Tab skip", nb);
        expect(t && top, "Ctrl+Tab, B hidden: the page's notebook focused");
        if (t && top)
        {
            expect(t->current() == 0 && notebook_headers(nb) == "ACD",
                   "Ctrl+Tab, B hidden: tabs " + notebook_headers(nb) + ", A current, " + str(t->current()));
            selected = 0;
            {
                bounded guard;
                ctrl_tab(top, false);
            }
            expect(t->current() == 2 && current(nb) == 2, "Ctrl+Tab from A: C, B skipped, " + str(t->current()) +
                                                              " (native " + str(current(nb)) + ")");
            t->set_current(0);
            spin();
            gtk_widget_grab_focus(nb);
            changes.clear();
            {
                bounded guard;
                ctrl_tab(top, true);
            }
            expect(t->current() == 3 && current(nb) == 3,
                   "Ctrl+Shift+Tab from A: D, " + str(t->current()) + " (native " + str(current(nb)) + ")");
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
        GtkWidget* top = ready("Ctrl+Tab all hidden", nb);
        expect(t && top, "Ctrl+Tab, every page hidden: the page's notebook focused");
        if (t && top)
        {
            const int native_before = current(nb);
            expect(t->current() == -1 && notebook_pages_shown(nb).empty(),
                   "every page hidden: current -1, " + str(t->current()) + ", no page shown (native current " +
                       str(native_before) + ")");
            selected = 0;
            {
                bounded guard;
                ctrl_tab(top, false);
                ctrl_tab(top, true);
            }
            expect(t->current() == -1 && current(nb) == native_before && notebook_pages_shown(nb).empty(),
                   "Ctrl+Tab and Ctrl+Shift+Tab, every page hidden: returned, nothing changed, current " +
                       str(t->current()) + " (native " + str(current(nb)) + ")");
            expect(changes == 0 && selected == 0, "every page hidden: no on_changed, " + str(changes) +
                                                      ", and not the document tabs', " + str(selected));
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options options;
    options.backend = "gtk4";
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui)
    {
        std::printf("skipped: no GTK 4 backend or display\n");
        return 77;
    }
    for (const bool sidebar : {false, true}) visibility(**ui, sidebar);
    for (const bool sidebar : {false, true}) hidden_from_start(**ui, sidebar);
    for (const bool sidebar : {false, true}) set_current_quiet(**ui, sidebar);
    for (const bool sidebar : {false, true}) reentrant(**ui, sidebar);
    sidebar_measure(**ui);
    strip_measure(**ui);
    fixed_canvas(**ui);
    ctrl_tab_pages(**ui);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
