// A window's document tabs driven through a real backend: the keys (with a
// text field focused, where they still work), a tab chosen in the native
// control, a middle click and a close button, each handler firing once,
// and the app's own calls firing none.
//   rapport-document-tabs-test-gtk3   (GDK_BACKEND=broadway, broadwayd running)
//   rapport-document-tabs-test-gtk4   (GDK_BACKEND=broadway, gtk4-broadwayd running)
//   rapport-document-tabs-test-qt6    (QT_QPA_PLATFORM=offscreen)
//   rapport-document-tabs-test-win32  (a window of its own: CI, or RAPPORT_NO_ACTIVATE=1)
// GTK 4 can't synthesize events: its window's key controller is given the
// keys, and the tab's middle-click gesture its release.
#include <rapport/rapport.h>

#if defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
#include <gtk/gtk.h>
#elif defined(RAPPORT_TEST_QT6)
#include <QAbstractButton>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTabBar>
#include <QTabWidget>
#elif defined(RAPPORT_TEST_WIN32)
#include <windows.h>
// After windows.h, which it needs.
#include <commctrl.h>
#endif

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#if defined(RAPPORT_TEST_WIN32) && defined(RAPPORT_TEST_BUILTIN_WIN32)
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
}

enum class k
{
    t,
    w,
    tab,
    one,
    nine,
};

// The native tab control: found, read back, and given input.
class driver
{
public:
    virtual ~driver() = default;
    virtual bool ready() = 0;
    virtual int count() = 0;
    virtual int current() = 0;
    // Ctrl and the key (and Shift), pressed and let go, to the window.
    virtual void key(k, bool shift) = 0;
    // As a click on tab i would: the control's own change of tab.
    virtual void choose(int i) = 0;
    virtual void middle_click(int i) = 0;
    // The tab's close button; false where tabs have none (Windows').
    virtual bool close_button(int i) = 0;
    // Windows' tabs only: a right click on tab i opens its menu (false
    // elsewhere); then, from the menu's own loop, item `item` (0 New Tab,
    // 1 Close Tab) chosen with the keys.
    virtual bool open_menu(int) { return false; }
    virtual void choose_in_menu(int) {}
    // Before focusing a control: the window active where the toolkit only
    // has a focus widget in the active window (Qt).
    virtual void activate() {}
};

#if defined(RAPPORT_TEST_GTK3)

class native : public driver
{
public:
    bool ready() override
    {
        if (!nb_)
            for (GList* l = gtk_window_list_toplevels(); l && !nb_; l = l->next)
                find(GTK_WIDGET(l->data));
        return nb_ && gtk_widget_get_realized(nb_) && gtk_widget_get_mapped(nb_);
    }
    int count() override { return gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb_)); }
    int current() override { return gtk_notebook_get_current_page(GTK_NOTEBOOK(nb_)); }
    void key(k key, bool shift) override
    {
        guint keyval = GDK_KEY_t;
        switch (key)
        {
        case k::t: keyval = GDK_KEY_t; break;
        case k::w: keyval = GDK_KEY_w; break;
        case k::tab: keyval = shift ? GDK_KEY_ISO_Left_Tab : GDK_KEY_Tab; break;
        case k::one: keyval = GDK_KEY_1; break;
        case k::nine: keyval = GDK_KEY_9; break;
        }
        GdkWindow* top = gtk_widget_get_window(gtk_widget_get_toplevel(nb_));
        for (GdkEventType type : {GDK_KEY_PRESS, GDK_KEY_RELEASE})
        {
            GdkEvent* e = gdk_event_new(type);
            e->key.window = GDK_WINDOW(g_object_ref(top));
            e->key.send_event = true;
            e->key.time = ++time_;
            e->key.keyval = keyval;
            e->key.state = GDK_CONTROL_MASK | (shift ? GDK_SHIFT_MASK : 0);
            GdkKeymapKey* keys = nullptr;
            gint n = 0;
            if (gdk_keymap_get_entries_for_keyval(
                    gdk_keymap_get_for_display(gdk_display_get_default()), keyval, &keys, &n) &&
                n > 0)
                e->key.hardware_keycode = guint16(keys[0].keycode);
            g_free(keys);
            e->key.string = g_strdup("");
            gdk_event_set_device(
                e, gdk_seat_get_keyboard(gdk_display_get_default_seat(gdk_display_get_default())));
            gtk_main_do_event(e);
            gdk_event_free(e);
        }
        spin();
    }
    void choose(int i) override
    {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(nb_), i);
        spin();
    }
    void middle_click(int i) override
    {
        GtkWidget* label = tab(i);
        GdkEvent* e = gdk_event_new(GDK_BUTTON_RELEASE);
        e->button.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(label)));
        e->button.button = 2;
        e->button.time = ++time_;
        gboolean handled = false;
        g_signal_emit_by_name(label, "button-release-event", e, &handled);
        gdk_event_free(e);
        spin();
    }
    bool close_button(int i) override
    {
        GtkWidget* box = gtk_bin_get_child(GTK_BIN(tab(i)));
        GList* children = gtk_container_get_children(GTK_CONTAINER(box));
        GtkWidget* close = GTK_WIDGET(g_list_last(children)->data);
        g_list_free(children);
        gtk_button_clicked(GTK_BUTTON(close));
        spin();
        return true;
    }

