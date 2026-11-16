// GTK 3: an app's row height for lists, trees and sidebars
// (list::row_height, tree::row_height, look_options::row_height), the rows
// measured off the desktop on Broadway. Rows are the logical px asked for at
// the text size (GTK's font DPI over 96), never shorter than the font's line
// and 2 px. (GTK's own scale factor is the device's, under logical px: the
// DPI isn't varied here.)
//
// Run without arguments: a list and a tree with row_height(20) measure it,
// and a list without one keeps GTK's rows (a lone plain list's). Then it runs
// itself again with --app, the app's row height 20 (look_options): a plain
// list, a tree and a sidebar's rows 20, a list's own row_height(28) over it,
// and a sidebar at its natural size with every row whole.
//   rapport-row-height-test-gtk3
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

std::string str(long v)
{
    return std::to_string(v);
}

void settle()
{
    for (int i = 0; i < 20; ++i)
    {
        while (gtk_events_pending()) gtk_main_iteration_do(false);
        g_usleep(5000);
    }
}

class rows_model : public rap::list_model
{
public:
    size_t row_count() const override { return 3; }
    std::string text(size_t row, size_t col) const override
    {
        return (col == 0 ? "Name " : "Note ") + std::to_string(row);
    }
};
rows_model g_model;

// Every widget of type `type` under `w`, in order.
void find_all(GtkWidget* w, GType type, std::vector<GtkWidget*>& out)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE(w, type)) out.push_back(w);
    if (!GTK_IS_CONTAINER(w)) return;
    GList* kids = gtk_container_get_children(GTK_CONTAINER(w));
    for (GList* l = kids; l; l = l->next) find_all(GTK_WIDGET(l->data), type, out);
    g_list_free(kids);
}
// The toplevel titled `title`.
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

// A tree view's first row's height (its background area: the whole row).
int view_row(GtkWidget* view)
{
    GtkTreePath* path = gtk_tree_path_new_first();
    GdkRectangle r{};
    gtk_tree_view_get_background_area(GTK_TREE_VIEW(view), path, nullptr, &r);
    gtk_tree_path_free(path);
    return r.height;
}

float text_scale()
{
    int dpi = 0;
    g_object_get(gtk_settings_get_default(), "gtk-xft-dpi", &dpi, nullptr);
    return dpi > 0 ? float(dpi) / 1024.f / 96.f : 1.f;
}
int font_line(GtkWidget* w)
{
    PangoFontMetrics* m = pango_context_get_metrics(gtk_widget_get_pango_context(w), nullptr, nullptr);
    const int line = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(m) + pango_font_metrics_get_descent(m));
    pango_font_metrics_unref(m);
    return line;
}
// The rows `logical` asks for in `w`.
int want(GtkWidget* w, float logical)
{
    return std::max(int(std::lround(logical * text_scale())), font_line(w) + 2);
}

int run_own(rap::ui& ui)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::vector<rap::column> cols{{"Name", 120_px}, {"Notes", 120_px}};
    const std::vector<rap::tree_item> items{{"Alpha", "a"}, {"Beta", "b"}};
    // A lone plain list: GTK's rows, as a list without row_height has them.
    auto ref = ui.create(window{"Plain", {list{cols, &g_model}.stretch()}}.size(400_px, 300_px));
    ref->show();
    settle();
    std::vector<GtkWidget*> views;
    if (GtkWidget* top = window_titled("Plain")) find_all(top, GTK_TYPE_TREE_VIEW, views);
    expect(views.size() == 1, "a lone plain list");
    const int plain = views.empty() ? -1 : view_row(views[0]);
    std::printf("note a plain list's rows %d\n", plain);

    auto win = ui.create(window{"Own rows",
                                {hbox{list{cols, &g_model}.row_height(20).stretch(), list{cols, &g_model}.stretch(),
                                      tree{items}.row_height(20).stretch()}}}
                             .size(900_px, 300_px));
    win->show();
    settle();
    views.clear();
    if (GtkWidget* top = window_titled("Own rows")) find_all(top, GTK_TYPE_TREE_VIEW, views);
    expect(views.size() == 3, "a list with row_height(20), a plain list, a tree with row_height(20)");
    if (views.size() != 3) return 1;
    expect(view_row(views[0]) == want(views[0], 20),
           "a list's rows its row_height, " + str(want(views[0], 20)) + ": " + str(view_row(views[0])));
    expect(view_row(views[1]) == plain,
           "a list without one, GTK's rows, " + str(plain) + ": " + str(view_row(views[1])));
    expect(view_row(views[2]) == want(views[2], 20),
           "a tree's rows its row_height, " + str(want(views[2], 20)) + ": " + str(view_row(views[2])));
    // A text size of 150% (GTK's font DPI, as GNOME's text scaling sets it),
    // the windows open: the rows again, 30; then back.
    int was = 0;
    g_object_get(gtk_settings_get_default(), "gtk-xft-dpi", &was, nullptr);
    g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 144 * 1024, nullptr);
    settle();
    expect(view_row(views[0]) == want(views[0], 20) && want(views[0], 20) == 30,
           "at 150% text a list's rows " + str(want(views[0], 20)) + ": " + str(view_row(views[0])));
    expect(view_row(views[2]) == want(views[2], 20),
           "at 150% text a tree's rows " + str(want(views[2], 20)) + ": " + str(view_row(views[2])));
    g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", was > 0 ? was : 96 * 1024, nullptr);
    settle();
    expect(view_row(views[0]) == want(views[0], 20),
           "back at its text size a list's rows " + str(want(views[0], 20)) + ": " + str(view_row(views[0])));
    return 0;
}

