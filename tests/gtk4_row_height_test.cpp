// An app's row height on GTK 4 (list::row_height, tree::row_height,
// look_options::row_height): each row round(row_height x the text size)
// logical px, never shorter than the font's line and 2, measured from the
// rows' own widgets. A list's or a tree's own wins over the app's; a list
// without either keeps its own rows; a sidebar's rows the app's, and at its
// natural size every row whole.
//   rapport-row-height-test-gtk4 [--app]   (needs a display; GDK_BACKEND=broadway will do)
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#include <algorithm>
#include <chrono>
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
    if (!ok) ++fails;
}

void pump(int ms = 300)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do
        while (g_main_context_iteration(nullptr, false)) {}
    while (std::chrono::steady_clock::now() < until);
}

// The first row's height as it asks for it now (its size request): what
// a desktop's frame clock lays it out at, at once. (Broadway with no viewer
// runs frames for a shown window only now and then, so its allocation can
// lag a change.)
int first_row_request(GtkWidget* view)
{
    for (GtkWidget* c = gtk_widget_get_first_child(view); c; c = gtk_widget_get_next_sibling(c))
        if (!std::string(gtk_widget_get_css_name(c)).compare("row"))
        {
            int min = 0;
            gtk_widget_measure(c, GTK_ORIENTATION_VERTICAL, -1, &min, nullptr, nullptr, nullptr);
            return min;
        }
    return -1;
}

std::string str(long v)
{
    return std::to_string(v);
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

// Every widget under `w` (itself too) of type `type`, in order.
void all_of(GtkWidget* w, GType type, std::vector<GtkWidget*>& out)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE(w, type)) out.push_back(w);
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) all_of(c, type, out);
}

