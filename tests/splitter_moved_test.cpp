// A splitter's on_moved fires for a drag of its divider and nothing else
// (#30): a press elsewhere in the panes, held while the window relays them
// out, isn't a drag.
//   rapport-splitter-moved-test-gtk3   (GDK_BACKEND=broadway, broadwayd running)
//   rapport-splitter-moved-test-gtk4   (GDK_BACKEND=broadway, gtk4-broadwayd running)
// GTK 3: presses go to the paned as it gets them, on its handle's window or
// bubbled up from a pane, with the sizes changed between press and release.
// GTK 4 can't synthesize a press, so its test is the hit test the backend
// makes of one (split_handle.h), asked of what GTK picks at the handle's
// middle, in a pane, and on another splitter's handle inside a pane.
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#if GTK_MAJOR_VERSION >= 4
#include "split_handle.h"
#endif

#include <chrono>
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

void pump(int ms = 150)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do
        while (g_main_context_iteration(nullptr, false)) {}
    while (std::chrono::steady_clock::now() < until);
}

GtkWidget* toplevel()
{
#if GTK_MAJOR_VERSION >= 4
    GListModel* tops = gtk_window_get_toplevels();
    return g_list_model_get_n_items(tops) ? GTK_WIDGET(g_list_model_get_item(tops, 0)) : nullptr;
#else
    GList* tops = gtk_window_list_toplevels();
    GtkWidget* w = tops ? GTK_WIDGET(tops->data) : nullptr;
    g_list_free(tops);
    return w;
#endif
}

// The paneds, outermost first, in the order a depth-first walk meets them.
void paneds(GtkWidget* w, std::vector<GtkWidget*>& out)
{
    if (GTK_IS_PANED(w)) out.push_back(w);
#if GTK_MAJOR_VERSION >= 4
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) paneds(c, out);
#else
    if (GTK_IS_CONTAINER(w))
    {
        GList* kids = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList* k = kids; k; k = k->next) paneds(GTK_WIDGET(k->data), out);
        g_list_free(kids);
    }
#endif
}

#if GTK_MAJOR_VERSION < 4
// A button press, then its release, as `paned` gets them: on `window`.
void click(GtkWidget* paned, GdkWindow* window, rap::splitter* split, std::vector<float> move_to)
{
    for (GdkEventType type : {GDK_BUTTON_PRESS, GDK_BUTTON_RELEASE})
    {
        GdkEvent* e = gdk_event_new(type);
        e->button.window = GDK_WINDOW(g_object_ref(window));
        e->button.send_event = true;
        e->button.button = GDK_BUTTON_PRIMARY;
        e->button.x = 2;
        e->button.y = 2;
        gdk_event_set_device(e, gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default())));
        gboolean handled = FALSE;
        g_signal_emit_by_name(paned, type == GDK_BUTTON_PRESS ? "button-press-event" : "button-release-event",
                              e, &handled);
        gdk_event_free(e);
        // Laid out again while the button's down: the sizes change.
        if (type == GDK_BUTTON_PRESS && !move_to.empty())
        {
            split->set_sizes(move_to);
            pump(100);
        }
    }
    pump(50);
}
#else
// What GTK picks at the middle of `w`, in its window's coordinates.
GtkWidget* pick_middle(GtkWidget* w)
{
    GtkNative* native = gtk_widget_get_native(w);
    graphene_rect_t r;
    if (!native || !gtk_widget_compute_bounds(w, GTK_WIDGET(native), &r)) return nullptr;
    return gtk_widget_pick(GTK_WIDGET(native), r.origin.x + r.size.width / 2, r.origin.y + r.size.height / 2,
                           GTK_PICK_DEFAULT);
}

