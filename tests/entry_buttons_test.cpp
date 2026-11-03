// An entry's buttons through the GTK 4 backend, off the desktop (Broadway):
// each is a focusable, named button that doesn't take the focus when
// clicked; Tab reaches the text first, then the buttons, and Shift+Tab goes
// back the same way; Space and Enter (the button's activate) act and drop
// a menu, and the focus comes back when it closes; a held-down button is
// checked to the theme and pressed to a screen reader.
//   rapport-entry-buttons-test-gtk4   (GDK_BACKEND=broadway, broadwayd running)
// ENTRY_TEST_SHOTS=dir also saves the window as it opens, with a button
// focused, and with one held down and focused, as PNGs there.
#include <rapport/rapport.h>

#include <gtk/gtk.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
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

void pump(int ms = 0)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do
    {
        while (g_main_context_iteration(nullptr, false))
        {
        }
        if (ms) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < until);
}

GtkWidget* find_type(GtkWidget* w, const char* type)
{
    if (std::string(G_OBJECT_TYPE_NAME(w)) == type) return w;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* f = find_type(c, type)) return f;
    return nullptr;
}

// The entries' GtkEntry widgets in order (the plain ones before and after).
void find_entries(GtkWidget* w, std::vector<GtkWidget*>& out)
{
    if (GTK_IS_ENTRY(w)) out.push_back(w);
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        find_entries(c, out);
}

bool save_once(GtkWidget* win, const std::string& path)
{
    bool ok = false;
    GdkPaintable* paintable = gtk_widget_paintable_new(win);
    GtkSnapshot* snap = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snap, gtk_widget_get_width(win), gtk_widget_get_height(win));
    if (GskRenderNode* node = gtk_snapshot_free_to_node(snap))
    {
        GdkTexture* tex =
            gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(win)), node, nullptr);
        ok = gdk_texture_save_to_png(tex, path.c_str());
        g_object_unref(tex);
        gsk_render_node_unref(node);
    }
    g_object_unref(paintable);
    return ok;
}

// As RAPPORT_SNAPSHOT does: tried again while GTK hasn't drawn it yet.
bool save(GtkWidget* win, const char* name)
{
    const char* dir = std::getenv("ENTRY_TEST_SHOTS");
    if (!dir) return true;
    for (int i = 0; i < 100; ++i)
    {
        if (save_once(win, std::string(dir) + "/" + name)) return true;
        pump(50);
    }
    return false;
}

std::string describe(GtkWidget* w)
{
    if (!w) return "nothing";
    const char* tip = gtk_widget_get_tooltip_text(w);
    return std::string(G_OBJECT_TYPE_NAME(w)) + (tip ? std::string(" \"") + tip + "\"" : "");
}

struct state
{
    int searched = 0, cased = 0, picked = 0;
    rap::entry_action* match_case = nullptr;
};