// The first row's height: a view's row widgets are its children named "row".
int first_row(GtkWidget* view)
{
    for (GtkWidget* c = gtk_widget_get_first_child(view); c; c = gtk_widget_get_next_sibling(c))
        if (!std::string(gtk_widget_get_css_name(c)).compare("row")) return gtk_widget_get_height(c);
    return -1;
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
    const int line = int(std::ceil(double(pango_font_metrics_get_height(m)) / PANGO_SCALE));
    pango_font_metrics_unref(m);
    return line;
}
// The rows `logical` asks for in view `w`.
int want(GtkWidget* w, float logical)
{
    return std::max(int(std::lround(logical * text_scale())), font_line(w) + 2);
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

// The views of window `top`: column views' inner list views, then the
// trees' list views, left to right as built.
std::vector<GtkWidget*> column_views(GtkWidget* top)
{
    std::vector<GtkWidget*> v;
    all_of(top, GTK_TYPE_COLUMN_VIEW, v);
    return v;
}
GtkWidget* rows_of(GtkWidget* column_view)
{
    std::vector<GtkWidget*> v;
    all_of(column_view, GTK_TYPE_LIST_VIEW, v);
    return v.empty() ? nullptr : v[0];
}
std::vector<GtkWidget*> tree_views(GtkWidget* top)
{
    std::vector<GtkWidget*> all, out;
    all_of(top, GTK_TYPE_LIST_VIEW, all);
    for (GtkWidget* w : all)
        if (!GTK_IS_COLUMN_VIEW(gtk_widget_get_parent(w))) out.push_back(w);
    return out;
}

int run_own(rap::ui& ui)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::vector<rap::column> cols{{"Name", 120_px}, {"Notes", 120_px}};
    auto win = ui.create(window{"Row height",
                                {hbox{list{cols, &g_model}.row_height(20).stretch(),
                                      list{cols, &g_model}.compact().row_height(20).stretch(),
                                      list{cols, &g_model}.stretch(),
                                      tree{{{"Alpha", "a"}, {"Beta", "b"}}}.row_height(20).stretch(),
                                      list{cols, &g_model}.row_height(32).stretch(),
                                      list{cols, &g_model}.row_height(1).stretch()}}}
                         .size(900_px, 400_px));
    win->show();
    pump();
    GtkWidget* top = window_titled("Row height");
    expect(top != nullptr, "the window");
    if (!top) return 1;
    const std::vector<GtkWidget*> lists = column_views(top), trees = tree_views(top);
    expect(lists.size() == 5 && trees.size() == 1, "five lists and a tree");
    if (lists.size() != 5 || trees.size() != 1) return 1;
    // 1, far under the text: its line and 2, exactly (no room round the
    // text where the rows are the app's).
    {
        GtkWidget* rows = rows_of(lists[4]);
        const int now = first_row(rows), w = font_line(rows) + 2;
        expect(now == w, "a list's rows its own 1: its line and 2, " + str(w) + ": " + str(now));
    }
    // 32, taller than any list's own rows: the override itself.
    {
        GtkWidget* rows = rows_of(lists[3]);
        const int now = first_row(rows), w = want(rows, 32);
        expect(now == w, "a list's rows its own 32, " + str(w) + ": " + str(now));
    }
    for (size_t i = 0; i < 2; ++i)
    {
        GtkWidget* rows = rows_of(lists[i]);
        const int now = first_row(rows), w = want(rows, 20);
        expect(now == w, std::string(i ? "a compact list" : "a list") + "'s rows its own, " + str(w) + ": " + str(now));
    }
    const int plain = first_row(rows_of(lists[2])), tree_now = first_row(trees[0]),
              tree_want = want(trees[0], 20);
    // Without one: no row height of rapport's on it, its rows its own.
    bool classed = false;
    for (GtkWidget* w : {rows_of(lists[2]), lists[2]})
    {
        char** classes = gtk_widget_get_css_classes(w);
        for (char** c = classes; c && *c; ++c) classed = classed || std::string(*c).rfind("rapport-row-", 0) == 0;
        g_strfreev(classes);
    }
    expect(plain > 0 && !classed, "a list without one keeps its own rows: " + str(plain));
    expect(tree_now == tree_want, "a tree's rows its own, " + str(tree_want) + ": " + str(tree_now));
    std::printf("note text size %.2f, a plain list's rows %d\n", text_scale(), plain);
    // A larger text size (GTK's font DPI, this process's settings only):
    // the rows made again, 150% of 20 (the font's line and 2 at least).
    int was = 0;
    g_object_get(gtk_settings_get_default(), "gtk-xft-dpi", &was, nullptr);
    g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024 * 3 / 2, nullptr);
    pump();
    {
        GtkWidget* rows = rows_of(lists[0]);
        const int now = first_row_request(rows), w = want(rows, 20);
        expect(now == w && w >= 30, "at a 150% text size a list's rows " + str(w) + ": " + str(now));
        const int tnow = first_row_request(trees[0]), tw = want(trees[0], 20);
        expect(tnow == tw, "at a 150% text size a tree's rows " + str(tw) + ": " + str(tnow));
        GtkWidget* small = rows_of(lists[4]);
        const int snow = first_row_request(small), sw = font_line(small) + 2;
        expect(snow == sw, "at a 150% text size a list's own 1: its larger line and 2, " + str(sw) + ": " + str(snow));
    }
    g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", was, nullptr);
    pump();
    {
        GtkWidget* rows = rows_of(lists[0]);
        expect(first_row_request(rows) == want(rows, 20), "back at its text size, a list's rows " +
                                                              str(want(rows, 20)) + ": " +
                                                              str(first_row_request(rows)));
    }
    return 0;
}

