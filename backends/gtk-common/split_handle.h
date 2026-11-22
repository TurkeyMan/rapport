// A splitter's chain of GtkPaneds, shared by the GTK 3 and GTK 4 backends,
// and (GTK 3) whether a press is on one of its handles. GTK 4's splitter
// takes its drags from the paneds' own drag gestures, which GTK lets go on
// only from the paned's handle.
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

#if GTK_MAJOR_VERSION < 4
// GTK 3: a press on a handle comes on that paned's own handle window.
inline bool press_on_handle(GtkWidget* first, GdkEventButton* e)
{
    for (GtkWidget* w = first; w; w = next(w))
        if (e->window == gtk_paned_get_handle_window(GTK_PANED(w))) return true;
    return false;
}
#endif

}  // namespace rap::gtk_split