int run_app(rap::ui& ui)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::vector<rap::column> cols{{"Name", 120_px}, {"Notes", 120_px}};
    const std::vector<rap::tree_item> items{{"Alpha", "a"}, {"Beta", "b"}};
    auto win = ui.create(window{"App rows",
                                {hbox{list{cols, &g_model}.row_height(28).stretch(), list{cols, &g_model}.stretch(),
                                      tree{items}.stretch(),
                                      tabs{{"One", label{"one"}}, {"Two", label{"two"}}}.sidebar().stretch()}}}
                             .size(1200_px, 400_px));
    win->show();
    settle();
    GtkWidget* top = window_titled("App rows");
    std::vector<GtkWidget*> views, rows;
    if (top)
    {
        find_all(top, GTK_TYPE_TREE_VIEW, views);
        find_all(top, GTK_TYPE_LIST_BOX_ROW, rows);
    }
    expect(views.size() == 3 && rows.size() == 2, "a list with row_height(28), a plain list, a tree, a sidebar");
    if (views.size() != 3 || rows.size() != 2) return 1;
    expect(view_row(views[0]) == want(views[0], 28), "a list's own row_height over the app's, " +
                                                         str(want(views[0], 28)) + ": " + str(view_row(views[0])));
    expect(view_row(views[1]) == want(views[1], 20),
           "a plain list's rows the app's, " + str(want(views[1], 20)) + ": " + str(view_row(views[1])));
    expect(view_row(views[2]) == want(views[2], 20),
           "a tree's rows the app's, " + str(want(views[2], 20)) + ": " + str(view_row(views[2])));
    for (GtkWidget* row : rows)
        expect(gtk_widget_get_allocated_height(row) == want(row, 20),
               "a sidebar's row the app's, " + str(want(row, 20)) + ": " + str(gtk_widget_get_allocated_height(row)));

    // A sidebar of four pages, its window its natural size: every row whole.
    auto side = ui.create(window{"Sidebar",
                                 {tabs{{"General", label{"one"}},
                                       {"Appearance", label{"two"}},
                                       {"Fonts", label{"three"}},
                                       {"Advanced", label{"four"}}}
                                      .sidebar()}});
    side->show();
    settle();
    std::vector<GtkWidget*> boxes, side_rows;
    if (GtkWidget* t = window_titled("Sidebar"))
    {
        find_all(t, GTK_TYPE_LIST_BOX, boxes);
        find_all(t, GTK_TYPE_LIST_BOX_ROW, side_rows);
    }
    expect(boxes.size() == 1 && side_rows.size() == 4, "a sidebar of four pages at its natural size");
    if (boxes.size() == 1)
    {
        const int h = gtk_widget_get_allocated_height(boxes[0]);
        for (size_t i = 0; i < side_rows.size(); ++i)
        {
            GtkAllocation a{};
            gtk_widget_get_allocation(side_rows[i], &a);
            int y = 0;
            gtk_widget_translate_coordinates(side_rows[i], boxes[0], 0, 0, nullptr, &y);
            expect(y >= 0 && y + a.height <= h, "its row " + str(long(i)) + " whole, " + str(y) + ".." +
                                                    str(y + a.height) + " in 0.." + str(h));
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    const bool app = argc > 1 && std::string(argv[1]) == "--app";
    rap::load_options options;
    options.backend = "gtk3";
    if (app) options.look.row_height = 20;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui)
    {
        std::printf("FAIL no gtk3 backend: %s\n", ui.error().c_str());
        return 1;
    }
    std::printf("%s, text size %.2f\n", app ? "the app's row height 20" : "each control's own", text_scale());
    const int r = app ? run_app(**ui) : run_own(**ui);
    int status = 0;
    if (!app)
    {
        // The app's row height is the process's: the second half in a
        // process of its own.
        char* child_argv[] = {argv[0], const_cast<char*>("--app"), nullptr};
        GError* error = nullptr;
        if (!g_spawn_sync(nullptr, child_argv, nullptr, G_SPAWN_CHILD_INHERITS_STDIN, nullptr, nullptr, nullptr,
                          nullptr, &status, &error))
        {
            expect(false, std::string("the --app run started: ") + (error ? error->message : "?"));
            if (error) g_error_free(error);
        }
        else
            expect(g_spawn_check_wait_status(status, nullptr), "the --app run passed");
    }
    std::printf(fails || r ? "%d failed\n" : "all passed\n", fails);
    return fails || r ? 1 : 0;
}
