// A splitter's on_moved fires for a drag of its divider and nothing else:
// a press elsewhere in the panes, held while the window relays them out,
// isn't a drag (#30), and nor is a press on a divider that's let go without
// the pointer moving, the panes laid out again by set_sizes meanwhile
// (#105). A splitter that is a pane of another, the last pane included, is
// a splitter of its own: its divider, its sizes and its on_moved are its,
// not the outer one's.
//   rapport-splitter-moved-test-gtk3   (GDK_BACKEND=broadway, broadwayd running)
//   rapport-splitter-moved-test-gtk4   (GDK_BACKEND=broadway, gtk4-broadwayd running)
// GTK 3: real drags, a press, motions and a release put through GTK's event
// handling on a divider's handle window, which the paned drags itself; and
// presses straight to the paned, as from a pane. GTK 4 can't synthesize a
// press, so its drags are the paned's own drag gesture's (drag-update,
// drag-end), as the splitter watches them.
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#include "split_handle.h"

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
// A button press, then its release, as `paned` gets them, straight to it
// (not through GTK's event handling): on `window`.
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

// A press, a motion or a release on `window` at (x, y) in it, put through
// GTK's own event handling, as one from the pointer is.
void deliver(GdkWindow* window, GdkEventType type, double x, double y)
{
    GdkEvent* e = gdk_event_new(type);
    int ox = 0, oy = 0;
    gdk_window_get_origin(window, &ox, &oy);
    if (type == GDK_MOTION_NOTIFY)
    {
        e->motion.window = GDK_WINDOW(g_object_ref(window));
        e->motion.send_event = true;
        e->motion.x = x;
        e->motion.y = y;
        e->motion.x_root = ox + x;
        e->motion.y_root = oy + y;
        e->motion.state = GDK_BUTTON1_MASK;
    }
    else
    {
        e->button.window = GDK_WINDOW(g_object_ref(window));
        e->button.send_event = true;
        e->button.button = GDK_BUTTON_PRIMARY;
        e->button.x = x;
        e->button.y = y;
        e->button.x_root = ox + x;
        e->button.y_root = oy + y;
        if (type == GDK_BUTTON_RELEASE) e->button.state = GDK_BUTTON1_MASK;
    }
    gdk_event_set_device(
        e, gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default())));
    gtk_main_do_event(e);
    gdk_event_free(e);
}

// A drag of a divider by its handle window, `by` px along both axes (the
// paned takes its own), in steps; or, with `move_to`, a press held while
// the panes are laid out again by set_sizes and let go where it was.
void drag(GdkWindow* handle, float by, rap::splitter* split = nullptr,
          std::vector<float> move_to = {})
{
    deliver(handle, GDK_BUTTON_PRESS, 2, 2);
    pump(30);
    if (split)
    {
        split->set_sizes(move_to);
        pump(100);
    }
    else
        for (int step = 1; step <= 4; ++step)
        {
            deliver(handle, GDK_MOTION_NOTIFY, 2 + by * step / 4, 2 + by * step / 4);
            pump(30);
        }
    deliver(handle, GDK_BUTTON_RELEASE, 2, 2);
    pump(50);
}
#else
// A paned's drag gesture for the mouse (not touch's pan).
GObject* drag_gesture(GtkWidget* paned)
{
    GObject* found = nullptr;
    GListModel* controllers = gtk_widget_observe_controllers(paned);
    for (guint i = 0, n = g_list_model_get_n_items(controllers); i < n && !found; ++i)
    {
        auto* c = static_cast<GObject*>(g_list_model_get_item(controllers, i));
        if (GTK_IS_GESTURE_DRAG(c) && !GTK_IS_GESTURE_PAN(c)) found = c;
        g_object_unref(c);
    }
    g_object_unref(controllers);
    return found;
}