private:
    GtkWidget* tab(int i)
    {
        return gtk_notebook_get_tab_label(GTK_NOTEBOOK(nb_),
                                          gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb_), i));
    }
    void find(GtkWidget* w)
    {
        if (nb_) return;
        if (GTK_IS_NOTEBOOK(w))
        {
            nb_ = w;
            return;
        }
        if (GTK_IS_CONTAINER(w))
            gtk_container_forall(
                GTK_CONTAINER(w),
                +[](GtkWidget* c, gpointer p) { static_cast<native*>(p)->find(c); }, this);
    }
    static void spin()
    {
        while (gtk_events_pending()) gtk_main_iteration_do(false);
    }
    GtkWidget* nb_ = nullptr;
    guint32 time_ = 1000;
};
constexpr const char* backend_name = "gtk3";

#elif defined(RAPPORT_TEST_GTK4)

class native : public driver
{
public:
    bool ready() override
    {
        if (!nb_)
        {
            GListModel* tops = gtk_window_get_toplevels();
            for (guint i = 0; i < g_list_model_get_n_items(tops) && !nb_; ++i)
            {
                auto* w = GTK_WIDGET(g_list_model_get_item(tops, i));
                win_ = w;
                find(w);
                g_object_unref(w);
            }
        }
        return nb_ && gtk_widget_get_mapped(nb_);
    }
    int count() override { return gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb_)); }
    int current() override { return gtk_notebook_get_current_page(GTK_NOTEBOOK(nb_)); }
    void key(k key, bool shift) override
    {
        guint keyval = GDK_KEY_t;
        switch (key)
        {
        case k::t: keyval = GDK_KEY_t; break;
        case k::w: keyval = GDK_KEY_w; break;
        case k::tab: keyval = shift ? GDK_KEY_ISO_Left_Tab : GDK_KEY_Tab; break;
        case k::one: keyval = GDK_KEY_1; break;
        case k::nine: keyval = GDK_KEY_9; break;
        }
        const auto state = GdkModifierType(GDK_CONTROL_MASK | (shift ? GDK_SHIFT_MASK : 0));
        // The window's capture-phase key controllers, the last added first
        // (rapport's, after GTK's own): the first to take the key has it.
        GListModel* controllers = gtk_widget_observe_controllers(win_);
        const guint n = g_list_model_get_n_items(controllers);
        for (guint i = n; i-- > 0;)
        {
            auto* c = GTK_EVENT_CONTROLLER(g_list_model_get_item(controllers, i));
            gboolean handled = false;
            if (GTK_IS_EVENT_CONTROLLER_KEY(c) &&
                gtk_event_controller_get_propagation_phase(c) == GTK_PHASE_CAPTURE)
            {
                g_signal_emit_by_name(c, "key-pressed", keyval, 0u, state, &handled);
                g_signal_emit_by_name(c, "key-released", keyval, 0u, state);
            }
            g_object_unref(c);
            if (handled) break;
        }
        g_object_unref(controllers);
        spin();
    }
    void choose(int i) override
    {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(nb_), i);
        spin();
    }
    void middle_click(int i) override
    {
        GListModel* controllers = gtk_widget_observe_controllers(tab(i));
        for (guint j = 0; j < g_list_model_get_n_items(controllers); ++j)
        {
            auto* c = G_OBJECT(g_list_model_get_item(controllers, j));
            if (GTK_IS_GESTURE_CLICK(c) &&
                gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(c)) == GDK_BUTTON_MIDDLE)
                g_signal_emit_by_name(c, "released", 1, 4.0, 4.0);
            g_object_unref(c);
        }
        g_object_unref(controllers);
        spin();
    }
    bool close_button(int i) override
    {
        g_signal_emit_by_name(gtk_widget_get_last_child(tab(i)), "clicked");
        spin();
        return true;
    }

