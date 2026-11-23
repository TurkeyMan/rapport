// GTK 4 widgets that outlive their rapport objects call nothing of them
// (#140). Some of what GTK 4 calls back isn't a signal (a drawing area's draw
// func, a tree list model's child function, a tick callback), so taking a
// widget's signals off doesn't take it off; once the widget's rapport object
// is gone, it must be.
//
// The rapport window's content (a canvas, a list with a painted column, a
// tree) is moved into a window of the test's own, the rapport window is
// destroyed, and the widgets, alive and shown in the test's window, are
// painted (through a GtkWidgetPaintable, so whether or not a frame comes)
// and a tree row is expanded. Nothing of the canvas, the list or the tree is
// called: its on_paint, the model's paint (through the list), the tree's
// on_expanding. (Under ASan, any of them reading a freed object fails too.)
//   rapport-outlived-widgets-test-gtk4   (needs a display; GDK_BACKEND=broadway will do)
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#include <dlfcn.h>
#include <link.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
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
        while (g_main_context_iteration(nullptr, false))
        {
        }
    while (std::chrono::steady_clock::now() < until);
}

int canvas_paints = 0, cell_paints = 0, expandings = 0, control_paints = 0, toggles = 0,
    drag_begins = 0, badge_draws = 0, menu_builds = 0, actions_run = 0, tree_toggles = 0,
    icon_draws = 0, tab_icon_draws = 0, after_ending = 0;

// The app's drawing for a row's icon ends the list (its window destroyed)
// when this is set; what it or the overlay draw after counts.
std::unique_ptr<rap::window>* g_window = nullptr;
bool g_end_in_draw = false, g_ended_by_draw = false;

class painted_model : public rap::list_model
{
public:
    size_t row_count() const override { return 20; }
    std::string text(size_t row, size_t) const override { return "Row " + std::to_string(row); }
    void paint(size_t, size_t, rap::painter&, const rap::cell_info&) const override
    {
        ++cell_paints;
    }
    void toggle_checked(size_t) override { ++toggles; }
};
painted_model g_model;

// A text column's rows with an icon drawn by the app, and an overlay on it:
// a list's badge.
class iconed_model : public rap::list_model
{
public:
    size_t row_count() const override { return 5; }
    std::string text(size_t row, size_t) const override { return "Item " + std::to_string(row); }
    rap::icon icon(size_t, size_t) const override
    {
        rap::icon i;
        i.draw = [](rap::painter&, rap::size_f)
        {
            if (g_ended_by_draw) ++after_ending;
            ++badge_draws;
            if (g_end_in_draw && g_window)
            {
                g_end_in_draw = false;
                g_ended_by_draw = true;
                g_window->reset();
            }
        };
        i.overlay = [](rap::painter&, rap::size_f)
        {
            if (g_ended_by_draw) ++after_ending;
            ++badge_draws;
        };
        return i;
    }
};
iconed_model g_iconed;

// A test seam of the backend's (`name`): in its module, loaded already
// (`module`) by ui::load, or linked in.
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

// Every widget under `w` (itself too) of type `type`, in order.
void all_of(GtkWidget* w, GType type, std::vector<GtkWidget*>& out)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE(w, type)) out.push_back(w);
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        all_of(c, type, out);
}

