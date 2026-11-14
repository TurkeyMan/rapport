// A window's document tabs torn off and dropped into other windows, through
// the real GTK and Qt backends, off the desktop.
//
// GTK 3 and 4 tear off and drop through GtkNotebook's own drag and drop,
// which can't be driven here (GTK 4 can't synthesize input, and neither
// drives a drag on Broadway), so its two ends are given as GTK gives them:
// the notebook's create-window signal (a tab let go outside any notebook),
// and a page moved from one notebook to another as GTK's drop does
// (gtk_notebook_detach_tab, then inserted: page-added). Checked: the tabs
// detachable, in their group; on_detach once with the id (and on GTK 3 the
// place), a null return changing nothing, and the app's window shown; a
// page dropped on another window's notebook put back and the target's
// on_receive asked once with the id, the index and the source window, true
// moving it through the binds and false changing nothing; another group's
// notebook not asked; dropped back on its own notebook, a move along the
// row (on_reorder once); the notebooks' pages as the bookkeeping says, after
// each. GTK 3: the right-click menu (from popup-menu, the menu keys' signal)
// with the app's items after the fixed ones.
//
// Qt: dragging doesn't tear off or drop yet (a tracked gap); the right-click
// menu does: New tab, Close tab, Move to new window (on_detach with no
// point), then the app's items.
//   rapport-document-tabs-tearoff-test-gtk3  (offscreen.sh gtk3)
//   rapport-document-tabs-tearoff-test-gtk4  (offscreen.sh gtk4)
//   rapport-document-tabs-tearoff-test-qt6   (offscreen.sh qt6)
#include <rapport/i18n.h>
#include <rapport/rapport.h>

#if defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
#include <gtk/gtk.h>
#elif defined(RAPPORT_TEST_QT6)
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QTabBar>
#include <QWidget>
#endif

#include <cstdio>
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
    std::fflush(stdout);
    if (!ok) ++fails;
}

std::string join(const std::vector<std::string>& v)
{
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : ",") + s;
    return out;
}

void pump()
{
#if defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
    for (int i = 0; i < 20; ++i)
        while (g_main_context_iteration(nullptr, FALSE)) {}
#elif defined(RAPPORT_TEST_QT6)
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
#endif
}

struct told_receive
{
    std::string id;
    int index = -1;
    rap::window* source = nullptr;
};

struct fixture;
std::vector<std::unique_ptr<fixture>> fixtures;
rap::ui* g_ui = nullptr;

struct fixture
{
    std::string name;
    std::unique_ptr<rap::window> win;
    rap::document_tabs* tabs = nullptr;
    std::vector<std::pair<std::string, std::optional<rap::point_f>>> detached;
    std::vector<told_receive> received;
    std::vector<std::pair<std::string, int>> reordered;
    bool refuse_detach = false, accept = true;
    // As an app may, there and then: on_detach closing this window once the
    // tab's out of it; on_receive closing it once it's taken one.
    bool close_on_detach = false, close_on_receive = false;
    fixture* made = nullptr;
};

fixture* make(const std::string& name, std::vector<std::string> ids, const std::string& group);

