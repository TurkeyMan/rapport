// GTK 3: a setter given the value a widget has does nothing (#88): no
// redraw, no resize, no change notification. Each setter's set once to a
// value (not counted), then to the same one again (counted: none of the
// three may come), then to another (a visible one must still draw or
// resize; notices then are #77's, and only noted). The window's drawing ("draw" on the toplevel, which GTK 3 draws
// its children through) and resizing (its "check-resize") are counted,
// off the desktop on Broadway.
//   rapport-setters-noop-test-gtk3
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#include <dlfcn.h>
#include <link.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
}

int g_draws = 0, g_resizes = 0, g_notes = 0, g_link_clicks = 0;
int g_fraction_sets = 0;  // gtk_progress_bar_set_fraction calls (each notifies "fraction")

// A test seam of the backend's (`name`): linked in (a static build), or in
// its module, loaded already (`module`, "rapport_gtk3" say) by ui::load.
void* backend_seam(const char* module, const char* name)
{
    if (void* f = dlsym(RTLD_DEFAULT, name)) return f;
    struct found
    {
        const char* module;
        std::string path;
    } f{module, {}};
    dl_iterate_phdr(
        +[](dl_phdr_info* info, size_t, void* p) -> int
        {
            auto* f = static_cast<found*>(p);
            if (!info->dlpi_name || !std::strstr(info->dlpi_name, f->module)) return 0;
            f->path = info->dlpi_name;
            return 1;
        },
        &f);
    if (f.path.empty()) return nullptr;
    void* h = dlopen(f.path.c_str(), RTLD_NOW | RTLD_NOLOAD);  // (loaded already: only its handle)
    return h ? dlsym(h, name) : nullptr;
}

// The main loop run for `ms`: everything queued drawn and laid out.
void settle(int ms = 200)
{
    const gint64 until = g_get_monotonic_time() + gint64(ms) * 1000;
    while (g_get_monotonic_time() < until)
    {
        if (gtk_events_pending())
            gtk_main_iteration_do(false);
        else
            g_usleep(2000);
    }
}

class rows : public rap::list_model
{
public:
    using rap::list_model::notify_reset;  // (the model reset, as an app's would)
    size_t row_count() const override { return 3; }
    std::string text(size_t row, size_t col) const override
    {
        return (col == 0 ? "Name " : "Note ") + std::to_string(row);
    }
};
rows g_rows;

// `w`'s descendants of type `t`, in order.
void all_of(GtkWidget* w, GType t, std::vector<GtkWidget*>& out)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE(w, t)) out.push_back(w);
    if (GTK_IS_CONTAINER(w))
    {
        GList* kids = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList* l = kids; l; l = l->next) all_of(GTK_WIDGET(l->data), t, out);
        g_list_free(kids);
    }
}
GtkWidget* first_of(GtkWidget* w, GType t)
{
    std::vector<GtkWidget*> v;
    all_of(w, t, v);
    return v.empty() ? nullptr : v[0];
}
GtkWidget* window_titled(const char* title)
{
    GtkWidget* top = nullptr;
    GList* tops = gtk_window_list_toplevels();
    for (GList* l = tops; l; l = l->next)
        if (const char* t = gtk_window_get_title(GTK_WINDOW(l->data)); t && std::string(t) == title)
            top = GTK_WIDGET(l->data);
    g_list_free(tops);
    return top;
}

rap::tree_item item(std::string id)
{
    rap::tree_item t;
    t.label = id;
    t.id = std::move(id);
    return t;
}