private:
    GtkWidget* tab(int i)
    {
        return gtk_notebook_get_tab_label(GTK_NOTEBOOK(nb_),
                                          gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb_), i));
    }
    void find(GtkWidget* w)
    {
        if (nb_) return;
        if (GTK_IS_NOTEBOOK(w))
        {
            nb_ = w;
            return;
        }
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            find(c);
    }
    static void spin()
    {
        while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, false);
    }
    GtkWidget* nb_ = nullptr;
    GtkWidget* win_ = nullptr;
};
constexpr const char* backend_name = "gtk4";

#elif defined(RAPPORT_TEST_QT6)

class native : public driver
{
public:
    bool ready() override
    {
        if (!bar_)
            for (QWidget* w : QApplication::topLevelWidgets())
                for (QTabBar* b : w->findChildren<QTabBar*>())
                    if (!qobject_cast<QTabWidget*>(b->parentWidget())) bar_ = b;  // not a page's
        return bar_ && bar_->isVisible();
    }
    int count() override { return bar_->count(); }
    int current() override { return bar_->currentIndex(); }
    void key(k key, bool shift) override
    {
        int code = Qt::Key_T;
        switch (key)
        {
        case k::t: code = Qt::Key_T; break;
        case k::w: code = Qt::Key_W; break;
        case k::tab: code = shift ? Qt::Key_Backtab : Qt::Key_Tab; break;
        case k::one: code = Qt::Key_1; break;
        case k::nine: code = Qt::Key_9; break;
        }
        Qt::KeyboardModifiers mods = Qt::ControlModifier;
        if (shift) mods |= Qt::ShiftModifier;
        QWidget* to = QApplication::focusWidget() ? QApplication::focusWidget() : bar_->window();
        QKeyEvent down(QEvent::KeyPress, code, mods);
        QApplication::sendEvent(to, &down);
        QKeyEvent up(QEvent::KeyRelease, code, mods);
        QApplication::sendEvent(to, &up);
        QApplication::processEvents();
    }
    void choose(int i) override
    {
        bar_->setCurrentIndex(i);
        QApplication::processEvents();
    }
    void middle_click(int i) override
    {
        const QPoint at = bar_->tabRect(i).center();
        QMouseEvent press(QEvent::MouseButtonPress, at, bar_->mapToGlobal(at), Qt::MiddleButton,
                          Qt::MiddleButton, Qt::NoModifier);
        QApplication::sendEvent(bar_, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, bar_->mapToGlobal(at), Qt::MiddleButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(bar_, &release);
        QApplication::processEvents();
    }
    bool close_button(int i) override
    {
        QWidget* b = bar_->tabButton(i, QTabBar::RightSide);
        if (!b) b = bar_->tabButton(i, QTabBar::LeftSide);
        if (auto* button = qobject_cast<QAbstractButton*>(b)) button->click();
        QApplication::processEvents();
        return true;
    }

private:
public:
    void activate() override
    {
        bar_->window()->activateWindow();
        QApplication::processEvents();
    }

private:
    QTabBar* bar_ = nullptr;
};
constexpr const char* backend_name = "qt6";

#elif defined(RAPPORT_TEST_WIN32)

class native : public driver
{
public:
    bool ready() override
    {
        if (!tab_) EnumThreadWindows(GetCurrentThreadId(), find, reinterpret_cast<LPARAM>(this));
        return tab_ && IsWindowVisible(tab_);
    }
    int count() override { return TabCtrl_GetItemCount(tab_); }
    int current() override { return TabCtrl_GetCurSel(tab_); }
    // Posted, for the backend's message loop to take as it takes typing:
    // with Ctrl (and Shift) held in the thread's key state.
    void key(k key, bool shift) override
    {
        WPARAM vk = 'T';
        switch (key)
        {
        case k::t: vk = 'T'; break;
        case k::w: vk = 'W'; break;
        case k::tab: vk = VK_TAB; break;
        case k::one: vk = '1'; break;
        case k::nine: vk = '9'; break;
        }
        BYTE state[256] = {};
        GetKeyboardState(state);
        state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
        state[VK_SHIFT] = state[VK_LSHIFT] = shift ? 0x80 : 0;
        SetKeyboardState(state);
        const LPARAM lp = 1 | (LPARAM(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC)) << 16);
        HWND to = GetFocus() ? GetFocus() : GetParent(tab_);
        PostMessageW(to, WM_KEYDOWN, vk, lp);
        PostMessageW(to, WM_KEYUP, vk, lp | (LPARAM(3) << 30));
        pending_keys_ = true;
    }
    // The keys are taken by the backend's loop, after this step.
    void settle()
    {
        if (!pending_keys_) return;
        pending_keys_ = false;
        BYTE state[256] = {};
        GetKeyboardState(state);
        state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_SHIFT] = state[VK_LSHIFT] = 0;
        SetKeyboardState(state);
    }
    void choose(int i) override { TabCtrl_SetCurFocus(tab_, i); }  // selects, and notifies
    void middle_click(int i) override
    {
        RECT r;
        TabCtrl_GetItemRect(tab_, i, &r);
        const LPARAM at = MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        SendMessageW(tab_, WM_MBUTTONDOWN, MK_MBUTTON, at);
        SendMessageW(tab_, WM_MBUTTONUP, 0, at);
    }
    bool close_button(int) override { return false; }
    // Posted, as a right click's WM_CONTEXTMENU is: the menu's loop runs in
    // the backend's, and the next step finds the menu there.
    bool open_menu(int i) override
    {
        RECT r;
        TabCtrl_GetItemRect(tab_, i, &r);
        POINT at{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        ClientToScreen(tab_, &at);
        PostMessageW(tab_, WM_CONTEXTMENU, WPARAM(tab_), MAKELPARAM(at.x, at.y));
        return true;
    }
    void choose_in_menu(int item) override
    {
        HWND menu = FindWindowW(L"#32768", nullptr);
        if (!menu) return;
        // Down to the item (the separator is passed over), then Enter.
        for (int n = 0; n <= item; ++n) PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
        PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 0);
    }
    // A menu left open (the keys missed it) is closed, so the test ends.
    static bool close_menu()
    {
        if (!FindWindowW(L"#32768", nullptr)) return false;
        EndMenu();
        return true;
    }

