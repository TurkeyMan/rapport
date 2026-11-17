// A setter given the value a widget already has does nothing on GTK 4
// (#88): no property set (no "notify" from any widget in the window), no
// relayout and no redraw (the window's frame clock runs no "layout" or
// "paint" phase). Given a different value, it still does as before (a
// notify, a layout or a paint). Every setter of every widget, set to its
// value twice: the second time, nothing.
//   rapport-setters-noop-test-gtk4   (needs a display; GDK_BACKEND=broadway will do)
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#include <dlfcn.h>
#include <link.h>

#include <chrono>
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

void pump(int ms = 150)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do
        while (g_main_context_iteration(nullptr, false)) {}
    while (std::chrono::steady_clock::now() < until);
}

// What happened in the window: properties notified, layout and paint phases.
struct counts
{
    int notify = 0, layout = 0, paint = 0;
    int any() const { return notify + layout + paint; }
    std::string str() const
    {
        return std::to_string(notify) + " notifies, " + std::to_string(layout) + " layouts, " +
               std::to_string(paint) + " paints";
    }
};
counts g_counts;
std::string g_last_notify;

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

void watch_tree(GtkWidget* w)
{
    g_signal_connect(w, "notify",
                     G_CALLBACK(+[](GObject* o, GParamSpec* spec, gpointer)
                                {
                                    ++g_counts.notify;
                                    g_last_notify = std::string(G_OBJECT_TYPE_NAME(o)) + "::" + spec->name;
                                }),
                     nullptr);
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) watch_tree(c);
}

// Set to the value it has (twice: the first may differ from what was built,
// the second must do nothing), then to another: that still shows.
bool g_quiet = true;  // the window settles: no layout or paint of its own
void probe(const std::string& what, const std::function<void()>& same, const std::function<void()>& changed)
{
    same();
    pump(900);  // whatever came before, settled (Broadway's frames come slowly with no viewer)
    // No notify ever; a layout or a paint of Broadway's own now and then
    // (none from the setter: one that relayouts shows it every time), so up
    // to three tries for none.
    counts c;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        g_counts = {};
        g_last_notify.clear();
        same();
        pump(600);
        c = g_counts;
        if (c.notify || c.layout + c.paint == 0) break;
    }
    if (g_quiet)
        expect(c.any() == 0, what + ", set to the value it has: nothing (" + c.str() +
                                 (g_last_notify.empty() ? "" : ", last " + g_last_notify) + ")");
    else
        expect(c.notify == 0, what + ", set to the value it has: no notify (" + c.str() +
                                  (g_last_notify.empty() ? "" : ", last " + g_last_notify) + ")");
    if (!changed) return;
    g_counts = {};
    changed();
    pump(1200);
    expect(g_counts.any() > 0, what + ", set to another: shown (" + g_counts.str() + ")");
}

GtkWidget* find_column_view(GtkWidget* w)
{
    if (GTK_IS_COLUMN_VIEW(w)) return w;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* found = find_column_view(c)) return found;
    return nullptr;
}
// The first widget under `w` (depth first, itself included) `is` says yes to.
GtkWidget* find_first(GtkWidget* w, const std::function<bool(GtkWidget*)>& is)
{
    if (is(w)) return w;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* found = find_first(c, is)) return found;
    return nullptr;
}
// A progress bar pulsing: its progress node in activity mode ("pulse").
bool pulsing(GtkWidget* bar)
{
    for (GtkWidget* c = gtk_widget_get_first_child(bar); c; c = gtk_widget_get_next_sibling(c))
        if (gtk_widget_has_css_class(c, "pulse")) return true;
        else if (pulsing(c))
            return true;
    return false;
}