// `set(a)` once, uncounted; `set(a)` again: no draw, resize or notice;
// `set(b)`: no notice, and (`visible`) a draw or a resize.
template <class T>
void probe(const char* what, const std::function<void(const T&)>& set, const T& a, const T& b, bool visible = true)
{
    set(a);
    // Long enough for the theme's transitions (a toggle's, a frame's) to end.
    settle(1200);
    g_draws = g_resizes = g_notes = 0;
    set(a);
    settle();
    expect(g_draws == 0 && g_resizes == 0 && g_notes == 0,
           std::string(what) + ", unchanged: nothing (" + std::to_string(g_draws) + " draws, " +
               std::to_string(g_resizes) + " resizes, " + std::to_string(g_notes) + " notices)");
    g_draws = g_resizes = g_notes = 0;
    set(b);
    settle();
    // (Whether a changed value's setter tells the app is #77's; noted only.)
    if (g_notes) std::printf("note %s, changed: %d notices\n", what, g_notes);
    if (visible)
        expect(g_draws > 0 || g_resizes > 0, std::string(what) + ", changed: drawn or laid out again (" +
                                                  std::to_string(g_draws) + " draws, " +
                                                  std::to_string(g_resizes) + " resizes)");
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
    rap::label* label_h = nullptr;
    rap::button* button_h = nullptr;
    rap::check* check_box_h = nullptr;
    rap::entry* entry_h = nullptr;
    rap::spin* spin_h = nullptr;
    rap::slider* slider_h = nullptr;
    rap::progress_bar* progress_h = nullptr;
    rap::drop_down* drop_h = nullptr;
    rap::combo* combo_h = nullptr;
    rap::text_view* text_h = nullptr;
    rap::list* list_h = nullptr;
    rap::tree* tree_h = nullptr;
    rap::menu_button* menu_h = nullptr;
    rap::tabs* tabs_h = nullptr;
    rap::date_picker* date_h = nullptr;
    rap::color_button* colour_h = nullptr;
    using namespace rap::layout;
    using namespace rap::literals;
    auto note = [] { ++g_notes; };
    auto win = (*ui)->create(window{
        "setters",
        {vbox{hbox{label{"Label"}.bind(label_h), button{"Button"}.bind(button_h).on_click(note),
                   check{"Check"}.bind(check_box_h).on_toggled([&](bool) { note(); }),
                   menu_button{"Menu"}.bind(menu_h)},
              hbox{entry{}.bind(entry_h).on_changed([&](const std::string&) { note(); }).stretch(),
                   spin{0, 100, 5}.bind(spin_h).on_changed([&](double) { note(); }),
                   slider{0, 100, 5}.bind(slider_h).on_changed([&](double) { note(); }).stretch()},
              hbox{progress_bar{}.bind(progress_h).stretch(),
                   drop_down{{"One", "Two", "Three"}, 0}.bind(drop_h).on_changed([&](int) { note(); }),
                   combo{{"one", "two"}, "one"}.bind(combo_h).on_changed([&](const std::string&) { note(); }),
                   date_picker{{2026, 11, 16}}.bind(date_h).on_changed([&](rap::date) { note(); }),
                   color_button{{10, 20, 30, 255}}.bind(colour_h).on_changed([&](rap::color) { note(); })},
              hbox{list{{{"Name", 100_px}, {"Notes", 100_px}}, &g_rows}
                       .bind(list_h)
                       .on_selection_changed(note)
                       .stretch(),
                   tree{{item("a"), item("b")}}.bind(tree_h).on_selection_changed(note).stretch(),
                   text_view{}.bind(text_h).on_changed(note).stretch()}
                  .stretch(),
              tabs{{"One", label{"one"}}, {"Two", label{"two"}}}.bind(tabs_h).on_changed([&](int) { note(); })}}}
                                 .size(900_px, 600_px));
    win->show();
    settle(500);

    GtkWidget* top = nullptr;
    GList* tops = gtk_window_list_toplevels();
    for (GList* l = tops; l; l = l->next)
        if (const char* t = gtk_window_get_title(GTK_WINDOW(l->data)); t && std::string(t) == "setters")
            top = GTK_WIDGET(l->data);
    g_list_free(tops);
    expect(top && label_h && button_h && check_box_h && entry_h && spin_h && slider_h && progress_h && drop_h && combo_h && text_h &&
               list_h && tree_h && menu_h && tabs_h && date_h && colour_h,
           "the window and its widgets");
    if (!top) return 1;
    g_signal_connect(top, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t*, gpointer) -> gboolean {
                         ++g_draws;
                         return false;
                     }),
                     nullptr);
    g_signal_connect(top, "check-resize", G_CALLBACK(+[](GtkContainer*, gpointer) { ++g_resizes; }), nullptr);
    settle();

    using S = std::string;
    probe<S>("label::set_text", [&](const S& v) { label_h->set_text(v); }, "Alpha", "Beta");
    probe<S>("button::set_text", [&](const S& v) { button_h->set_text(v); }, "Go", "Stop");
    probe<rap::icon>("button::set_icon", [&](const rap::icon& v) { button_h->set_icon(v); },
                     rap::icon::named("go-up"), rap::icon::named("go-down"));
    probe<bool>("check::set_checked", [&](const bool& v) { check_box_h->set_checked(v); }, true, false);
    probe<rap::check_state>("check::set_state", [&](const rap::check_state& v) { check_box_h->set_state(v); },
                            rap::check_state::indeterminate, rap::check_state::checked);
    probe<S>("menu_button::set_text", [&](const S& v) { menu_h->set_text(v); }, "Menu", "More");
    probe<bool>("menu_button::set_pushed", [&](const bool& v) { menu_h->set_pushed(v); }, true, false);
    probe<S>("entry::set_text", [&](const S& v) { entry_h->set_text(v); }, "abc", "abcd");
    probe<S>("entry::set_placeholder", [&](const S& v) { entry_h->set_placeholder(v); }, "Find", "Search");
    probe<bool>("entry::set_read_only", [&](const bool& v) { entry_h->set_read_only(v); }, true, false, false);
    probe<bool>("entry::set_frame", [&](const bool& v) { entry_h->set_frame(v); }, false, true);
    probe<double>("spin::set_value", [&](const double& v) { spin_h->set_value(v); }, 10, 20);
    probe<double>("slider::set_value", [&](const double& v) { slider_h->set_value(v); }, 10, 60);
    probe<std::pair<double, double>>("slider::set_range",
                                     [&](const std::pair<double, double>& v) { slider_h->set_range(v.first, v.second); },
                                     {0, 100}, {0, 200});
    probe<float>("progress_bar::set_fraction", [&](const float& v) { progress_h->set_fraction(v); }, 0.25f, 0.75f);
    probe<int>("drop_down::set_current", [&](const int& v) { drop_h->set_current(v); }, 1, 2);
    probe<std::vector<S>>("drop_down::set_items", [&](const std::vector<S>& v) { drop_h->set_items(v); },
                          {"One", "Two", "Three"}, {"Uno", "Dos", "Tres"}, false);
    probe<S>("combo::set_text", [&](const S& v) { combo_h->set_text(v); }, "two", "three");
    probe<rap::date>("date_picker::set_date", [&](const rap::date& v) { date_h->set_date(v); }, {2026, 1, 2},
                     {2026, 3, 4});
    probe<rap::color>("color_button::set_color", [&](const rap::color& v) { colour_h->set_color(v); },
                      {200, 10, 10, 255}, {10, 200, 10, 255});
    probe<S>("text_view::set_text", [&](const S& v) { text_h->set_text(v); }, "Some text", "Other text");
    probe<bool>("text_view::set_read_only", [&](const bool& v) { text_h->set_read_only(v); }, true, false, false);
    probe<size_t>("list::set_current", [&](const size_t& v) { list_h->set_current(v); }, 1, 2);
    probe<std::vector<size_t>>("list::select", [&](const std::vector<size_t>& v) { list_h->select(v); }, {0, 2},
                               {1});
    probe<bool>("list::set_column_visible", [&](const bool& v) { list_h->set_column_visible(1, v); }, false, true);
    probe<bool>("tree::set_expanded", [&](const bool& v) { tree_h->set_expanded("a", v); }, false, false, false);
    probe<int>("tabs::set_current", [&](const int& v) { tabs_h->set_current(v); }, 1, 0);
    probe<S>("tree::select", [&](const S& v) { tree_h->select(v); }, "a", "b");
    probe<bool>("widget::set_enabled", [&](const bool& v) { button_h->set_enabled(v); }, false, true);
    probe<bool>("widget::set_visible", [&](const bool& v) { check_box_h->set_visible(v); }, false, true);
    probe<S>("widget::set_tooltip", [&](const S& v) { label_h->set_tooltip(v); }, "Tip", "Other tip", false);
    probe<S>("window::set_title", [&](const S& v) { win->set_title(v); }, "setters", "setters again", false);

    // -- what's on screen, not a cache (#91's review) --------------------
    rap::progress_bar* bar = nullptr;
    rap::spin* sp = nullptr;
    rap::slider* sl = nullptr;
    rap::date_picker* plain_date = nullptr;
    rap::date_picker* epoch_date = nullptr;
    rap::list* lst = nullptr;
    rap::splitter* parent = nullptr;
    rap::button* child = nullptr;
    rap::splitter* split = nullptr;
    rap::entry* ent = nullptr;
    rap::text_view* txt = nullptr;
    rap::combo* cmb = nullptr;
    rap::check* chk = nullptr;
    rap::tabs* tbs = nullptr;
    auto win2 = (*ui)->create(window{
        "regressions",
        {vbox{hbox{progress_bar{}.bind(bar).stretch(), spin{0, 100, 1}.bind(sp), slider{0, 100, 1}.bind(sl).stretch(),
                   date_picker{}.bind(plain_date), date_picker{{1970, 1, 1}}.bind(epoch_date)},
              hbox{entry{}.bind(ent).stretch(), combo{{"one", "two"}, "one"}.bind(cmb), check{"Check"}.bind(chk)},
              splitter(orientation::horizontal, button{"Child"}.bind(child), label{"other"}).bind(parent),
              splitter(orientation::horizontal, list{{{"Name", 100_px}, {"Notes", 100_px}}, &g_rows}.bind(lst),
                       text_view{}.bind(txt))
                  .bind(split)
                  .stretch(),
              tabs{{"One", label{"one"}}, {"Two", label{"two"}}}.bind(tbs),
              label{"A link"}.link([] { ++g_link_clicks; })}}}
                                  .size(800_px, 500_px));
    win2->show();
    settle(800);
    GtkWidget* top2 = window_titled("regressions");
    expect(top2 && bar && sp && sl && plain_date && epoch_date && lst && parent && child && split && ent && txt &&
               cmb && chk && tbs,
           "the regressions' window and widgets");
    if (top2 && bar && sp && sl && plain_date && epoch_date && lst && parent && child && split && ent && txt && cmb &&
        chk && tbs)
    {
        // A new link label: its link's markup in place, and it activates.
        {
            GtkWidget* link = nullptr;
            std::vector<GtkWidget*> labels;
            all_of(top2, GTK_TYPE_LABEL, labels);
            for (GtkWidget* l : labels)
                if (std::string(gtk_label_get_text(GTK_LABEL(l))) == "A link") link = l;
            const std::string markup = link ? gtk_label_get_label(GTK_LABEL(link)) : "";
            gboolean handled = false;
            if (link) g_signal_emit_by_name(link, "activate-link", "rapport:", &handled);
            expect(markup.find("<a ") != std::string::npos && g_link_clicks == 1,
                   "a new link label: its link's markup (" + markup + ") and activated (" +
                       std::to_string(g_link_clicks) + " clicks)");
        }

        // Progress: pulse, then the fraction it had: out of activity mode, at
        // that fraction. GTK 3 doesn't say whether it's pulsing; only
        // gtk_progress_bar_set_fraction ends activity mode, so its calls are
        // counted (it notifies "fraction" every time, the same value or not).
        GtkWidget* pb = first_of(top2, GTK_TYPE_PROGRESS_BAR);
        if (pb)
            g_signal_connect(pb, "notify::fraction", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer) { ++g_fraction_sets; }),
                             nullptr);
        for (const float f : {0.f, 0.5f})
        {
            bar->set_fraction(f);
            settle();
            g_fraction_sets = 0;
            bar->set_fraction(f);  // not pulsing: as it is
            const int idle = g_fraction_sets;
            bar->pulse();
            g_fraction_sets = 0;
            bar->set_fraction(f);  // pulsing: the fraction set, activity mode left
            const int after_pulse = g_fraction_sets;
            settle();
            expect(pb && idle == 0 && after_pulse == 1 && gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(pb)) == f,
                   "progress: the same fraction (" + std::to_string(f) + ") sets nothing (" + std::to_string(idle) +
                       " set_fraction calls); after pulse() it's set, leaving activity mode (" +
                       std::to_string(after_pulse) + " calls)");
        }

        // Spin: set 4, the user types 7 (not committed), set 4: the edit shows
        // 4 and keeps it on leaving.
        GtkWidget* sb = first_of(top2, GTK_TYPE_SPIN_BUTTON);
        sp->set_value(4);
        settle();
        gtk_entry_set_text(GTK_ENTRY(sb), "7");  // as typed: not yet the adjustment's
        sp->set_value(4);
        settle();
        const std::string shown = gtk_entry_get_text(GTK_ENTRY(sb));
        gtk_spin_button_update(GTK_SPIN_BUTTON(sb));  // leaving the field commits what it shows
        expect(shown == "4" && sp->value() == 4,
               "spin: 4, typed 7, set 4: the edit shows 4 (" + shown + ") and keeps it (" +
                   std::to_string(sp->value()) + ")");
        // A new range: the value re-applied, the adjustment and value() agree.
        sp->set_value(40);
        sp->set_range(20, 120);
        settle();
        expect(gtk_spin_button_get_value(GTK_SPIN_BUTTON(sb)) == 40 && sp->value() == 40,
               "spin: 40, then range 20..120: still 40, natively and value()");
        sp->set_range(50, 120);
        settle();
        expect(gtk_spin_button_get_value(GTK_SPIN_BUTTON(sb)) == 50 && sp->value() == 50 &&
                   std::string(gtk_entry_get_text(GTK_ENTRY(sb))) == "50",
               "spin: range 50..120 clamps 40 to 50, natively, value() and shown");

        // Slider: range 0..100 at 40, then 20..120: still 40 everywhere.
        GtkWidget* sc = first_of(top2, GTK_TYPE_SCALE);
        sl->set_value(40);
        sl->set_range(20, 120);
        settle();
        expect(sc && gtk_range_get_value(GTK_RANGE(sc)) == 40 && sl->value() == 40,
               "slider: 40, then range 20..120: still 40, natively and value()");

        // Dates: a default picker and one set to 1970-01-01 both show it.
        std::vector<GtkWidget*> buttons;
        all_of(top2, GTK_TYPE_BUTTON, buttons);
        int dated = 0;
        for (GtkWidget* b : buttons)
            if (const char* l = gtk_button_get_label(GTK_BUTTON(b)); l && std::string(l).find("70") != std::string::npos)
                ++dated;
        expect(dated >= 2, "date pickers: the default and 1970-01-01 both show their date (" + std::to_string(dated) +
                               " labelled)");

        // List set_current: set 1, select nothing, set 1: row 1 selected
        // again.
        lst->set_current(1);
        settle();
        lst->select({});
        settle();
        lst->set_current(1);
        settle();
        expect(lst->selection() == std::vector<size_t>{1}, "list: set_current(1), select({}), set_current(1): row 1 "
                                                           "selected again");

        // The sort indicator changed underneath (as a header click would),
        // then the app's restored: shown again.
        GtkWidget* tv = first_of(top2, GTK_TYPE_TREE_VIEW);
        lst->set_sort_indicator(1, rap::sort_order::descending);
        settle();
        GtkTreeViewColumn* c0 = gtk_tree_view_get_column(GTK_TREE_VIEW(tv), 0);
        GtkTreeViewColumn* c1 = gtk_tree_view_get_column(GTK_TREE_VIEW(tv), 1);
        gtk_tree_view_column_set_sort_indicator(c1, false);
        gtk_tree_view_column_set_sort_indicator(c0, true);
        gtk_tree_view_column_set_sort_order(c0, GTK_SORT_ASCENDING);
        lst->set_sort_indicator(1, rap::sort_order::descending);
        settle();
        expect(gtk_tree_view_column_get_sort_indicator(c1) && !gtk_tree_view_column_get_sort_indicator(c0) &&
                   gtk_tree_view_column_get_sort_order(c1) == GTK_SORT_DESCENDING,
               "list: the sort indicator changed underneath, then the app's again: shown");
        // The column order changed as a user's drag would, then restored.
        lst->set_column_order({0, 1});
        gtk_tree_view_move_column_after(GTK_TREE_VIEW(tv), c0, c1);
        lst->set_column_order({0, 1});
        settle();
        expect(lst->column_order() == std::vector<size_t>{0, 1} &&
                   gtk_tree_view_get_column(GTK_TREE_VIEW(tv), 0) == c0,
               "list: the columns dragged, then the app's order again: restored");
        // The app's indicator asked for while the columns already show it
        // (changed underneath to it), then the model reset (which clears the
        // columns' indicators and puts the app's back): the one asked for
        // last, not the one before it.
        lst->set_sort_indicator(1, rap::sort_order::descending);
        settle();
        gtk_tree_view_column_set_sort_indicator(c1, false);
        gtk_tree_view_column_set_sort_indicator(c0, true);
        gtk_tree_view_column_set_sort_order(c0, GTK_SORT_ASCENDING);
        lst->set_sort_indicator(0, rap::sort_order::ascending);  // shown already: nothing native
        g_rows.notify_reset();
        settle();
        expect(gtk_tree_view_column_get_sort_indicator(c0) && !gtk_tree_view_column_get_sort_indicator(c1) &&
                   gtk_tree_view_column_get_sort_order(c0) == GTK_SORT_ASCENDING,
               "list: an indicator asked for as shown already, then the model reset: that one shown, not the "
               "older one");

        // Splitter: 300, a window resize moves the divider, 300 again: 300.
        GtkWidget* pane = nullptr;
        {
            std::vector<GtkWidget*> paneds;
            all_of(top2, GTK_TYPE_PANED, paneds);
            for (GtkWidget* w : paneds)
                if (first_of(w, GTK_TYPE_TREE_VIEW)) pane = w;
        }
        split->set_sizes({300, 300});
        settle(500);
        if (pane) gtk_paned_set_position(GTK_PANED(pane), 200);  // moved, as a drag or a resize would
        settle(300);
        split->set_sizes({300, 300});
        settle(500);
        expect(pane && gtk_paned_get_position(GTK_PANED(pane)) == 300 && split->sizes().size() >= 1 &&
                   std::lround(split->sizes()[0]) == 300,
               "splitter: 300, moved, 300 again: placed at 300 (" +
                   std::to_string(pane ? gtk_paned_get_position(GTK_PANED(pane)) : -1) + ")");

        // Editable values changed as the user would, then the app's again.
        GtkWidget* en = nullptr;
        {
            std::vector<GtkWidget*> entries;
            all_of(top2, GTK_TYPE_ENTRY, entries);
            for (GtkWidget* e : entries)
                if (!GTK_IS_SPIN_BUTTON(e) && !GTK_IS_COMBO_BOX(gtk_widget_get_parent(e))) en = e;
        }
        ent->set_text("A");
        if (en) gtk_entry_set_text(GTK_ENTRY(en), "B");
        ent->set_text("A");
        expect(ent->text() == "A", "entry: A, the user's B, A again: A");
        txt->set_text("A");
        {
            GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(first_of(top2, GTK_TYPE_TEXT_VIEW)));
            gtk_text_buffer_set_text(buf, "B", -1);
        }
        txt->set_text("A");
        expect(txt->text() == "A", "text view: A, the user's B, A again: A");
        cmb->set_text("one");
        {
            GtkWidget* ce = gtk_bin_get_child(GTK_BIN(first_of(top2, GTK_TYPE_COMBO_BOX)));
            gtk_entry_set_text(GTK_ENTRY(ce), "two");
        }
        cmb->set_text("one");
        expect(cmb->text() == "one", "combo: one, the user's two, one again: one");
        chk->set_checked(true);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(first_of(top2, GTK_TYPE_CHECK_BUTTON)), false);
        chk->set_checked(true);
        expect(chk->checked(), "check: on, the user's off, on again: on");
        tbs->set_current(1);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(first_of(top2, GTK_TYPE_NOTEBOOK)), 0);
        tbs->set_current(1);
        expect(tbs->current() == 1, "tabs: 1, the user's 0, 1 again: 1");

        // Enabled: a control's own, the effective its and its ancestors'.
        GtkWidget* cb = nullptr;
        for (GtkWidget* b : buttons)
            if (const char* l = gtk_button_get_label(GTK_BUTTON(b)); l && std::string(l) == "Child") cb = b;
        parent->set_enabled(false);
        child->set_enabled(false);
        parent->set_enabled(true);
        settle();
        expect(cb && !gtk_widget_get_sensitive(cb) && !gtk_widget_is_sensitive(cb),
               "enabled: the parent off, the child off, the parent on: the child still off");
        child->set_enabled(true);
        parent->set_enabled(false);
        child->set_enabled(true);  // its own already on: nothing
        parent->set_enabled(true);
        settle();
        expect(cb && gtk_widget_get_sensitive(cb) && gtk_widget_is_sensitive(cb),
               "enabled: the parent off, the child's own on again (nothing), the parent on: the child on");

        // Fullscreen asked on then off before any answer: off asked too.
        win2->set_fullscreen(true);
        win2->set_fullscreen(false);
        settle(500);
        const bool full = gdk_window_get_state(gtk_widget_get_window(top2)) & GDK_WINDOW_STATE_FULLSCREEN;
        expect(!full, "fullscreen: on then off at once: not fullscreen");

        // The requests themselves, counted by the backend's seam: none
        // answered in between (no main loop run); a window-state-event (as
        // the window manager's, an outside change) sent by hand.
        using requests_fn = const char*();
        auto* requests =
            reinterpret_cast<requests_fn*>(backend_seam("rapport_gtk3", "rapport_gtk3_fullscreen_requests_for_tests"));
        expect(requests != nullptr, "fullscreen: the backend's request counter");
        if (requests)
        {
            GdkWindow* gw = gtk_widget_get_window(top2);
            auto seen = [&](bool fs)
            {
                GdkEvent* e = gdk_event_new(GDK_WINDOW_STATE);
                e->window_state.window = GDK_WINDOW(g_object_ref(gw));
                e->window_state.send_event = true;
                e->window_state.changed_mask = GDK_WINDOW_STATE_FULLSCREEN;
                e->window_state.new_window_state =
                    GdkWindowState((gdk_window_get_state(gw) & ~GDK_WINDOW_STATE_FULLSCREEN) |
                                   (fs ? GDK_WINDOW_STATE_FULLSCREEN : 0));
                gtk_main_do_event(e);
                gdk_event_free(e);
            };
            seen(false);
            requests();
            win2->set_fullscreen(true);
            win2->set_fullscreen(false);
            std::string r = requests();
            expect(r == "FU", "fullscreen: on then off, unanswered: both asked, in order (" + r + ")");
            seen(false);
            requests();
            win2->set_fullscreen(true);
            win2->set_fullscreen(true);
            r = requests();
            expect(r == "F", "fullscreen: on twice, unanswered: asked once (" + r + ")");
            seen(false);
            requests();
            win2->set_fullscreen(true);
            win2->set_fullscreen(false);
            seen(true);  // made fullscreen from outside, after the reversal
            win2->set_fullscreen(false);
            r = requests();
            expect(r == "FUU", "fullscreen: on, off, then fullscreen from outside: off asked again (" + r + ")");
            seen(false);
            settle(500);
        }
    }

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