// A paned's handle: its child that's neither pane.
GtkWidget* handle_of(GtkWidget* paned)
{
    GtkPaned* p = GTK_PANED(paned);
    for (GtkWidget* c = gtk_widget_get_first_child(paned); c; c = gtk_widget_get_next_sibling(c))
        if (c != gtk_paned_get_start_child(p) && c != gtk_paned_get_end_child(p)) return c;
    return nullptr;
}
#endif

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv, {.backend = GTK_MAJOR_VERSION >= 4 ? "gtk4" : "gtk3"});
    if (!ui) return 2;
    using namespace rap::layout;
    rap::splitter *outer = nullptr, *inner = nullptr;
    // Three panes (two paneds in the chain); the middle one holds a splitter
    // of its own, whose handle isn't the outer splitter's.
    auto win = (*ui)->create(window{"Splitter moved",
                                    {splitter{rap::orientation::horizontal, button{"left"}.stretch(),
                                              splitter{rap::orientation::vertical, label{"top"}, label{"bottom"}}
                                                  .bind(inner)
                                                  .stretch(),
                                              label{"right"}}
                                         .bind(outer)}}
                                 .size(60_ch, 20_lines));
    win->show();
    pump(400);
    if (!outer || !inner) return 2;
    int moved = 0;
    outer->on_moved([&] { ++moved; });

    GtkWidget* top = toplevel();
    std::vector<GtkWidget*> all;
    if (top) paneds(top, all);
    // The outer chain is the first paned and the next marked one; the inner
    // splitter is the remaining paned.
    expect(all.size() == 3, "three paneds: two for the outer splitter's three panes, one for the inner");
    if (all.size() != 3) return 1;
    GtkWidget *first = all[0], *second = nullptr, *other = nullptr;
    for (size_t i = 1; i < all.size(); ++i)
        (g_object_get_data(G_OBJECT(all[i]), "rapport-split-chain") && !second ? second : other) = all[i];
    expect(second && other, "the outer chain's second paned and the inner splitter's are told apart");
    if (!second || !other) return 1;
    const std::vector<float> at = outer->sizes();
    auto nudged = [&](float by)
    {
        std::vector<float> s = at;
        s[0] += by;
        return s;
    };

#if GTK_MAJOR_VERSION < 4
    GdkWindow* elsewhere = gtk_widget_get_window(top);
    click(first, elsewhere, outer, nudged(40));
    expect(moved == 0, "a press from a pane, held while the panes are laid out again: no on_moved");
    click(first, gtk_paned_get_handle_window(GTK_PANED(first)), outer, nudged(-30));
    expect(moved == 1, "a press on the first divider, the panes moved: on_moved once");
    click(second, gtk_paned_get_handle_window(GTK_PANED(second)), outer, nudged(20));
    expect(moved == 2, "a press on the second divider, the panes moved: on_moved once more");
    click(first, gtk_paned_get_handle_window(GTK_PANED(first)), outer, {});
    expect(moved == 2, "a press on a divider, nothing moved: no on_moved");
    click(other, gtk_paned_get_handle_window(GTK_PANED(other)), outer, nudged(10));
    expect(moved == 2, "a press on the inner splitter's divider: not the outer one's on_moved");
#else
    expect(rap::gtk_split::handle_picked(first, pick_middle(handle_of(first))),
           "the first divider's middle is one of its handles");
    expect(rap::gtk_split::handle_picked(first, pick_middle(handle_of(second))),
           "the second divider's middle is one of its handles");
    expect(!rap::gtk_split::handle_picked(first, pick_middle(gtk_paned_get_start_child(GTK_PANED(first)))),
           "the middle of the left pane (a button) isn't");
    expect(!rap::gtk_split::handle_picked(first, pick_middle(gtk_paned_get_end_child(GTK_PANED(second)))),
           "the middle of the right pane isn't");
    expect(!rap::gtk_split::handle_picked(first, pick_middle(handle_of(other))),
           "the inner splitter's divider isn't the outer splitter's");
    expect(rap::gtk_split::handle_picked(other, pick_middle(handle_of(other))),
           "but it is the inner splitter's own");
    expect(!rap::gtk_split::handle_picked(first, nullptr), "nothing picked isn't");
    (void)moved;
    (void)nudged;
#endif

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