// The paned's drag, as its gesture reports one (drag-begin needs a real
// press, which GTK 4 can't make): moved by `by` in steps, then ended; or,
// with `move_to`, the panes laid out again by set_sizes and the drag ended
// without moving.
void drag(GtkWidget* paned, float by, rap::splitter* split = nullptr,
          std::vector<float> move_to = {})
{
    GObject* gesture = drag_gesture(paned);
    if (!gesture) return;
    if (split)
    {
        split->set_sizes(move_to);
        pump(100);
    }
    else
        for (int step = 1; step <= 4; ++step)
        {
            g_signal_emit_by_name(gesture, "drag-update", double(by * step / 4),
                                  double(by * step / 4));
            pump(30);
        }
    g_signal_emit_by_name(gesture, "drag-end", 0.0, 0.0);
    pump(50);
}
#endif

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv, {.backend = GTK_MAJOR_VERSION >= 4 ? "gtk4" : "gtk3"});
    if (!ui) return 2;
    using namespace rap::layout;
    rap::splitter *outer = nullptr, *inner = nullptr, *last = nullptr, *nested = nullptr;
    // Above: three panes (two paneds in its chain), the middle one a splitter
    // of its own. Below: two panes, the last one a splitter of its own, put
    // straight in (the end child of the outer paned, as more of a chain is).
    auto win = (*ui)->create(
        window{"Splitter moved",
               {vbox{
                   splitter{rap::orientation::horizontal, button{"left"}.stretch(),
                            splitter{rap::orientation::vertical, label{"top"}, label{"bottom"}}.bind(inner).stretch(),
                            label{"right"}}
                       .bind(outer)
                       .stretch(),
                   splitter{rap::orientation::horizontal, label{"first"}.stretch(),
                            splitter{rap::orientation::vertical, label{"upper"}, label{"lower"}}.bind(nested).stretch()}
                       .bind(last)
                       .stretch(),
               }}}
            .size(60_ch, 24_lines));
    win->show();
    pump(400);
    if (!outer || !inner || !last || !nested) return 2;
    int moved = 0, moved_last = 0, moved_nested = 0;
    outer->on_moved([&] { ++moved; });
    last->on_moved([&] { ++moved_last; });
    nested->on_moved([&] { ++moved_nested; });

    GtkWidget* top = toplevel();
    std::vector<GtkWidget*> all;
    if (top) paneds(top, all);
    // In the order a depth-first walk meets them: the upper splitter's two,
    // the one in its middle pane, the lower splitter's, the one in its last.
    expect(all.size() == 5, "five paneds: two for the upper splitter, one each for the other three");
    if (all.size() != 5) return 1;
    GtkWidget *first = all[0], *second = all[1], *other = all[2], *lower = all[3], *in_last = all[4];
    auto chain = [](GtkWidget* w) { return g_object_get_data(G_OBJECT(w), "rapport-split-chain"); };
    expect(chain(first) && chain(first) == chain(second), "the upper splitter's two paneds are one chain");
    expect(chain(other) && chain(other) != chain(first), "the splitter in its middle pane is a chain of its own");
    expect(chain(lower) && chain(in_last) && chain(lower) != chain(in_last),
           "the splitter in the lower one's last pane is a chain of its own");
    expect(rap::gtk_split::next(first) == second && !rap::gtk_split::next(second),
           "the upper chain goes on to its second paned and no further");
    expect(!rap::gtk_split::next(lower), "the lower chain doesn't go on into the splitter in its last pane");
    expect(outer->sizes().size() == 3, "the upper splitter has three sizes");
    expect(last->sizes().size() == 2, "the lower splitter has two sizes, not the nested one's too");
    expect(nested->sizes().size() == 2, "the splitter in its last pane has its own two");

    const std::vector<float> at = outer->sizes();
    auto nudged = [](const std::vector<float>& from, float by)
    {
        std::vector<float> s = from;
        if (!s.empty()) s[0] += by;
        return s;
    };

#if GTK_MAJOR_VERSION < 4
    auto handle = [](GtkWidget* paned) { return gtk_paned_get_handle_window(GTK_PANED(paned)); };
    click(first, gtk_widget_get_window(top), outer, nudged(at, 40));
    expect(moved == 0, "a press from a pane, held while the panes are laid out again: no on_moved");
    drag(handle(first), 30);
    expect(moved == 1, "the first divider dragged: on_moved once");
    drag(handle(second), 20);
    expect(moved == 2, "the second divider dragged: on_moved once more");
    drag(handle(first), 0, outer, {});
    expect(moved == 2, "a press on a divider let go, nothing moved: no on_moved");
    drag(handle(first), 0, outer, nudged(outer->sizes(), -30));
    expect(moved == 2,
           "a press on a divider, the panes laid out again by set_sizes while it's down, let go "
           "without moving: no on_moved");
    drag(handle(other), 15);
    expect(moved == 2,
           "the middle pane's splitter's divider dragged: not the outer one's on_moved");
    // A press on the last pane's splitter's divider is its paned's, which
    // the splitter it's the last pane of holds too.
    drag(handle(in_last), 15);
    expect(moved_nested == 1, "the last pane's splitter's divider dragged: its on_moved once");
    expect(moved_last == 0, "and not the on_moved of the splitter it's the last pane of");
#else
    expect(drag_gesture(first) && drag_gesture(second) && drag_gesture(in_last),
           "each paned has its drag gesture");
    drag(first, 30);
    expect(moved == 1, "the first divider dragged: on_moved once");
    drag(second, 20);
    expect(moved == 2, "the second divider dragged: on_moved once more");
    drag(first, 0, outer, nudged(outer->sizes(), -30));
    expect(moved == 2,
           "the panes laid out again by set_sizes while a divider's held, let go without moving: "
           "no on_moved");
    drag(other, 15);
    expect(moved == 2,
           "the middle pane's splitter's divider dragged: not the outer one's on_moved");
    drag(in_last, 15);
    expect(moved_nested == 1, "the last pane's splitter's divider dragged: its on_moved once");
    expect(moved_last == 0, "and not the on_moved of the splitter it's the last pane of");
    (void)at;
#endif

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