rap::layout::document_tabs tabs_spec(fixture& f, const std::vector<std::string>& ids, const std::string& group)
{
    std::vector<rap::document_tab> items;
    for (const std::string& id : ids) items.push_back({id, "Tab " + id, {}, {}});
    fixture* self = &f;
    return rap::layout::document_tabs{items}
        .current(ids.empty() ? std::string() : ids[0])
        .group(group)
        .on_reorder([self](const std::string& id, int i) { self->reordered.push_back({id, i}); })
        .on_detach(
            [self, group](const std::string& id, std::optional<rap::point_f> at) -> rap::window*
            {
                self->detached.push_back({id, at});
                if (self->refuse_detach) return nullptr;
                fixture* made = make(self->name + "+" + id, {id}, group);
                self->tabs->remove(id);
                self->made = made;
                rap::window* w = made->win.get();
                if (self->close_on_detach)
                {
                    self->tabs = nullptr;
                    self->win.reset();
                }
                return w;
            })
        .on_receive(
            [self](const rap::document_tabs::transfer& t)
            {
                self->received.push_back({t.id, t.index, t.source});
                if (!self->accept) return false;
                for (auto& o : fixtures)
                    if (o->win && o->win.get() == t.source)
                    {
                        o->tabs->remove(t.id);
                        self->tabs->insert(t.index, {t.id, t.title, t.icon, t.tooltip});
                        if (self->close_on_receive)
                        {
                            self->tabs = nullptr;
                            self->win.reset();
                        }
                        return true;
                    }
                return false;
            })
        .on_context_menu([](const std::string& id) { return rap::menu{{"_Rename " + id, [] {}}}; })
        .bind(f.tabs);
}

fixture* make(const std::string& name, std::vector<std::string> ids, const std::string& group)
{
    fixtures.push_back(std::make_unique<fixture>());
    fixture& f = *fixtures.back();
    f.name = name;
    using namespace rap::layout;
    window w{name, {label{"Content"}}};
    w.size(600_px, 200_px);
    w.tabs(tabs_spec(f, ids, group));
    f.win = g_ui->create(std::move(w));
    return &f;
}

#if defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)

GtkWidget* find_notebook(GtkWidget* w)
{
    if (GTK_IS_NOTEBOOK(w)) return w;
#if defined(RAPPORT_TEST_GTK3)
    if (!GTK_IS_CONTAINER(w)) return nullptr;
    GList* children = gtk_container_get_children(GTK_CONTAINER(w));
    GtkWidget* found = nullptr;
    for (GList* c = children; c && !found; c = c->next) found = find_notebook(GTK_WIDGET(c->data));
    g_list_free(children);
    return found;
#else
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* found = find_notebook(c)) return found;
    return nullptr;
#endif
}

GtkWidget* window_titled(const std::string& title)
{
#if defined(RAPPORT_TEST_GTK3)
    GList* all = gtk_window_list_toplevels();
    GtkWidget* found = nullptr;
    for (GList* w = all; w && !found; w = w->next)
    {
        const char* t = gtk_window_get_title(GTK_WINDOW(w->data));
        if (t && title == t) found = GTK_WIDGET(w->data);
    }
    g_list_free(all);
    return found;
#else
    GListModel* all = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(all); ++i)
    {
        auto* w = GTK_WINDOW(g_list_model_get_item(all, i));
        const char* t = gtk_window_get_title(w);
        g_object_unref(w);
        if (t && title == t) return GTK_WIDGET(w);
    }
    return nullptr;
#endif
}

GtkNotebook* notebook_of(fixture& f)
{
    GtkWidget* w = window_titled(f.name);
    return w ? GTK_NOTEBOOK(find_notebook(w)) : nullptr;
}

// The notebook's pages' ids, in order.
std::vector<std::string> pages(GtkNotebook* nb)
{
    std::vector<std::string> out;
    for (int i = 0; nb && i < gtk_notebook_get_n_pages(nb); ++i)
    {
        const char* id = static_cast<const char*>(
            g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(nb, i)), "rapport-tab"));
        out.push_back(id ? id : "?");
    }
    return out;
}

GtkWidget* page_for(GtkNotebook* nb, const std::string& id)
{
    for (int i = 0; i < gtk_notebook_get_n_pages(nb); ++i)
    {
        GtkWidget* p = gtk_notebook_get_nth_page(nb, i);
        const char* pid = static_cast<const char*>(g_object_get_data(G_OBJECT(p), "rapport-tab"));
        if (pid && id == pid) return p;
    }
    return nullptr;
}