private:
    static BOOL CALLBACK find(HWND top, LPARAM p)
    {
        EnumChildWindows(
            top,
            [](HWND c, LPARAM p) -> BOOL
            {
                auto* self = reinterpret_cast<native*>(p);
                wchar_t name[32] = {};
                GetClassNameW(c, name, 32);
                // The document tabs' (fixed width), not a page's.
                if (!_wcsicmp(name, WC_TABCONTROLW) &&
                    (GetWindowLongPtrW(c, GWL_STYLE) & TCS_FIXEDWIDTH))
                    self->tab_ = c;
                return TRUE;
            },
            p);
        return TRUE;
    }
    HWND tab_ = nullptr;
    bool pending_keys_ = false;
};
constexpr const char* backend_name = "win32";

#endif

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options opts;
    opts.backend = backend_name;
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 2;
    }
    rap::document_tabs* tabs = nullptr;
    rap::entry* field = nullptr;
    rap::entry* inner_field = nullptr;
    rap::tabs* inner = nullptr;
    std::vector<int> inner_changed;
    std::vector<std::string> selected, closed;
    int added = 0, next_id = 0;
    using namespace rap::layout;
    auto win = (*ui)->create(
        window{"Document tabs",
               {vbox{entry{}.bind(field), label{"The current tab's content"},
                     // Page tabs in the content: Ctrl+Tab in them is theirs.
                     rap::layout::tabs{{"One", vbox{entry{}.bind(inner_field)}},
                                       {"Two", label{"Page two"}}}
                         .on_changed([&](int i) { inner_changed.push_back(i); })
                         .bind(inner)
                         .stretch()}}}
            .size(60_ch, 12_lines)
            .tabs(document_tabs{{{"a", "Alpha", rap::icon::for_file("alpha/"), "/home/me/alpha"},
                                 {"b", "Beta", rap::icon::named("folder"), "/home/me/beta"},
                                 {"c", "Gamma", {}, {}}}}
                      .current("b")
                      .on_select([&](const std::string& id) { selected.push_back(id); })
                      .on_close([&](const std::string& id) { closed.push_back(id); })
                      .on_new(
                          [&]
                          {
                              ++added;
                              // As an app does: a tab added and made current.
                              const std::string id = "n" + std::to_string(next_id++);
                              tabs->add({id, "New", {}, {}});
                              tabs->select(id);
                          })
                      .bind(tabs)));
    win->show();
    native d;

    // One step a tick, so each step's input has been through the backend's
    // own loop (win32 takes posted keys there) before the next looks.
    std::vector<std::function<void()>> steps;
    size_t step = 0;  // the next to run
    auto once = [&](const std::vector<std::string>& got, const std::vector<std::string>& want,
                    const std::string& what) { expect(got == want, what); };
    steps.push_back(
        [&]
        {
            expect(d.count() == 3 && d.current() == 1 && tabs && tabs->current() == "b",
                   "made with three tabs, the second current");
            d.activate();
            if (field) field->focus();  // the keys work while a field has the focus
            d.key(k::tab, false);
        });
    steps.push_back(
        [&]
        {
            once(selected, {"c"}, "Ctrl+Tab: on_select(c) once");
            expect(d.current() == 2, "and the control shows it");
            d.key(k::tab, false);
        });
    steps.push_back(
        [&]
        {
            once(selected, {"c", "a"}, "Ctrl+Tab from the last: round to a");
            d.key(k::tab, true);
        });
    steps.push_back(
        [&]
        {
            once(selected, {"c", "a", "c"}, "Ctrl+Shift+Tab from the first: round to c");
            d.key(k::one, false);
        });
    steps.push_back(
        [&]
        {
            once(selected, {"c", "a", "c", "a"}, "Ctrl+1: a");
            d.key(k::nine, false);
        });
    steps.push_back(
        [&]
        {
            once(selected, {"c", "a", "c", "a", "c"}, "Ctrl+9: the last");
            selected.clear();
            d.key(k::t, false);
        });
    steps.push_back(
        [&]
        {
            expect(added == 1, "Ctrl+T: on_new once");
            expect(d.count() == 4 && d.current() == 3 && selected.empty(),
                   "the app's add and select: shown, no on_select");
            d.key(k::w, false);
        });
    steps.push_back(
        [&]
        {
            once(closed, {"n0"}, "Ctrl+W: on_close(n0) once");
            expect(d.count() == 4, "and the tab stays until the app removes it");
            tabs->remove("n0");
            expect(d.count() == 3 && d.current() == 2 && tabs->current() == "c" && selected.empty(),
                   "the app removes the current one: the one before it is current, no on_select");
            d.choose(0);
        });
    steps.push_back(
        [&]
        {
            once(selected, {"a"}, "a tab chosen in the control: on_select(a) once");
            closed.clear();
            d.middle_click(1);
        });
    steps.push_back(
        [&]
        {
            once(closed, {"b"}, "a middle click: on_close(b) once");
            closed.clear();
            if (d.close_button(2)) once(closed, {"c"}, "the close button: on_close(c) once");
            tabs->update({"a", "Alpha 2", {}, "elsewhere"});
            tabs->select("c");
            expect(d.current() == 2 && selected.size() == 1,
                   "the app's update and select fire nothing");
            // In a page's tabs, Ctrl+Tab goes round their pages instead.
            if (inner_field) inner_field->focus();
            inner_changed.clear();  // GTK 3's page tabs tell of their first page as they show
            d.key(k::tab, false);
        });
    steps.push_back(
        [&]
        {
            expect(inner && inner->current() == 1 && inner_changed == std::vector<int>{1},
                   "Ctrl+Tab with the focus in a page's tabs: their next page, on_changed once "
                   "(page " +
                       std::to_string(inner ? inner->current() : -1) + ", " +
                       std::to_string(inner_changed.size()) + " changes)");
            expect(selected.size() == 1 && d.current() == 2, "and the document tabs don't move");
            // Ctrl+T there is still the document tabs'.
            d.key(k::t, false);
        });
    steps.push_back(
        [&]
        {
            expect(added == 2, "Ctrl+T with the focus in a page's tabs: on_new");
            closed.clear();
            added = 0;
            if (!d.open_menu(0)) step += 4;  // tabs with no menu (all but Windows')
        });
    steps.push_back([&] { d.choose_in_menu(1); });  // in the menu's loop
    steps.push_back(
        [&]
        {
#if defined(RAPPORT_TEST_WIN32)
            expect(!native::close_menu(), "the menu took its keys");
#endif
            once(closed, {"a"}, "the right-click menu's Close Tab: on_close(a) once");
            if (d.open_menu(0)) return;
            step += 1;
        });
    steps.push_back([&] { d.choose_in_menu(0); });
    steps.push_back(
        [&]
        {
#if defined(RAPPORT_TEST_WIN32)
            native::close_menu();
            expect(added == 1 && closed.size() == 1, "the right-click menu's New Tab: on_new once");
#endif
        });

    int waited = 0;
    rap::subscription tick = (*ui)->timer(
        30, true,
        [&]
        {
#if defined(RAPPORT_TEST_WIN32)
            d.settle();
#endif
            if (step == 0 && !d.ready())
            {
                if (++waited < 300) return;
                expect(false, std::string("the tabs are shown (") + backend_name + ")");
            }
            else if (step < steps.size())
            {
                steps[step++]();
                return;
            }
            std::printf(fails ? "%d failed\n" : "all passed\n", fails);
            std::fflush(stdout);
            (*ui)->quit(fails ? 1 : 0);
        });
    return (*ui)->run();
}