void run(GtkWidget* win, state& st)
{
    // No transitions: the snapshots show the state at once, and Broadway
    // draws few frames.
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", false, nullptr);
    GtkWidget* box = find_type(win, "RapportEntryBox");
    std::vector<GtkWidget*> plain;
    find_entries(win, plain);
    expect(box && plain.size() == 2, "the composite entry and two plain GtkEntries");
    if (!box || plain.size() != 2) return;
    // Once GTK has drawn it: unfocused, as it opens.
    expect(save(win, "entry-start.png"), "saved the entry as it opens");

    // The children: options (a menu), search, text, clear, match case.
    GtkWidget* text = nullptr;
    std::vector<GtkWidget*> buttons;
    for (GtkWidget* c = gtk_widget_get_first_child(box); c; c = gtk_widget_get_next_sibling(c))
        if (GTK_IS_TEXT(c))
            text = c;
        else if (!GTK_IS_NATIVE(c))
            buttons.push_back(c);
    expect(text && buttons.size() == 4, "a text and four buttons (two leading, clear, trailing)");
    if (!text || buttons.size() != 4) return;
    const char* names[] = {"Search options", "Search", "Clear", "Match case"};
    for (size_t i = 0; i < buttons.size(); ++i)
    {
        GtkWidget* b = buttons[i];
        const std::string n = names[i];
        expect(GTK_IS_BUTTON(b) && gtk_widget_get_focusable(b) && !gtk_widget_get_focus_on_click(b),
               n + ": a focusable GtkButton, not focused by a click");
        expect(gtk_test_accessible_has_role(GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_ROLE_BUTTON),
               n + ": the button role");
        char* err = gtk_test_accessible_check_property(GTK_ACCESSIBLE(b),
                                                       GTK_ACCESSIBLE_PROPERTY_LABEL, names[i]);
        expect(!err, n + ": named by its tooltip" + (err ? std::string(" (") + err + ")" : ""));
        g_free(err);
    }
    GtkWidget* options = buttons[0];
    GtkWidget* search = buttons[1];
    GtkWidget* clear = buttons[2];
    GtkWidget* match = buttons[3];

    // Tab: first entry, the text, the buttons, the last entry; Shift+Tab back.
    auto focus = [&] { return gtk_root_get_focus(GTK_ROOT(win)); };
    auto in = [](GtkWidget* f, GtkWidget* w)
    { return f && (f == w || gtk_widget_is_ancestor(f, w)); };
    auto tab = [&](GtkDirectionType dir)
    {
        g_signal_emit_by_name(win, "move-focus", dir);
        pump();
        return focus();
    };
    gtk_widget_grab_focus(plain[0]);
    pump();
    const std::vector<GtkWidget*> forward = {text, options, search, clear, match, plain[1]};
    std::string seen;
    bool ok = in(focus(), plain[0]);
    for (GtkWidget* want : forward)
    {
        GtkWidget* f = tab(GTK_DIR_TAB_FORWARD);
        seen += " > " + describe(f);
        ok = ok && in(f, want);
    }
    expect(ok, "Tab: first entry, text, options, search, clear, match case, last entry:" + seen);
    seen.clear();
    ok = true;
    for (auto it = forward.rbegin() + 1; it != forward.rend(); ++it)
    {
        GtkWidget* f = tab(GTK_DIR_TAB_BACKWARD);
        seen += " < " + describe(f);
        ok = ok && in(f, *it);
    }
    GtkWidget* f = tab(GTK_DIR_TAB_BACKWARD);
    seen += " < " + describe(f);
    expect(ok && in(f, plain[0]), "Shift+Tab: the same, back to the first entry:" + seen);
    gtk_widget_grab_focus(box);
    pump();
    expect(focus() == text, "focus given to the entry goes to its text");

    // A focused button: the theme's ring (focus-visible, as after a key).
    tab(GTK_DIR_TAB_FORWARD);
    tab(GTK_DIR_TAB_FORWARD);
    gtk_window_set_focus_visible(GTK_WINDOW(win), true);
    pump(100);
    expect(focus() == search && (gtk_widget_get_state_flags(search) & GTK_STATE_FLAG_FOCUS_VISIBLE),
           "Tab to the search button: focused, its ring visible");
    expect(save(win, "entry-focus-search.png"), "saved the focused search button");

    // Space and Enter are the button's activate (it clicks after showing
    // itself pressed for a moment).
    gtk_widget_activate(search);
    pump(400);
    expect(st.searched == 1 && focus() == search, "activating the search button acts, keeps focus");
    tab(GTK_DIR_TAB_FORWARD);
    tab(GTK_DIR_TAB_FORWARD);
    gtk_widget_activate(match);
    pump(400);
    expect(st.cased == 1, "activating match case acts");
    if (st.match_case) st.match_case->set_checked(true);
    pump(500);
    expect(gtk_widget_get_state_flags(match) & GTK_STATE_FLAG_CHECKED,
           "held down: the theme's checked state");
    char* err = gtk_test_accessible_check_state(GTK_ACCESSIBLE(match), GTK_ACCESSIBLE_STATE_PRESSED,
                                                GTK_ACCESSIBLE_TRISTATE_TRUE);
    expect(!err, std::string("held down: pressed to a screen reader") +
                     (err ? std::string(" (") + err + ")" : ""));
    g_free(err);
    // GTK hides the ring a few seconds after the last key: as if one were
    // pressed again.
    gtk_window_set_focus_visible(GTK_WINDOW(win), true);
    pump(100);
    expect(save(win, "entry-focus-checked.png"), "saved the held-down match case, focused");
    gtk_widget_activate(clear);
    pump(400);
    expect(focus() == text && !gtk_widget_get_visible(clear) &&
               !*gtk_editable_get_text(GTK_EDITABLE(text)),
           "the clear button empties the text, hides, and the text has the focus");

    // A menu from the keyboard: it drops; an item chosen, or Escape (the
    // popover's popdown), and the focus is back on the button.
    auto popover = [&]() -> GtkWidget*
    {
        for (GtkWidget* c = gtk_widget_get_first_child(box); c; c = gtk_widget_get_next_sibling(c))
            if (GTK_IS_POPOVER(c)) return c;
        return nullptr;
    };
    auto open = [&]
    {
        for (int i = 0; i < 100; ++i)
        {
            pump(10);
            if (popover() && gtk_widget_get_mapped(popover())) return true;
        }
        return false;
    };
    gtk_widget_grab_focus(options);
    pump();
    gtk_widget_activate(options);
    expect(open(), "activating the options button drops its menu");
    gtk_widget_activate_action(box, "ea.i0", nullptr);
    gtk_popover_popdown(GTK_POPOVER(popover()));
    pump(50);
    expect(st.picked == 1, "an item chosen runs it");
    expect(focus() == options,
           "after an item, the focus is on the options button: " + describe(focus()));
    gtk_widget_activate(options);
    expect(open(), "dropped again");
    gtk_popover_popdown(GTK_POPOVER(popover()));
    pump(50);
    expect(focus() == options,
           "after Escape, the focus is on the options button: " + describe(focus()));

    // Clicked (no focus taken), the menu gives the focus back to the text.
    gtk_widget_grab_focus(text);
    pump();
    g_signal_emit_by_name(options, "clicked");
    expect(open(), "clicked, the options button drops its menu");
    gtk_popover_popdown(GTK_POPOVER(popover()));
    pump(50);
    expect(focus() == text, "closed, the focus is on the text: " + describe(focus()));
}

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options opts;
    opts.backend = "gtk4";
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 2;
    }
    state st;
    using namespace rap::layout;
    auto win = (*ui)->create(window{
        "Entry buttons",
        {vbox{
            entry{"before"},
            entry{"text"}
                .leading_menu({"open-menu-symbolic"}, "Search options", [&]
                              { return rap::menu{{"_Regular expression", [&] { ++st.picked; }}}; })
                .leading_action({"system-search-symbolic"}, "Search", [&] { ++st.searched; })
                .clearable()
                .trailing_action(
                    {"format-text-bold-symbolic"}, "Match case", [&] { ++st.cased; },
                    st.match_case),
            entry{"after"},
        }}}.size(40_ch, 8_lines));
    win->show();
    (*ui)->post(
        [&]
        {
            GtkWidget* gw = nullptr;
            bool ok = false;
            for (int i = 0; i < 500 && !ok; ++i)
            {
                pump(10);
                GListModel* tops = gtk_window_get_toplevels();
                for (guint t = 0; t < g_list_model_get_n_items(tops) && !gw; ++t)
                {
                    auto* w = GTK_WIDGET(g_list_model_get_item(tops, t));
                    if (GTK_IS_WINDOW(w) && find_type(w, "RapportEntryBox")) gw = w;
                    g_object_unref(w);
                }
                ok = gw && gtk_widget_get_mapped(gw);
            }
            expect(ok, "the window is shown (gtk4)");
            if (ok) run(gw, st);
            std::printf(fails ? "%d failed\n" : "all passed\n", fails);
            std::fflush(stdout);
            (*ui)->quit(fails ? 1 : 0);
        });
    return (*ui)->run();
}
