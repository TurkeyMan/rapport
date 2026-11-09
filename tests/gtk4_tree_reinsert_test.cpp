// A GTK 4 tree's row replaced in its parent's store (how a row gains or loses
// its chevron, or shows new text): selection, focus and what's open all stay
// by id.
//   rapport-tree-reinsert-test-gtk4   (needs a display; GDK_BACKEND=broadway will do)
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}

void pump(int ms = 150)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do
        while (g_main_context_iteration(nullptr, false)) {}
    while (std::chrono::steady_clock::now() < until);
}

GtkWidget* find_list_view(GtkWidget* w)
{
    if (GTK_IS_LIST_VIEW(w)) return w;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* found = find_list_view(c)) return found;
    return nullptr;
}

std::string id_of(GtkTreeListRow* row)
{
    auto* o = static_cast<GtkStringObject*>(gtk_tree_list_row_get_item(row));
    std::string id = o ? gtk_string_object_get_string(o) : "";
    if (o) g_object_unref(o);
    return id;
}

struct view
{
    GtkWidget* list;
    GListModel* rows() const { return G_LIST_MODEL(gtk_list_view_get_model(GTK_LIST_VIEW(list))); }
    guint count() const { return g_list_model_get_n_items(rows()); }
    GtkTreeListRow* row(const std::string& id) const  // unreferenced; held by the model
    {
        for (guint i = 0; i < count(); ++i)
        {
            auto* r = static_cast<GtkTreeListRow*>(g_list_model_get_item(rows(), i));
            g_object_unref(r);
            if (id_of(r) == id) return r;
        }
        return nullptr;
    }
    bool open(const std::string& id) const
    {
        GtkTreeListRow* r = row(id);
        return r && gtk_tree_list_row_get_expanded(r);
    }
    bool chevron(const std::string& id) const
    {
        GtkTreeListRow* r = row(id);
        return r && gtk_tree_list_row_is_expandable(r);
    }
    void focus(const std::string& id) const
    {
        gtk_list_view_scroll_to(GTK_LIST_VIEW(list), gtk_tree_list_row_get_position(row(id)), GTK_LIST_SCROLL_FOCUS,
                                nullptr);
        pump();
    }
    std::string focused() const
    {
        GtkWidget* f = gtk_root_get_focus(gtk_widget_get_root(list));
        if (!f || f == list || !gtk_widget_is_ancestor(f, list)) return {};
        while (gtk_widget_get_parent(f) != list) f = gtk_widget_get_parent(f);
        GtkWidget* child = gtk_widget_get_first_child(f);
        if (!child || !GTK_IS_TREE_EXPANDER(child)) return {};
        GtkTreeListRow* r = gtk_tree_expander_get_list_row(GTK_TREE_EXPANDER(child));
        return r ? id_of(r) : std::string();
    }
};

rap::tree_item item(std::string id, bool lazy = false, bool expanded = false)
{
    rap::tree_item t;
    t.label = id;
    t.id = std::move(id);
    t.has_children = lazy;
    t.expanded = expanded;
    return t;
}

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options options;
    options.backend = "gtk4";
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui)
    {
        std::printf("FAIL %s\n", ui.error().c_str());
        return 1;
    }
    rap::tree* t = nullptr;
    rap::tree_item a = item("a", false, true);
    a.children = {item("x"), item("y")};
    using namespace rap::layout;
    auto win = (*ui)->create(window{"t", {tree{{a, item("n", true, true), item("b")}}
                                              .bind(t)
                                              .on_expanding([](const std::string&) {})}}
                                 .size(30_ch, 16_lines));
    win->show();
    pump(400);

    GListModel* tops = gtk_window_get_toplevels();
    GtkWidget* top = GTK_WIDGET(g_list_model_get_item(tops, 0));
    view v{find_list_view(top)};
    g_object_unref(top);
    if (!v.list)
    {
        std::printf("FAIL no list view\n");
        return 1;
    }

    // A row that stays, with new text.
    t->select("x");
    v.focus("x");
    expect(v.focused() == "x", "the focus starts on x");
    t->update_item("a", "A two", {}, false);
    pump();
    expect(t->selected() == "x", "re-inserting a parent keeps a child selected");
    expect(v.open("a"), "and the parent open");
    expect(v.focused() == "x", "and the focus on the child");

    // The focus on the row replaced.
    t->select("a");
    v.focus("a");
    t->update_item("a", "A three", {}, false);
    pump();
    expect(t->selected() == "a", "a re-inserted row stays selected");
    expect(v.focused() == "a", "and keeps the focus");
    expect(v.open("a"), "and stays open");

    // The chevron going: an empty answer.
    expect(v.chevron("n") && v.open("n"), "an unanswered node is open with a chevron");
    t->select("b");
    v.focus("n");
    t->set_children("n", {});
    pump();
    expect(!v.chevron("n"), "an empty answer takes the chevron");
    expect(!v.open("n"), "and closes it");
    expect(t->selected() == "b", "the selection elsewhere stays");
    expect(v.focused() == "n", "and the focus on the row replaced");

    // The chevron coming back.
    t->update_item("n", "n", {}, true);
    pump();
    expect(v.chevron("n"), "has_children brings it back");
    expect(t->selected() == "b", "the selection stays");
    expect(v.open("a"), "and what else was open");

    // An answer with children: the row isn't replaced.
    t->set_expanded("n", true);
    pump();
    t->select("n");
    v.focus("n");
    t->set_children("n", {item("p"), item("q", true, true)});
    pump();
    expect(v.open("n") && v.row("p") && v.open("q"), "an answer shows its children, open as asked");
    expect(t->selected() == "n" && v.focused() == "n", "selection and focus stay");

    win->close();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
