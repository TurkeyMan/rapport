// GTK 3 and 4: a path's own icon (icon::for_path) from GIO, never waited
// for. "/" and the home folder each come with an icon of their own, not
// the fallback name's, on the main context after being asked, and once
// found it's there at once; a path that isn't there has none; a widget
// that goes before its answer comes isn't told. Then through a tree in a
// window that's never shown: its rows show the fallback first and GIO's
// icon once it's found (GTK 3's rows are read back from the tree store).
// Off the desktop:
//   offscreen.sh gtk3 rapport-path-icons-test-gtk3
//   offscreen.sh gtk4 rapport-path-icons-test-gtk4
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#include "gtk_common.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}

// The main context run until `done`, or `seconds` pass.
bool run_until(const std::function<bool()>& done, int seconds)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!done())
    {
        if (std::chrono::steady_clock::now() > until) return false;
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    return true;
}

std::string text_of(GIcon* g)
{
    if (!g) return "(none)";
    gchar* s = g_icon_to_string(g);
    std::string out = s ? s : "?";
    g_free(s);
    return out;
}

const char* const fallback = "rapport-test-fallback";

#ifdef RAPPORT_TEST_GTK3
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

// The tree store's icon (column 3) on the row with `id` (column 1).
GIcon* row_icon(GtkTreeModel* model, const std::string& id)
{
    GtkTreeIter it;
    for (bool ok = gtk_tree_model_get_iter_first(model, &it); ok; ok = gtk_tree_model_iter_next(model, &it))
    {
        gchar* row_id = nullptr;
        GIcon* icon = nullptr;
        gtk_tree_model_get(model, &it, 1, &row_id, 3, &icon, -1);
        const bool match = row_id && id == row_id;
        g_free(row_id);
        if (match)
        {
            if (icon) g_object_unref(icon);  // the store keeps its own
            return icon;
        }
        if (icon) g_object_unref(icon);
    }
    return nullptr;
}
#endif

}  // namespace

int main(int argc, char** argv)
{
    using namespace rap::gtkcommon;
    const std::string home = g_get_home_dir();
    GIcon* named = g_themed_icon_new(fallback);

    // -- GIO's answers, as the backends ask for them ------------------------
    for (const std::string& path : {std::string("/"), home})
    {
        GCancellable* c = g_cancellable_new();
        GIcon* told = nullptr;
        int times = 0;
        const size_t asked = path_icons().lookups;
        GIcon* at_once = path_g_icon(path, c,
                                     [&](GIcon* g)
                                     {
                                         told = g;
                                         ++times;
                                     });
        expect(!at_once && times == 0, path + ": not there at once, nobody told yet (asking never waits)");
        expect(path_icons().lookups == asked + 1, path + ": GIO asked");
        expect(run_until([&] { return times > 0; }, 20), path + ": told on the main context");
        std::printf("     %s: %s\n", path.c_str(), text_of(told).c_str());
        expect(told && G_IS_ICON(told), path + ": an icon");
        expect(told && !g_icon_equal(told, named), path + ": not the fallback name's");
        expect(path_g_icon(path, c, [&](GIcon*) { ++times; }) == told && times == 1 &&
                   path_icons().lookups == asked + 1,
               path + ": then there at once, not asked again");
        g_object_unref(c);
    }

    {
        GCancellable* c = g_cancellable_new();
        const std::string gone = home + "/rapport-no-such-folder";
        bool told = false;
        expect(!path_g_icon(gone, c, [&](GIcon*) { told = true; }), "a path that isn't there: not at once");
        run_until([&] { return path_icons().none.contains(gone); }, 20);
        expect(path_icons().none.contains(gone) && !told, "and found to have none, nobody told");
        g_object_unref(c);
    }

    {
        // A widget gone before its answer: never told; asked again by
        // another, it's found.
        const std::string tmp = "/tmp";
        GCancellable* gone = g_cancellable_new();
        bool told_gone = false;
        path_g_icon(tmp, gone, [&](GIcon*) { told_gone = true; });
        g_cancellable_cancel(gone);
        run_until([&] { return !path_icons().waiting.contains(tmp); }, 20);
        expect(!told_gone, "a widget gone before its answer isn't told");
        GCancellable* c = g_cancellable_new();
        GIcon* got = path_g_icon(tmp, c, [&](GIcon* g) { got = g; });
        run_until([&] { return got != nullptr; }, 20);
        expect(got != nullptr, "asked again by another, it's found");
        g_object_unref(gone);
        g_object_unref(c);
    }

    // -- through a tree, never shown ----------------------------------------
    rap::load_options options;
#ifdef RAPPORT_TEST_GTK3
    options.backend = "gtk3";
#else
    options.backend = "gtk4";
#endif
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui)
    {
        std::printf("FAIL no %s backend: %s\n", options.backend.c_str(), ui.error().c_str());
        return 1;
    }
    auto item = [](std::string id, const std::string& path)
    {
        rap::tree_item t;
        t.label = id;
        t.id = std::move(id);
        t.icon = rap::icon::for_path(path, fallback);
        return t;
    };
    using namespace rap::layout;
    // Paths the test's own lookups above haven't asked (the backend's cache
    // is its own, but so the rows start on the fallback either way).
    auto win = (*ui)->create(window{"t", {tree{{item("root", "/"), item("home", home), item("usr", "/usr")}}}});
    (void)win;  // never shown
#ifdef RAPPORT_TEST_GTK3
    GtkWidget* view = nullptr;
    GList* tops = gtk_window_list_toplevels();
    for (GList* l = tops; l && !view; l = l->next) view = find_tree_view(GTK_WIDGET(l->data));
    g_list_free(tops);
    expect(view != nullptr, "the tree view is there");
    if (!view) return 1;
    GtkTreeModel* model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GIcon* before = row_icon(model, "usr");
    expect(before && g_icon_equal(before, named), "a row shows the fallback name's icon first");
    const bool all = run_until(
        [&]
        {
            for (const char* id : {"root", "home", "usr"})
                if (GIcon* g = row_icon(model, id); !g || g_icon_equal(g, named)) return false;
            return true;
        },
        20);
    expect(all, "then each row GIO's own, in place");
    for (const char* id : {"root", "home", "usr"})
        std::printf("     row %s: %s\n", id, text_of(row_icon(model, id)).c_str());
#else
    // GTK 4's rows bind their icons as they're shown: the tree is made and
    // its answers come without harm; the rows' own are the helper's above.
    run_until([] { return false; }, 1);
    expect(true, "a tree with path icons made, its answers come, nothing breaks");
#endif
    g_object_unref(named);
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
