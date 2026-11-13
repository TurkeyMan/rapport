// A splitter's chain of GtkPaneds, and whether a press is on one of its
// handles: shared by the GTK 3 and GTK 4 backends, and by the test that
// checks the GTK 4 hit test against real paneds (GTK 4 can't make a press).
#pragma once

#include <gtk/gtk.h>

namespace rap::gtk_split
{

// A splitter of n panes is a chain of n-1 nested GtkPaneds (each marked
// "rapport-split-chain"). The paned after `paned` in its chain, if any.
inline GtkWidget* next(GtkWidget* paned)
{
#if GTK_MAJOR_VERSION >= 4
    GtkWidget* after = gtk_paned_get_end_child(GTK_PANED(paned));
#else
    GtkWidget* after = gtk_paned_get_child2(GTK_PANED(paned));
#endif
    return after && g_object_get_data(G_OBJECT(after), "rapport-split-chain") ? after : nullptr;
}

inline bool in_chain(GtkWidget* first, GtkWidget* paned)
{
    for (GtkWidget* w = first; w; w = next(w))
        if (w == paned) return true;
    return false;
}

#if GTK_MAJOR_VERSION >= 4
// Whether `picked` (the widget under a press, gtk_widget_pick's answer) is
// one of the handles of the splitter whose chain starts at `first`: the
// child of one of its paneds that's neither pane. Its nearest paned in the
// chain decides, so a press in a pane, a nested pane's handle in another
// splitter's chain, or anything inside them is not.
inline bool handle_picked(GtkWidget* first, GtkWidget* picked)
{
    for (GtkWidget* w = picked; w; w = gtk_widget_get_parent(w))
    {
        GtkWidget* parent = gtk_widget_get_parent(w);
        if (!parent || !GTK_IS_PANED(parent) || !in_chain(first, parent)) continue;
        GtkPaned* paned = GTK_PANED(parent);
        return w != gtk_paned_get_start_child(paned) && w != gtk_paned_get_end_child(paned);
    }
    return false;
}

// Whether a press event lands on one of those handles: picked where it is,
// as the paned's own drag takes it (a handle's wider grab area counts).
inline bool press_on_handle(GtkWidget* first, GdkEvent* e)
{
    GtkNative* native = gtk_widget_get_native(first);
    double x = 0, y = 0, sx = 0, sy = 0;
    if (!native || !gdk_event_get_position(e, &x, &y)) return false;
    gtk_native_get_surface_transform(native, &sx, &sy);
    return handle_picked(first, gtk_widget_pick(GTK_WIDGET(native), x - sx, y - sy, GTK_PICK_DEFAULT));
}
#else
// GTK 3: a press on a handle comes on that paned's own handle window.
inline bool press_on_handle(GtkWidget* first, GdkEventButton* e)
{
    for (GtkWidget* w = first; w; w = next(w))
        if (e->window == gtk_paned_get_handle_window(GTK_PANED(w))) return true;
    return false;
}
#endif

}  // namespace rap::gtk_split