// GTK's drop of a dragged tab on a notebook: the page taken from its own
// (detach_tab, as a drag does) and put in the other's at `index`, its label
// with it.
void gtk_drop(GtkNotebook* from, GtkNotebook* to, const std::string& id, int index)
{
    GtkWidget* page = page_for(from, id);
    if (!page) return;
    GtkWidget* label = gtk_notebook_get_tab_label(from, page);
    g_object_ref(page);
    g_object_ref(label);
    gtk_notebook_detach_tab(from, page);
    gtk_notebook_insert_page(to, page, label, index);
    g_object_unref(label);
    g_object_unref(page);
}

GtkNotebook* create_window(GtkNotebook* nb, const std::string& id, int x, int y)
{
    GtkNotebook* ret = nullptr;
#if defined(RAPPORT_TEST_GTK3)
    g_signal_emit_by_name(nb, "create-window", page_for(nb, id), x, y, &ret);
#else
    (void)x;
    (void)y;
    g_signal_emit_by_name(nb, "create-window", page_for(nb, id), &ret);
#endif
    return ret;
}

void run()
{
    using ids = std::vector<std::string>;
    fixture& a = *make("Tear A", {"a", "b", "c"}, "g");
    fixture& b = *make("Tear B", {"x", "y"}, "g");
    fixture& c = *make("Tear C", {"p", "q"}, "other");
    for (fixture* f : {&a, &b, &c}) f->win->show();
    pump();
    GtkNotebook *na = notebook_of(a), *nb = notebook_of(b), *nc = notebook_of(c);
    expect(na && nb && nc, "the windows' notebooks");
    if (!na || !nb || !nc) return;
    expect(gtk_notebook_get_tab_detachable(na, page_for(na, "a")) &&
               std::string(gtk_notebook_get_group_name(na) ? gtk_notebook_get_group_name(na) : "") == "g",
           "detachable tabs, in the group's name");

    // Refused: on_detach once, nothing changes.
    a.refuse_detach = true;
    GtkNotebook* got = create_window(na, "b", 300, 400);
    pump();
    expect(a.detached.size() == 1 && a.detached[0].first == "b", "create-window: on_detach(b) once");
#if defined(RAPPORT_TEST_GTK3)
    expect(!a.detached.empty() && a.detached[0].second && int(a.detached[0].second->x) == 300 &&
               int(a.detached[0].second->y) == 400,
           "with the drop's place");
#else
    expect(!a.detached.empty() && !a.detached[0].second, "GTK 4: no place to give");
#endif
    expect(!got && a.tabs->ids() == ids{"a", "b", "c"} && pages(na) == ids{"a", "b", "c"},
           "refused: no notebook returned, nothing changed");

    // Made: the app's window, shown; the tab out of A once GTK's done.
    a.refuse_detach = false;
    got = create_window(na, "b", 300, 400);
    expect(!got, "made: still no notebook for GTK (the app's moved it)");
    expect(a.tabs->ids() == ids{"a", "c"}, "the bookkeeping at once: " + join(a.tabs->ids()));
    pump();
    expect(pages(na) == ids{"a", "c"}, "and A's notebook once the drag's done: " + join(pages(na)));
    if (!a.made) return expect(false, "the app's window made");
    fixture& t = *a.made;
    GtkWidget* tw = window_titled(t.name);
    expect(tw && gtk_widget_get_visible(tw) && t.tabs->ids() == ids{"b"}, "the app's window shown, with b");
    // Exactly once anywhere: in the new window, not in A, and GTK's moved no
    // page of its own into it (create-window gave it no notebook).
    int pages_b = 0;
    for (auto& f : fixtures)
        if (GtkNotebook* n = notebook_of(*f))
            for (const std::string& id : pages(n)) pages_b += id == "b";
    expect(pages_b == 1 && pages(notebook_of(t)) == ids{"b"},
           "b in exactly one notebook, the new window's (" + std::to_string(pages_b) + ")");

    // Dropped on B: put back, and B asked.
    GtkNotebook* nt = notebook_of(t);
    gtk_drop(nt, nb, "b", 1);
    pump();
    expect(b.received.size() == 1 && b.received[0].id == "b" && b.received[0].index == 1 &&
               b.received[0].source == t.win.get(),
           "dropped on B at 1: on_receive(b, 1, the torn-off window) once");
    expect(b.tabs->ids() == ids{"x", "b", "y"} && pages(nb) == ids{"x", "b", "y"} && t.tabs->ids().empty() &&
               pages(nt).empty(),
           "taken: B's bookkeeping and notebook have it second, the torn-off window's empty: " + join(pages(nb)));

    // Refused by B: both as they were.
    b.received.clear();
    b.accept = false;
    gtk_drop(na, nb, "c", 0);
    pump();
    expect(b.received.size() == 1 && b.received[0].id == "c" && b.received[0].index == 0 &&
               b.received[0].source == a.win.get(),
           "dropped on B from A: on_receive(c, 0, A)");
    expect(a.tabs->ids() == ids{"a", "c"} && pages(na) == ids{"a", "c"} && b.tabs->ids() == ids{"x", "b", "y"} &&
               pages(nb) == ids{"x", "b", "y"},
           "refused: both notebooks as they were (A's page back): " + join(pages(na)) + " / " + join(pages(nb)));
    b.accept = true;

    // Another group: never asked.
    gtk_drop(na, nc, "c", 0);
    pump();
    expect(c.received.empty() && pages(na) == ids{"a", "c"} && pages(nc) == ids{"p", "q"},
           "dropped on C, another group: not asked, put back");

    // Dropped back on its own notebook elsewhere: a move along the row.
    gtk_drop(na, na, "a", 1);
    pump();
    expect(a.reordered.size() == 1 && a.reordered[0].first == "a" && a.reordered[0].second == 1 &&
               a.tabs->ids() == ids{"c", "a"} && pages(na) == ids{"c", "a"},
           "dropped back on its own row: on_reorder(a, 1) once: " + join(pages(na)));
    expect(gtk_notebook_get_current_page(na) == 1 && a.tabs->current() == "a",
           "and the tab current before is the notebook's current one still");

#if defined(RAPPORT_TEST_GTK3)
    // The menu keys' signal: the current tab's menu.
    gboolean handled = false;
    g_signal_emit_by_name(na, "popup-menu", &handled);
    pump();
    // Attached under the current tab (its label), as the keys' menu is.
    GList* menus = nullptr;
    for (int i = 0; i < gtk_notebook_get_n_pages(na) && !menus; ++i)
        if (GtkWidget* label = gtk_notebook_get_tab_label(na, gtk_notebook_get_nth_page(na, i)))
            menus = gtk_menu_get_for_attach_widget(label);
    std::vector<std::string> items;
    GtkWidget* move = nullptr;
    if (menus)
    {
        GList* children = gtk_container_get_children(GTK_CONTAINER(menus->data));
        for (GList* i = children; i; i = i->next)
        {
            const char* l = GTK_IS_SEPARATOR_MENU_ITEM(i->data) ? "" : gtk_menu_item_get_label(GTK_MENU_ITEM(i->data));
            items.push_back(l ? l : "");
            if (items.size() == 4) move = GTK_WIDGET(i->data);
        }
        g_list_free(children);
        gtk_menu_popdown(GTK_MENU(menus->data));
    }
    const std::string current = a.tabs->current();
    expect(handled && items.size() == 6 && items[0] == rap::i18n::tr("rapport", "_New tab") && items[1].empty() &&
               items[2] == rap::i18n::tr("rapport", "_Close tab") &&
               items[3] == rap::i18n::tr("rapport", "_Move to new window") && items[4].empty() &&
               items[5] == "_Rename " + current,
           "popup-menu: New tab, Close tab, Move to new window, then the app's (" + join(items) + ")");
    // Move to new window: on_detach with no point, and the app's window,
    // returned unshown, shown.
    a.detached.clear();
    a.made = nullptr;
    if (move) gtk_menu_item_activate(GTK_MENU_ITEM(move));
    pump();
    expect(a.detached.size() == 1 && a.detached[0].first == current && !a.detached[0].second && a.made,
           "Move to new window: on_detach(" + current + ") with no point");
    GtkWidget* mw = a.made ? window_titled(a.made->name) : nullptr;
    expect(mw && gtk_widget_get_visible(mw) && a.made->tabs->ids() == ids{current},
           "and the app's window shown, with " + current);
#endif

    // The app closing a window from its handler, there and then: nothing of
    // it touched after. on_detach closing the source once the tab's out
    // (GTK's drag holds the notebook, as here).
    fixture& f = *make("Tear F", {"f1", "f2"}, "g");
    f.close_on_detach = true;
    f.win->show();
    pump();
    GtkNotebook* nf = notebook_of(f);
    if (!nf) return expect(false, "F's notebook");
    g_object_ref(nf);
    create_window(nf, "f1", 200, 200);
    g_object_unref(nf);
    pump();
    expect(!f.win && f.made && f.made->tabs->ids() == ids{"f1"} && window_titled(f.made->name) &&
               gtk_widget_get_visible(window_titled(f.made->name)),
           "on_detach closing its own window: closed, the app's window shown all the same");
    // on_receive closing the target once it's taken the tab.
    fixture& g = *make("Tear G", {"g1"}, "g");
    g.close_on_receive = true;
    g.win->show();
    pump();
    GtkNotebook* ng = notebook_of(g);
    if (!ng) return expect(false, "G's notebook");
    const std::vector<std::string> b_before = b.tabs->ids();
    gtk_drop(nb, ng, b_before.front(), 0);
    pump();
    expect(g.received.size() == 1 && g.received[0].id == b_before.front() && !g.win &&
               b.tabs->ids() == std::vector<std::string>(b_before.begin() + 1, b_before.end()) &&
               pages(nb) == b.tabs->ids(),
           "on_receive taking it and closing its own window: closed, B without it: " + join(pages(nb)));
}

