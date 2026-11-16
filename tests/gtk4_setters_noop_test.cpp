// A setter given the value a widget already has does nothing on GTK 4
// (#88): no property set (no "notify" from any widget in the window), no
// relayout and no redraw (the window's frame clock runs no "layout" or
// "paint" phase). Given a different value, it still does as before (a
// notify, a layout or a paint). Every setter of every widget, set to its
// value twice: the second time, nothing.
//   rapport-setters-noop-test-gtk4   (needs a display; GDK_BACKEND=broadway will do)
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#include <chrono>
#include <cstdio>
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
                   date_picker{rap::date{2024, 5, 6}}.bind(dp)},
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

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
