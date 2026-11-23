// A splitter's chain of GtkPaneds, shared by the GTK 3 and GTK 4 backends,
// and whether a press is on one of its handles: GTK 3's, on the handle's
// window; GTK 4's, for a touch drag (GTK 4's splitter takes its drags from
// the paneds' own gestures, which start the mouse's only from the handle,
// but touch's from a wider band).
#pragma once

#include <gtk/gtk.h>

namespace rap::gtk_split
{

// A splitter of n panes is a chain of n-1 nested GtkPaneds, each marked
// "rapport-split-chain" with its splitter's own id: a splitter that is
// itself a pane (the last one included) is a chain of its own, not more of
// this one (#42).
inline gpointer new_chain()
{
    static guint last = 0;
    return GUINT_TO_POINTER(++last);
}

inline void mark(GtkWidget* paned, gpointer chain)
{
    g_object_set_data(G_OBJECT(paned), "rapport-split-chain", chain);
}

// The paned after `paned` in its chain, if any.
inline GtkWidget* next(GtkWidget* paned)
{
#if GTK_MAJOR_VERSION >= 4
    GtkWidget* after = gtk_paned_get_end_child(GTK_PANED(paned));
#else
    GtkWidget* after = gtk_paned_get_child2(GTK_PANED(paned));
#endif
    gpointer chain = g_object_get_data(G_OBJECT(paned), "rapport-split-chain");
    return after && chain && g_object_get_data(G_OBJECT(after), "rapport-split-chain") == chain ? after
                                                                                                    : nullptr;
}

#if GTK_MAJOR_VERSION >= 4
// A paned's handle: its child that's neither pane.
inline GtkWidget* handle_of(GtkWidget* paned)
{
    GtkPaned* p = GTK_PANED(paned);
    for (GtkWidget* c = gtk_widget_get_first_child(paned); c; c = gtk_widget_get_next_sibling(c))
        if (c != gtk_paned_get_start_child(p) && c != gtk_paned_get_end_child(p)) return c;
    return nullptr;
}

// Whether (x, y), in the paned's coordinates, is where GTK starts a mouse
// drag of its divider: the handle, and a thin one's few pixels either side
// (GtkPaned's HANDLE_EXTRA_SIZE, 6). GTK starts a touch drag in a wider
// band; rapport counts a touch drag only from here, as a mouse's (#138).
inline bool on_handle(GtkWidget* paned, double x, double y)
{
    GtkWidget* handle = handle_of(paned);
    graphene_rect_t r;
    if (!handle || !gtk_widget_compute_bounds(handle, paned, &r)) return false;
    if (!gtk_paned_get_wide_handle(GTK_PANED(paned))) graphene_rect_inset(&r, -6, -6);
    const graphene_point_t at = GRAPHENE_POINT_INIT(float(x), float(y));
    return graphene_rect_contains_point(&r, &at);
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