#elif defined(RAPPORT_TEST_QT6)

void run()
{
    fixture& a = *make("Tear A", {"a", "b"}, "g");
    a.win->show();
    pump();
    QTabBar* bar = nullptr;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->windowTitle() == QString::fromStdString(a.name)) bar = w->findChild<QTabBar*>();
    expect(bar != nullptr, "the window's tab bar");
    if (!bar) return;
    const QPoint at = bar->tabRect(1).center();
    QContextMenuEvent e(QContextMenuEvent::Mouse, at, bar->mapToGlobal(at));
    QApplication::sendEvent(bar, &e);
    pump();
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    std::vector<std::string> items;
    QAction* move = nullptr;
    if (menu)
        for (QAction* act : menu->actions())
        {
            items.push_back(act->isSeparator() ? std::string() : act->text().toStdString());
            if (items.size() == 4) move = act;
        }
    expect(items.size() == 6 && items[1].empty() && items[4].empty() && items[5] == "&Rename b",
           "the menu: New tab, Close tab, Move to new window, then the app's (" + join(items) + ")");
    if (move) move->trigger();
    if (menu) menu->close();
    pump();
    expect(a.detached.size() == 1 && a.detached[0].first == "b" && !a.detached[0].second,
           "Move to new window: on_detach(b) with no point");
    expect(a.tabs->ids() == std::vector<std::string>{"a"} && a.made && a.made->tabs->ids() == std::vector<std::string>{"b"},
           "and the app moved it to its window");
    bool made_shown = false;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (a.made && w->windowTitle() == QString::fromStdString(a.made->name)) made_shown = w->isVisible();
    expect(made_shown, "and its window shown, as the handler returned it unshown");
}

#endif

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::printf("FAIL %s\n", ui.error().c_str());
        return 1;
    }
    g_ui = ui->get();
    std::printf("backend %s\n", std::string((*ui)->backend_name()).c_str());
    run();
    pump();
    fixtures.clear();
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