// `w` painted now, whatever the frame clock's doing: what its drawing areas
// draw is drawn.
void paint_now(GtkWidget* w)
{
    // Laid out (Broadway, with no viewer, runs a window's frames only now
    // and then), and drawn again, not a render node kept from before.
    int min_w = 0, min_h = 0;
    gtk_widget_measure(w, GTK_ORIENTATION_HORIZONTAL, -1, &min_w, nullptr, nullptr, nullptr);
    gtk_widget_measure(w, GTK_ORIENTATION_VERTICAL, -1, &min_h, nullptr, nullptr, nullptr);
    gtk_widget_allocate(w, std::max(min_w, 400), std::max(min_h, 500), -1, nullptr);
    std::vector<GtkWidget*> areas;
    all_of(w, GTK_TYPE_DRAWING_AREA, areas);
    for (GtkWidget* a : areas) gtk_widget_queue_draw(a);
    GdkPaintable* paintable = gtk_widget_paintable_new(w);
    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, gtk_widget_get_width(w), gtk_widget_get_height(w));
    if (GskRenderNode* node = gtk_snapshot_free_to_node(snapshot)) gsk_render_node_unref(node);
    g_object_unref(paintable);
}

}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto ui = rap::ui::load(argc, argv, {.backend = "gtk4"});
    if (!ui) return 2;
    using namespace rap::layout;
    std::vector<rap::column> cols{
        {.title = "Graph", .width = 200_px, .stretch = true, .painted = true}};
    std::vector<rap::column> text_cols{{.title = "Name", .width = 200_px, .stretch = true}};
    rap::list* listed = nullptr;
    rap::tree* treed = nullptr;
    rap::tree_item folder{"Folder", "f", {{"Inside", "f/i"}}};
    rap::icon drawn_icon, drawn_tab_icon;
    drawn_icon.draw = [](rap::painter&, rap::size_f) { ++icon_draws; };
    drawn_tab_icon.draw = [](rap::painter&, rap::size_f) { ++tab_icon_draws; };
    folder.check = rap::check_state::unchecked;
    auto win = (*ui)->create(window{
        "Outlived widgets",
        {vbox{canvas{}.on_paint([](rap::painter&, rap::size_f) { ++canvas_paints; }).stretch(),
              list{cols, &g_model}.checkboxes().bind(listed).stretch(),
              list{text_cols, &g_iconed}.stretch(), button{"Drawn"}.icon(drawn_icon),
              tabs{page{"First", label{"one"}}.icon(drawn_tab_icon), page{"Second", label{"two"}}},
              menu_button{"Menu"}.on_menu(
                  []
                  {
                      ++menu_builds;
                      return rap::menu{rap::menu_item{"Do", [] { ++actions_run; }}};
                  }),
              tree{{folder}}
                  .on_expanding([](const std::string&) { ++expandings; })
                  .on_toggled([](const std::string&) { ++tree_toggles; })
                  .bind(treed)
                  .stretch()}}}.size(400_px, 700_px));
    g_window = &win;
    win->show();
    pump(600);
    if (!listed || !treed) return 2;
    // A drag from a cell: its gesture and its keys on the list.
    rap::list::cell_drag drag;
    drag.begin = [](size_t, size_t, rap::point_f) -> std::optional<rap::list::cell_drag_start>
    {
        ++drag_begins;
        return std::nullopt;
    };
    listed->on_cell_drag(std::move(drag));

    GListModel* tops = gtk_window_get_toplevels();
    GtkWidget* rapport_window =
        g_list_model_get_n_items(tops) ? GTK_WIDGET(g_list_model_get_item(tops, 0)) : nullptr;
    GtkWidget* content =
        rapport_window ? gtk_window_get_child(GTK_WINDOW(rapport_window)) : nullptr;
    if (rapport_window) g_object_unref(rapport_window);  // (get_item's reference)
    expect(content != nullptr, "the rapport window's content");
    if (!content) return 1;

    // Painted while the rapport objects are there: each draws.
    paint_now(content);
    expect(canvas_paints > 0 && cell_paints > 0 && badge_draws > 0 && icon_draws > 0 &&
               tab_icon_draws > 0,
           "before: the canvas, the list's painted cells, a row's badge, a button's icon and a "
           "tab's icon draw (" +
               std::to_string(canvas_paints) + ", " + std::to_string(cell_paints) + ", " +
               std::to_string(badge_draws) + ", " + std::to_string(icon_draws) + ", " +
               std::to_string(tab_icon_draws) + ")");

    // The menu button's menu built once, and its item's action there.
    std::vector<GtkWidget*> menu_buttons;
    all_of(content, GTK_TYPE_MENU_BUTTON, menu_buttons);
    expect(menu_buttons.size() == 1, "the menu button");
    if (menu_buttons.empty()) return 1;
    GtkWidget* menu_button = menu_buttons[0];
    gtk_menu_button_popup(GTK_MENU_BUTTON(menu_button));
    pump(100);
    gtk_menu_button_popdown(GTK_MENU_BUTTON(menu_button));
    const bool activated = gtk_widget_activate_action(menu_button, "mb.i0", nullptr);
    // The item's action itself, held as GTK or an app may hold one.
    using action_fn = void* (*)(void*, const char*);
    auto* menu_action = reinterpret_cast<action_fn>(
        backend_seam("rapport_gtk4", "rapport_gtk4_menu_action_for_tests"));
    auto* held_action =
        menu_action ? static_cast<GAction*>(menu_action(menu_button, "i0")) : nullptr;
    expect(held_action != nullptr,
           "the menu's first item's action, held (rapport_gtk4_menu_action_for_tests)");
    expect(menu_builds == 1 && activated && actions_run == 1,
           "before: the menu built as it opens (" + std::to_string(menu_builds) +
               "), its item's action run (" + std::to_string(actions_run) + ")");

    // A row's box kept elsewhere, the tree's rows then replaced (the tree
    // still there): toggled, it reaches no row it no longer shows.
    std::vector<GtkWidget*> checks;
    all_of(content, GTK_TYPE_CHECK_BUTTON, checks);
    GtkWidget* kept_check = nullptr;
    for (GtkWidget* c : checks)
        if (gtk_widget_get_visible(c)) kept_check = c;
    expect(kept_check != nullptr, "a tree row's box");
    if (kept_check) g_object_ref(kept_check);
    treed->set_items({{"Other", "o"}});
    pump(200);
    tree_toggles = 0;
    if (kept_check)
    {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(kept_check),
                                    !gtk_check_button_get_active(GTK_CHECK_BUTTON(kept_check)));
        pump(50);
    }
    expect(tree_toggles == 0, "a row's box kept past its row, toggled: no row's toggled (" +
                                  std::to_string(tree_toggles) + ")");

    // The content into the test's own window, then the rapport window gone.
    g_object_ref(content);
    gtk_window_set_child(GTK_WINDOW(rapport_window), nullptr);
    GtkWidget* keeper = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(keeper), 400, 500);
    gtk_window_set_child(GTK_WINDOW(keeper), content);
    g_object_unref(content);
    gtk_widget_set_visible(keeper, true);
    pump(300);
    // Painted, the app's drawing for a row's icon ends the list (its window
    // destroyed): neither that row's overlay nor anything after draws the
    // app's.
    g_end_in_draw = true;
    for (int i = 0; i < 5 && !g_ended_by_draw; ++i)
    {
        paint_now(content);
        pump(100);
    }
    expect(g_ended_by_draw, "a row's icon drawing ended the list (its window destroyed)");
    expect(after_ending == 0, "after: not its overlay, nor any row's drawing after it (" +
                                  std::to_string(after_ending) + ")");
    win.reset();  // (gone already)
    pump(100);

    // A drawing area of the test's own beside them, so that what's drawn
    // after is known to have been drawn at all.
    GtkWidget* control = gtk_drawing_area_new();
    gtk_widget_set_size_request(control, 50, 50);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(control),
        +[](GtkDrawingArea*, cairo_t*, int, int, gpointer) { ++control_paints; }, nullptr, nullptr);
    gtk_box_append(GTK_BOX(content), control);
    canvas_paints = cell_paints = expandings = control_paints = badge_draws = icon_draws =
        tab_icon_draws = 0;
    for (int i = 0; i < 5 && !control_paints; ++i)
    {
        paint_now(content);
        pump(200);
    }
    expect(control_paints > 0, "after: the window drawn (a drawing area of the test's own in it: " +
                                   std::to_string(control_paints) + ")");
    expect(canvas_paints == 0,
           "after: the canvas's on_paint isn't called (" + std::to_string(canvas_paints) + ")");
    expect(cell_paints == 0,
           "after: the list's painted cells don't reach it (" + std::to_string(cell_paints) + ")");
    expect(badge_draws == 0, "after: a row's badge doesn't draw the app's icon (" +
                                 std::to_string(badge_draws) + ")");
    expect(icon_draws == 0,
           "after: a button's icon doesn't draw the app's (" + std::to_string(icon_draws) + ")");
    expect(tab_icon_draws == 0,
           "after: a tab's icon doesn't draw the app's (" + std::to_string(tab_icon_draws) + ")");

    // The menu button opened, its action asked for: neither reaches it.
    menu_builds = actions_run = 0;
    gtk_menu_button_popup(GTK_MENU_BUTTON(menu_button));
    pump(100);
    gtk_menu_button_popdown(GTK_MENU_BUTTON(menu_button));
    gtk_widget_activate_action(menu_button, "mb.i0", nullptr);
    if (held_action) g_action_activate(held_action, nullptr);
    pump(50);
    expect(menu_builds == 0, "after: opening the menu button doesn't build its menu (" +
                                 std::to_string(menu_builds) + ")");
    expect(actions_run == 0,
           "after: its item's action, asked for or activated held, doesn't run (" +
               std::to_string(actions_run) + ")");
    if (held_action) g_object_unref(held_action);

    // The tree's row expanded: its child function asked.
    std::vector<GtkWidget*> views;
    all_of(content, GTK_TYPE_LIST_VIEW, views);
    GtkTreeListRow* row = nullptr;
    for (GtkWidget* v : views)
    {
        GtkSelectionModel* sel = gtk_list_view_get_model(GTK_LIST_VIEW(v));
        GListModel* model = sel && GTK_IS_SINGLE_SELECTION(sel)
                                ? gtk_single_selection_get_model(GTK_SINGLE_SELECTION(sel))
                                : nullptr;
        if (model && GTK_IS_TREE_LIST_MODEL(model))
            row = gtk_tree_list_model_get_row(GTK_TREE_LIST_MODEL(model), 0);
    }
    expect(row != nullptr, "the tree's first row");
    if (row)
    {
        gtk_tree_list_row_set_expanded(row, true);
        pump(100);
        g_object_unref(row);
    }
    expect(expandings == 0,
           "after: expanding the tree's row doesn't reach it (" + std::to_string(expandings) + ")");

    // The list's own controllers on its column view (a middle click's
    // scrolling, the pointer's motion, a cell's drag, Space for a row's box,
    // Escape for a drag), each given input as GTK would: none reaches it.
    std::vector<GtkWidget*> column_views;
    all_of(content, GTK_TYPE_COLUMN_VIEW, column_views);
    expect(column_views.size() == 2,
           "the lists' column views (the first, the one with boxes and a cell drag)");
    int given = 0;
    if (!column_views.empty())
    {
        GListModel* controllers = gtk_widget_observe_controllers(column_views[0]);
        for (guint i = 0, n = g_list_model_get_n_items(controllers); i < n; ++i)
        {
            auto* c = static_cast<GObject*>(g_list_model_get_item(controllers, i));
            gboolean handled = false;
            if (GTK_IS_GESTURE_CLICK(c) &&
                gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(c)) == GDK_BUTTON_MIDDLE)
            {
                g_signal_emit_by_name(c, "pressed", 1, 10.0, 30.0);
                g_signal_emit_by_name(c, "released", 1, 10.0, 30.0);
                ++given;
            }
            else if (GTK_IS_EVENT_CONTROLLER_MOTION(c))
            {
                g_signal_emit_by_name(c, "motion", 10.0, 40.0);
                ++given;
            }
            else if (GTK_IS_GESTURE_DRAG(c) && !GTK_IS_GESTURE_PAN(c))
            {
                g_signal_emit_by_name(c, "drag-begin", 10.0, 30.0);
                g_signal_emit_by_name(c, "drag-update", 5.0, 5.0);
                g_signal_emit_by_name(c, "drag-end", 5.0, 5.0);
                ++given;
            }
            else if (GTK_IS_EVENT_CONTROLLER_KEY(c))
            {
                g_signal_emit_by_name(c, "key-pressed", guint(GDK_KEY_space), 0u,
                                      GdkModifierType(0), &handled);
                g_signal_emit_by_name(c, "key-pressed", guint(GDK_KEY_Escape), 0u,
                                      GdkModifierType(0), &handled);
                ++given;
            }
            g_object_unref(c);
        }
        g_object_unref(controllers);
        pump(100);
    }
    expect(given >= 4, "the list's controllers given input (" + std::to_string(given) + ")");
    expect(toggles == 0 && drag_begins == 0, "after: none reaches the list (rows toggled " +
                                                 std::to_string(toggles) + ", drags begun " +
                                                 std::to_string(drag_begins) + ")");

    if (kept_check) g_object_unref(kept_check);
    gtk_window_destroy(GTK_WINDOW(keeper));
    pump(100);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
