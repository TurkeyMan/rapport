// GTK 3: tree_item::separator_before puts separator rows in the tree view,
// which the cursor keys must step over and nothing may report. Driven
// through the view's own keybinding signal (move-cursor, what the arrow,
// Home/End and Page keys emit), off the desktop on Broadway.
//   rapport-tree-separator-keys-test-gtk3
#include <rapport/rapport.h>

#include <gtk/gtk.h>

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
    if (!ok) ++fails;
}

rap::tree_item item(std::string id, bool line, std::vector<rap::tree_item> children = {})
{
    rap::tree_item t;
    t.label = id;
    t.id = std::move(id);
    t.separator_before = line;
    t.children = std::move(children);
    t.expanded = !t.children.empty();
    return t;
}

GtkWidget* find_tree_view(GtkWidget* w)
{
    if (GTK_IS_TREE_VIEW(w)) return w;
    if (!GTK_IS_CONTAINER(w)) return nullptr;
    GList* kids = gtk_container_get_children(GTK_CONTAINER(w));
    GtkWidget* found = nullptr;
    for (GList* l = kids; l && !found; l = l->next) found = find_tree_view(GTK_WIDGET(l->data));
    g_list_free(kids);
    return found;
}

void settle()
{
    while (gtk_events_pending()) gtk_main_iteration_do(false);
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
    rap::tree* t = nullptr;
    std::vector<std::string> changes;
    using namespace rap::layout;
    auto win = (*ui)->create(window{
        "t",
        {tree{{item("a", false), item("b", true), item("c", true, {item("c1", true), item("c2", true)}),
               item("d", true)}}
             .bind(t)
             .on_selection_changed([&] { changes.push_back(t->selected().value_or("")); })}});
    win->show();
    settle();

    GtkWidget* view = nullptr;
    GList* tops = gtk_window_list_toplevels();
    for (GList* l = tops; l && !view; l = l->next) view = find_tree_view(GTK_WIDGET(l->data));
    g_list_free(tops);
    expect(view != nullptr, "the tree view is there");
    if (!view) return 1;
    // Nobody's at a Broadway display to focus the window: told it has the
    // focus, as GTK tells its own popups, so the keys are the tree's.
    GtkWidget* top = gtk_widget_get_toplevel(view);
    GdkEvent* focus = gdk_event_new(GDK_FOCUS_CHANGE);
    focus->focus_change.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(top)));
    focus->focus_change.send_event = TRUE;
    focus->focus_change.in = TRUE;
    gtk_widget_send_focus_change(top, focus);
    gdk_event_free(focus);
    gtk_widget_grab_focus(view);
    settle();

    // What the view holds: separator rows before b, c and d at the top, and
    // c2 under c (c1 is first under c, and a the first row: none).
    GtkTreeModel* model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    // The rows, for when a check fails: label, id, separator.
    auto dump = [&]
    {
        std::string out;
        std::function<void(GtkTreeIter*, int)> walk = [&](GtkTreeIter* parent, int depth)
        {
            GtkTreeIter it;
            for (bool ok = gtk_tree_model_iter_children(model, &it, parent); ok;
                 ok = gtk_tree_model_iter_next(model, &it))
            {
                gchar *label = nullptr, *id = nullptr;
                gboolean sep = false;
                gtk_tree_model_get(model, &it, 0, &label, 1, &id, 4, &sep, -1);
                out += std::string(size_t(depth) * 2, ' ') + (sep ? std::string("----") : std::string(label ? label : "?")) +
                       "\n";
                g_free(label);
                g_free(id);
                walk(&it, depth + 1);
            }
        };
        walk(nullptr, 0);
        std::printf("rows:\n%s", out.c_str());
    };
    const bool rows_right = gtk_tree_model_iter_n_children(model, nullptr) == 7;
    if (!rows_right) dump();
    expect(rows_right, "three top-level separator rows, before b, c and d");

    t->select("a");
    changes.clear();
    auto move = [&](GtkMovementStep step, int count)
    {
        gboolean done = false;
        g_signal_emit_by_name(view, "move-cursor", step, count, &done);
        settle();
    };
    const std::vector<std::string> down = {"b", "c", "c1", "c2", "d"};
    expect(gtk_widget_has_focus(view), "the tree has the keyboard focus");
    for (const std::string& want : down)
    {
        move(GTK_MOVEMENT_DISPLAY_LINES, 1);
        expect(t->selected() == std::optional<std::string>(want),
               "Down onto " + want + ", past any line (on " + t->selected().value_or("(none)") + ")");
    }
    for (auto it = down.rbegin() + 1; it != down.rend(); ++it)
    {
        move(GTK_MOVEMENT_DISPLAY_LINES, -1);
        expect(t->selected() == std::optional<std::string>(*it), "Up onto " + *it);
    }
    move(GTK_MOVEMENT_DISPLAY_LINES, -1);
    expect(t->selected() == std::optional<std::string>("a"), "Up onto a");
    move(GTK_MOVEMENT_BUFFER_ENDS, 1);
    expect(t->selected() == std::optional<std::string>("d"), "End onto d");
    move(GTK_MOVEMENT_BUFFER_ENDS, -1);
    expect(t->selected() == std::optional<std::string>("a"), "Home onto a");
    move(GTK_MOVEMENT_PAGES, 1);
    expect(t->selected() && !t->selected()->empty(), "PageDown lands on an item");
    move(GTK_MOVEMENT_PAGES, -1);
    expect(t->selected() == std::optional<std::string>("a"), "PageUp back onto a");

    bool all_named = true;
    for (const std::string& c : changes) all_named &= !c.empty();
    expect(!changes.empty() && all_named, "every change reported names an item, never a separator");

    // Lines on and off in place, and a refresh, keep the rows right.
    t->set_separator_before("b", false);
    expect(gtk_tree_model_iter_n_children(model, nullptr) == 6, "turning b's line off takes its row out");
    t->set_separator_before("b", true);
    t->set_separator_before("a", true);
    expect(gtk_tree_model_iter_n_children(model, nullptr) == 7, "on again puts it back; a, first, gets none");
    t->select("c2");
    t->set_children("c", {item("c0", false), item("c2", true)});
    expect(t->selected() == std::optional<std::string>("c2"), "set_children keeps the selection");
    GtkTreePath* c_path = gtk_tree_path_new_from_indices(4, -1);  // a, sep, b, sep, c
    GtkTreeIter c_it;
    expect(gtk_tree_model_get_iter(model, &c_it, c_path) && gtk_tree_model_iter_n_children(model, &c_it) == 3,
          "c's new children: c0, a separator row, c2");
    gtk_tree_path_free(c_path);

    win->close();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