int run_app(rap::ui& ui)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::vector<rap::column> cols{{"Name", 120_px}, {"Notes", 120_px}};
    auto win = ui.create(window{"Row height app",
                                {hbox{list{cols, &g_model}.row_height(28).stretch(), list{cols, &g_model}.stretch(),
                                      list{cols, &g_model}.file_view().stretch(),
                                      tree{{{"Alpha", "a"}, {"Beta", "b"}}}.stretch(),
                                      tree{{{"Alpha", "a"}, {"Beta", "b"}}}.navigator().stretch()}}}
                         .size(1100_px, 400_px));
    win->show();
    pump();
    GtkWidget* top = window_titled("Row height app");
    expect(top != nullptr, "the window");
    if (!top) return 1;
    const std::vector<GtkWidget*> lists = column_views(top), trees = tree_views(top);
    expect(lists.size() == 3 && trees.size() == 2, "three lists and two trees");
    if (lists.size() != 3 || trees.size() != 2) return 1;
    {
        GtkWidget* rows = rows_of(lists[0]);
        const int now = first_row(rows), w = want(rows, 28);
        expect(now == w, "a list's own row_height (28) over the app's, " + str(w) + ": " + str(now));
    }
    const char* names[] = {"a plain list", "a file_view"};
    for (size_t i = 1; i < 3; ++i)
    {
        GtkWidget* rows = rows_of(lists[i]);
        const int now = first_row(rows), w = want(rows, 20);
        expect(now == w, std::string(names[i - 1]) + "'s rows the app's, " + str(w) + ": " + str(now));
    }
    for (size_t i = 0; i < 2; ++i)
    {
        const int now = first_row(trees[i]), w = want(trees[i], 20);
        expect(now == w, std::string(i ? "a navigator" : "a tree") + "'s rows the app's, " + str(w) + ": " +
                             str(now));
    }
    // A sidebar of four pages in a window of its natural size: its rows the
    // app's, every one whole.
    auto side = ui.create(window{"Row height sidebar",
                                 {tabs{{"General", label{"one"}},
                                       {"Appearance", label{"two"}},
                                       {"Fonts", label{"three"}},
                                       {"Advanced", label{"four"}}}
                                      .sidebar()}});
    side->show();
    pump();
    GtkWidget* stop = window_titled("Row height sidebar");
    std::vector<GtkWidget*> boxes;
    if (stop) all_of(stop, GTK_TYPE_LIST_BOX, boxes);
    expect(boxes.size() == 1, "a sidebar in a window of its natural size");
    if (boxes.size() == 1)
    {
        GtkWidget* box = boxes[0];
        const int w = want(box, 20), box_h = gtk_widget_get_height(box);
        int i = 0;
        for (GtkWidget* r = gtk_widget_get_first_child(box); r; r = gtk_widget_get_next_sibling(r), ++i)
        {
            graphene_rect_t b{};
            const bool placed = gtk_widget_compute_bounds(r, box, &b);
            expect(gtk_widget_get_height(r) == w, "the sidebar's row " + str(i) + " the app's, " + str(w) + ": " +
                                                      str(gtk_widget_get_height(r)));
            expect(placed && b.origin.y >= 0 && b.origin.y + b.size.height <= box_h + 0.5f,
                   "its row " + str(i) + " whole, " + str(long(b.origin.y)) + ".." +
                       str(long(b.origin.y + b.size.height)) + " in 0.." + str(box_h));
        }
        expect(i == 4, "its four rows: " + str(i));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    bool app = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--app") app = true;
    // Without --app: this run's, then the app's (look_options::row_height
    // is the backend's from its start: a process of its own), so one
    // command runs both.
    int app_code = 0;
    if (!app)
    {
        gchar* out = nullptr;
        gint status = 0;
        const gchar* args[] = {argv[0], "--app", nullptr};
        GError* error = nullptr;
        if (g_spawn_sync(nullptr, const_cast<gchar**>(args), nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, &out,
                         nullptr, &status, &error))
        {
            std::printf("--app:\n%s", out ? out : "");
            app_code = g_spawn_check_wait_status(status, nullptr) ? 0 : 1;
        }
        else
        {
            std::printf("FAIL --app not run: %s\n", error ? error->message : "?");
            app_code = 1;
        }
        g_free(out);
        if (error) g_error_free(error);
    }
    rap::load_options options;
    options.backend = "gtk4";
    if (app) options.look.row_height = 20;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui)
    {
        std::printf("skipped: no GTK 4 backend or display\n");
        return 77;
    }
    const int r = app ? run_app(**ui) : run_own(**ui);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails || r || app_code ? 1 : 0;
}