class rows_model : public rap::list_model
{
public:
    size_t row_count() const override { return 4; }
    std::string text(size_t row, size_t col) const override
    {
        return (col == 0 ? "Name " : "Note ") + std::to_string(row);
    }
};
rows_model g_model;

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options options;
    options.backend = "gtk4";
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 2;
    // No caret blinking in an entry (a paint every half second of its own).
    g_object_set(gtk_settings_get_default(), "gtk-cursor-blink", FALSE, nullptr);

    using namespace rap::layout;
    using namespace rap::literals;
    rap::label* lab = nullptr;
    rap::button* btn = nullptr;
    rap::check* chk = nullptr;
    rap::entry* ent = nullptr;
    rap::entry* box = nullptr;
    rap::drop_down* dd = nullptr;
    rap::combo* cmb = nullptr;
    rap::list* lst = nullptr;
    rap::tree* tr = nullptr;
    rap::menu_button* mb = nullptr;
    rap::progress_bar* pb = nullptr;
    rap::date_picker* dp = nullptr;
    rap::tool_button* tb = nullptr;
    rap::slider* sl = nullptr;
    rap::spin* sp = nullptr;
    rap::color_button* cb = nullptr;
    rap::tabs* tbs = nullptr;
    rap::splitter* spl = nullptr;
    rap::canvas* cv = nullptr;
    rap::date_picker* dp0 = nullptr;
    rap::entry* acted = nullptr;
    rap::entry_action* act = nullptr;
    // Sortable, so a column shows the sort indicator.
    std::vector<rap::column> cols{{"Name", 120_px}, {"Notes", 120_px}};
    for (auto& c : cols) c.sortable = true;
    auto win = (*ui)->create(window{
        "Setters",
        {vbox{hbox{label{"Label"}.bind(lab), button{"Button"}.bind(btn), check{"Check"}.bind(chk),
                   tool_button{rap::icon::named("go-up"), "Tool"}.toggle(true).bind(tb),
                   menu_button{"Menu"}.bind(mb), color_button{rap::color{10, 20, 30, 255}}.bind(cb)},
              hbox{entry{"one"}.bind(ent), entry{"two"}.clearable().bind(box),
                   drop_down{{"A", "B", "C"}, 0}.bind(dd), combo{{"x", "y"}, "x"}.bind(cmb)},
              hbox{slider{0, 100, 10}.bind(sl), spin{0, 100, 10}.bind(sp), progress_bar{}.bind(pb),
                   date_picker{rap::date{2024, 5, 6}}.bind(dp), date_picker{}.bind(dp0),
                   entry{"three"}.trailing_action(rap::icon::named("go-down"), "Act", [] {}, act).bind(acted)},
              splitter{rap::orientation::horizontal, list{cols, &g_model}.bind(lst).stretch(),
                       tree{{{"Alpha", "a", {{"Child", "c"}}}, {"Beta", "b"}}}.bind(tr).stretch(),
                       canvas{}.bind(cv).stretch()}
                  .bind(spl)
                  .stretch(),
              tabs{{"One", label{"one"}}, {"Two", label{"two"}}}.bind(tbs)}}}
                                 .size(900_px, 600_px));
    win->show();
    pump(600);
    GListModel* tops = gtk_window_get_toplevels();
    GtkWidget* top = nullptr;
    for (guint i = 0; i < g_list_model_get_n_items(tops) && !top; ++i)
    {
        auto* w = static_cast<GtkWidget*>(g_list_model_get_item(tops, i));
        const char* t = gtk_window_get_title(GTK_WINDOW(w));
        if (t && std::string(t) == "Setters") top = w;
        g_object_unref(w);
    }
    expect(top != nullptr, "the window");
    expect(lab && btn && chk && ent && box && dd && cmb && lst && tr && mb && pb && dp && tb && sl && sp && cb &&
               tbs && spl && cv,
           "its widgets bound");
    if (!top || !lab || !btn || !chk || !ent || !box || !dd || !cmb || !lst || !tr || !mb || !pb || !dp || !tb ||
        !sl || !sp || !cb || !tbs || !spl || !cv)
        return 1;
    watch_tree(top);
    if (GdkFrameClock* clock = gtk_widget_get_frame_clock(top))
    {
        g_signal_connect(clock, "layout", G_CALLBACK(+[](GdkFrameClock*, gpointer) { ++g_counts.layout; }), nullptr);
        g_signal_connect(clock, "paint", G_CALLBACK(+[](GdkFrameClock*, gpointer) { ++g_counts.paint; }), nullptr);
    }
    else
        expect(false, "the window's frame clock");
    // Settled: nothing of its own between setters.
    pump(400);
    g_counts = {};
    pump(300);
    g_quiet = g_counts.layout + g_counts.paint == 0;
    std::printf("note the window at rest: %s%s\n", g_counts.str().c_str(),
                g_quiet ? "" : " (layout and paint not counted: it isn't quiet)");

    probe("a label's text", [&] { lab->set_text("Label"); }, [&] { lab->set_text("Label 2"); });
    probe("a label's tooltip", [&] { lab->set_tooltip("tip"); }, [&] { lab->set_tooltip("tip 2"); });
    probe("a label's enabled", [&] { lab->set_enabled(true); }, [&] { lab->set_enabled(false); });
    lab->set_enabled(true);
    probe("a label's visible", [&] { lab->set_visible(true); }, [&] { lab->set_visible(false); });
    lab->set_visible(true);
    probe("a button's text", [&] { btn->set_text("Button"); }, [&] { btn->set_text("Button 2"); });
    probe("a button's icon", [&] { btn->set_icon(rap::icon::named("go-down")); },
          [&] { btn->set_icon(rap::icon::named("go-up")); });
    probe("a check's state", [&] { chk->set_checked(true); }, [&] { chk->set_checked(false); });
    probe("a check's indeterminate state", [&] { chk->set_state(rap::check_state::indeterminate); },
          [&] { chk->set_state(rap::check_state::checked); });
    probe("a tool button's checked", [&] { tb->set_checked(true); }, [&] { tb->set_checked(false); });
    probe("a tool button's icon", [&] { tb->set_icon(rap::icon::named("go-down")); },
          [&] { tb->set_icon(rap::icon::named("go-up")); });
    probe("a menu button's text", [&] { mb->set_text("Menu"); }, [&] { mb->set_text("Menu 2"); });
    probe("a menu button's icon", [&] { mb->set_icon(rap::icon::named("go-down")); },
          [&] { mb->set_icon(rap::icon::named("go-up")); });
    probe("a colour button's colour", [&] { cb->set_color({10, 20, 30, 255}); }, [&] { cb->set_color({40, 50, 60, 255}); });
    probe("an entry's text", [&] { ent->set_text("one"); }, [&] { ent->set_text("one 2"); });
    probe("an entry's read-only", [&] { ent->set_read_only(true); }, [&] { ent->set_read_only(false); });
    probe("an entry's placeholder", [&] { ent->set_placeholder("type"); }, [&] { ent->set_placeholder("type 2"); });
    probe("an entry's frame", [&] { ent->set_frame(false); }, [&] { ent->set_frame(true); });
    probe("a clearable entry's text", [&] { box->set_text("two"); }, [&] { box->set_text("two 2"); });
    probe("a clearable entry's read-only", [&] { box->set_read_only(true); }, [&] { box->set_read_only(false); });
    probe("a clearable entry's placeholder", [&] { box->set_placeholder("type"); },
          [&] { box->set_placeholder("type 2"); });
    probe("a clearable entry's frame", [&] { box->set_frame(false); }, [&] { box->set_frame(true); });
    probe("a drop-down's items", [&] { dd->set_items({"A", "B", "C"}); }, [&] { dd->set_items({"A", "B", "D"}); });
    probe("a drop-down's current", [&] { dd->set_current(1); }, [&] { dd->set_current(2); });
    probe("a combo's items", [&] { cmb->set_items({"x", "y"}); }, [&] { cmb->set_items({"x", "z"}); });
    probe("a combo's text", [&] { cmb->set_text("x"); }, [&] { cmb->set_text("y"); });
    probe("a slider's value", [&] { sl->set_value(20); }, [&] { sl->set_value(30); });
    probe("a slider's range", [&] { sl->set_range(0, 100); }, [&] { sl->set_range(0, 200); });
    probe("a slider's marks", [&] { sl->set_marks({{50, "half"}}); }, [&] { sl->set_marks({{25, "quarter"}}); });
    probe("a spin's value", [&] { sp->set_value(20); }, [&] { sp->set_value(30); });
    probe("a spin's range", [&] { sp->set_range(0, 100); }, [&] { sp->set_range(0, 200); });
    probe("a progress bar's fraction", [&] { pb->set_fraction(0.25f); }, [&] { pb->set_fraction(0.5f); });
    probe("a date picker's date", [&] { dp->set_date({2024, 5, 7}); }, [&] { dp->set_date({2024, 5, 8}); });
    probe("a list's selection", [&] { lst->select({1, 2}); }, [&] { lst->select({3}); });
    probe("a list's sort indicator", [&] { lst->set_sort_indicator(0, rap::sort_order::ascending); }, nullptr);
    // Set to another: the column view sorted so (its header's arrow).
    {
        lst->set_sort_indicator(0, rap::sort_order::descending);
        pump(200);
        GtkWidget* cv_w = find_column_view(top);
        GtkSorter* sorter = cv_w ? gtk_column_view_get_sorter(GTK_COLUMN_VIEW(cv_w)) : nullptr;
        expect(sorter && GTK_IS_COLUMN_VIEW_SORTER(sorter) &&
                   gtk_column_view_sorter_get_primary_sort_order(GTK_COLUMN_VIEW_SORTER(sorter)) ==
                       GTK_SORT_DESCENDING,
               "a list's sort indicator, set to another: the column view sorted descending");
    }
    probe("a list's column widths", [&] { lst->set_column_widths({150, 130}); },
          [&] { lst->set_column_widths({160, 130}); });
    probe("a list's column order", [&] { lst->set_column_order({1, 0}); }, [&] { lst->set_column_order({0, 1}); });
    probe("a list's column visible", [&] { lst->set_column_visible(1, false); },
          [&] { lst->set_column_visible(1, true); });
    probe("a tree's expanded", [&] { tr->set_expanded("a", true); }, [&] { tr->set_expanded("a", false); });
    probe("a splitter's sizes", [&] { spl->set_sizes(spl->sizes()); }, nullptr);
    probe("tabs' current", [&] { tbs->set_current(1); }, [&] { tbs->set_current(0); });
    probe("a canvas's cursor", [&] { cv->set_cursor(rap::cursor::pointer); }, nullptr);
    probe("a window's title", [&] { win->set_title("Setters"); }, [&] { win->set_title("Setters 2"); });
    win->set_title("Setters");
    probe("a window's minimum size", [&] { win->set_min_size({300, 200}); }, [&] { win->set_min_size({320, 200}); });
    probe("a window's menu bar visible", [&] { win->set_menu_bar_visible(true); }, nullptr);

    // "Unchanged" is what's on screen, not a value kept: each of these,
    // changed under the setter (as the user, or another call, would), and
    // the setter given its old value again must apply it.
    // A progress bar pulsing, given the fraction it had: shows it, stops.
    if (GtkWidget* bar = find_first(top, [](GtkWidget* w) { return GTK_IS_PROGRESS_BAR(w); }))
    {
        pb->set_fraction(0.5f);
        pump(200);
        pb->pulse();
        pump(200);
        const bool was = pulsing(bar);
        pb->set_fraction(0.5f);
        pump(300);
        expect(was && !pulsing(bar) && gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(bar)) == 0.5,
               "a pulsing progress bar given the fraction it had: shown, not pulsing (pulsing before: " +
                   std::string(was ? "yes" : "no") + ")");
    }
    else
        expect(false, "the progress bar");
    // A spin with text typed and not taken, given its value: shows the value.
    if (GtkWidget* spin_w = find_first(top, [](GtkWidget* w) { return GTK_IS_SPIN_BUTTON(w); }))
    {
        sp->set_value(4);
        pump(200);
        gtk_editable_set_text(GTK_EDITABLE(spin_w), "7");  // typed, not activated
        pump(100);
        sp->set_value(4);
        pump(200);
        const std::string shown = gtk_editable_get_text(GTK_EDITABLE(spin_w));
        expect(shown == "4" && sp->value() == 4,
               "a spin with 7 typed, given its value 4: shows 4 (" + shown + ", " + std::to_string(sp->value()) + ")");
    }
    else
        expect(false, "the spin button");
    // A new range: the value applied again in it, the native one and value()
    // agreeing (40 in 20..120).
    sp->set_range(0, 100);
    sp->set_value(40);
    sp->set_range(20, 120);
    pump(200);
    if (GtkWidget* spin_w = find_first(top, [](GtkWidget* w) { return GTK_IS_SPIN_BUTTON(w); }))
        expect(sp->value() == 40 && gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_w)) == 40,
               "a spin's new range 20..120: its value 40 still, shown");
    sl->set_range(0, 100);
    sl->set_value(40);
    sl->set_range(20, 120);
    pump(200);
    if (GtkWidget* scale = find_first(top, [](GtkWidget* w) { return GTK_IS_SCALE(w); }))
        expect(sl->value() == 40 && gtk_range_get_value(GTK_RANGE(scale)) == 40,
               "a slider's new range 20..120: its value 40 still, shown (" + std::to_string(sl->value()) + ", " +
                   std::to_string(gtk_range_get_value(GTK_RANGE(scale))) + ")");
    // A splitter dragged (its divider moved), given the sizes it had: placed
    // so again.
    if (GtkWidget* paned = find_first(top, [](GtkWidget* w) { return GTK_IS_PANED(w); }))
    {
        spl->set_sizes({250, 300, 100});
        pump(600);
        const int placed = gtk_paned_get_position(GTK_PANED(paned));
        gtk_paned_set_position(GTK_PANED(paned), 200);  // the user's drag
        pump(400);
        spl->set_sizes({250, 300, 100});
        pump(600);
        expect(placed == 250 && gtk_paned_get_position(GTK_PANED(paned)) == 250 && spl->sizes()[0] == 250,
               "a splitter dragged to 200, given 250 again: at 250 (" +
                   std::to_string(gtk_paned_get_position(GTK_PANED(paned))) + ")");
    }
    else
        expect(false, "the splitter's paned");
    // The list's header changed by the user (sort, width, order), the setter
    // given the old state: it's back.
    if (GtkWidget* cv_w = find_column_view(top))
    {
        lst->set_column_order({0, 1});
        pump(200);
        GListModel* cols_now = gtk_column_view_get_columns(GTK_COLUMN_VIEW(cv_w));
        auto* c0 = static_cast<GtkColumnViewColumn*>(g_list_model_get_item(cols_now, 0));
        auto* c1 = static_cast<GtkColumnViewColumn*>(g_list_model_get_item(cols_now, 1));
        lst->set_sort_indicator(1, rap::sort_order::descending);
        pump(200);
        gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(cv_w), c0, GTK_SORT_ASCENDING);  // the user's click
        pump(200);
        lst->set_sort_indicator(1, rap::sort_order::descending);
        pump(200);
        auto* sorter = GTK_COLUMN_VIEW_SORTER(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(cv_w)));
        expect(gtk_column_view_sorter_get_primary_sort_column(sorter) == c1 &&
                   gtk_column_view_sorter_get_primary_sort_order(sorter) == GTK_SORT_DESCENDING,
               "a list sorted by the user, given its old sort indicator: column 1 descending again");
        lst->set_column_widths({150, 130});
        pump(200);
        gtk_column_view_column_set_fixed_width(c0, 210);  // the user's resize
        pump(200);
        lst->set_column_widths({150, 130});
        pump(200);
        expect(gtk_column_view_column_get_fixed_width(c0) == 150,
               "a column resized by the user, given its old width: 150 again (" +
                   std::to_string(gtk_column_view_column_get_fixed_width(c0)) + ")");
        gtk_column_view_insert_column(GTK_COLUMN_VIEW(cv_w), 0, c1);  // the user's drag
        pump(200);
        lst->set_column_order({0, 1});
        pump(200);
        auto* first = static_cast<GtkColumnViewColumn*>(g_list_model_get_item(cols_now, 0));
        expect(first == c0, "columns moved by the user, given the old order: back");
        g_object_unref(first);
        g_object_unref(c0);
        g_object_unref(c1);
    }
    else
        expect(false, "the column view");
    // Fullscreen asked for and then not, before the first is answered: the
    // second isn't skipped; the window ends not fullscreen. (Broadway may
    // never answer: the bookkeeping is what's checked then.)
    win->set_fullscreen(true);
    win->set_fullscreen(false);
    pump(1500);
    expect(!gtk_window_is_fullscreen(GTK_WINDOW(top)), "fullscreen asked for, then not at once: not fullscreen");
    // The requests themselves, counted by the backend's seam: none answered
    // in between (no main loop run); a notify::fullscreened as the window
    // manager's (an outside change) sent through the backend's other seam.
    {
        using requests_fn = const char*();
        using seen_fn = void(void*, int);
        auto* requests =
            reinterpret_cast<requests_fn*>(backend_seam("rapport_gtk4", "rapport_gtk4_fullscreen_requests_for_tests"));
        auto* seen_seam = reinterpret_cast<seen_fn*>(backend_seam("rapport_gtk4", "rapport_gtk4_fullscreen_seen_for_tests"));
        expect(requests && seen_seam, "fullscreen: the backend's request counter and notifier");
        if (requests && seen_seam)
        {
            auto seen = [&](bool fs) { seen_seam(top, fs); };
            // Nothing pending, not fullscreen: whatever request is still pending is
            // answered by one of the two notifications (only its own state answers it).
            auto settled = [&]
            {
                seen(true);
                seen(false);
            };
            settled();
            requests();
            win->set_fullscreen(true);
            win->set_fullscreen(false);
            std::string r = requests();
            expect(r == "FU", "fullscreen: on then off, unanswered: both asked, in order (" + r + ")");
            settled();
            requests();
            win->set_fullscreen(true);
            win->set_fullscreen(true);
            r = requests();
            expect(r == "F", "fullscreen: on twice, unanswered: asked once (" + r + ")");
            settled();
            requests();
            win->set_fullscreen(true);
            win->set_fullscreen(false);
            seen(true);  // the first request's answer: the second still pending
            win->set_fullscreen(true);
            r = requests();
            expect(r == "FUF", "fullscreen: on, off, the first answered, on again: asked again, in order (" + r + ")");
            seen(true);
            win->set_fullscreen(false);
            settled();
            pump(500);
        }
    }
    // A date picker built with the default date shows it (the constructor's
    // set isn't skipped for matching a default).
    {
        GtkWidget* shown = nullptr;
        find_first(top,
                   [&](GtkWidget* w)
                   {
                       if (!GTK_IS_MENU_BUTTON(w)) return false;
                       const char* l = gtk_menu_button_get_label(GTK_MENU_BUTTON(w));
                       if (l && std::string(l).find("70") != std::string::npos) shown = w;
                       return shown != nullptr;
                   });
        expect(dp0 && dp0->date() == rap::date{} && shown != nullptr,
               "a date picker built with the default date (1970-01-01): it shows it");
    }
    // Enabled is a widget's own, not as its parent makes it: an entry's
    // action disabled itself, under its entry disabled and enabled again,
    // stays disabled; enabled itself (as it is) under a disabled entry, it's
    // enabled once the entry is.
    if (GtkWidget* act_w = find_first(top,
                                      [](GtkWidget* w)
                                      {
                                          if (!GTK_IS_BUTTON(w)) return false;
                                          const char* t = gtk_widget_get_tooltip_text(w);
                                          return t && std::string(t) == "Act";
                                      }))
    {
        act->set_enabled(true);
        acted->set_enabled(false);
        act->set_enabled(false);
        acted->set_enabled(true);
        pump(200);
        expect(!gtk_widget_get_sensitive(act_w) && !gtk_widget_is_sensitive(act_w),
               "an action disabled itself under its entry disabled and enabled again: still disabled");
        act->set_enabled(true);
        acted->set_enabled(false);
        act->set_enabled(true);  // as it is: nothing
        acted->set_enabled(true);
        pump(200);
        expect(gtk_widget_get_sensitive(act_w) && gtk_widget_is_sensitive(act_w),
               "an action enabled under a disabled entry: enabled once the entry is");
    }
    else
        expect(false, "the entry's action");

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
