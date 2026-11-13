// A window's document tabs driven through a real backend: the keys (with a
// text field focused, where they still work), a tab chosen in the native
// control, a middle click and a close button, each handler firing once,
// and the app's own calls firing none; a tab moved by Ctrl+Shift+PgDn and
// PgUp, and dragged (GTK: the notebook's own reorder, as its drag ends;
// Qt: the mouse's events; Windows: its messages), on_reorder once each; and
// a press whose tabs the app changes (an on_select adding one before it; a
// tab added or the pressed one removed, or removed and added again, or a
// key's move, between the press and its release): no on_reorder but the
// key's own.
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
#include <QKeySequence>
#include <QMouseEvent>
#include <QTabBar>
#include <QTabWidget>
#elif defined(RAPPORT_TEST_WIN32)
#include <windows.h>
// After windows.h, which it needs.
#include <commctrl.h>
#endif

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
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
    page_up,
    page_down,
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
    // elsewhere); then, from the menu's own loop, item `item` (0 New tab,
    // 1 Close tab) chosen with the keys.
    virtual bool open_menu(int) { return false; }
    virtual void choose_in_menu(int) {}
    // Before focusing a control: the window active where the toolkit only
    // has a focus widget in the active window (Qt).
    virtual void activate() {}
    // Tab i's title shows literally, with no access key in it.
    virtual bool literal_title(int, const std::string&) { return true; }
    // Tab `from` dragged to `to` and let go, as the user does.
    virtual void drag(int from, int to) = 0;
    // Tab i pressed (made current, as a press does), dragged a little way
    // and back, let go where it was pressed: it isn't moved.
    virtual void drag_back(int i) = 0;
    // Tab i pressed, and later let go where it was pressed (GTK: the
    // notebook's drag ending where the page is then), with no move between.
    virtual void press(int i) = 0;
    virtual void release() = 0;
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
        case k::page_up: keyval = GDK_KEY_Page_Up; break;
        case k::page_down: keyval = GDK_KEY_Page_Down; break;
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
    // As the notebook's own drag ends: the page moved, page-reordered.
    void drag(int from, int to) override
    {
        gtk_notebook_reorder_child(GTK_NOTEBOOK(nb_), gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb_), from), to);
        spin();
    }
    void press(int i) override
    {
        pressed_ = GTK_WIDGET(g_object_ref(gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb_), i)));
        gtk_notebook_set_current_page(GTK_NOTEBOOK(nb_), i);
        spin();
    }
    // The notebook's drag ends where the page is (none if it's gone).
    void release() override
    {
        if (!pressed_) return;
        const int at = gtk_notebook_page_num(GTK_NOTEBOOK(nb_), pressed_);
        if (at >= 0) g_signal_emit_by_name(nb_, "page-reordered", pressed_, guint(at));
        g_object_unref(pressed_);
        pressed_ = nullptr;
        spin();
    }
    GtkWidget* pressed_ = nullptr;
    // Pressed: current (on_select, which may change the tabs); its drag
    // ending where the page is now, as page-reordered says when the
    // notebook's scrolled with no move.
    void drag_back(int i) override
    {
        GtkWidget* page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb_), i);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(nb_), i);
        spin();
        g_signal_emit_by_name(nb_, "page-reordered", page, guint(gtk_notebook_page_num(GTK_NOTEBOOK(nb_), page)));
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
        case k::page_up: keyval = GDK_KEY_Page_Up; break;
        case k::page_down: keyval = GDK_KEY_Page_Down; break;
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
    // As the notebook's own drag ends: the page moved, page-reordered.
    void drag(int from, int to) override
    {
        gtk_notebook_reorder_child(GTK_NOTEBOOK(nb_), gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb_), from), to);
        spin();
    }
    void press(int i) override
    {
        pressed_ = GTK_WIDGET(g_object_ref(gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb_), i)));
        gtk_notebook_set_current_page(GTK_NOTEBOOK(nb_), i);
        spin();
    }
    // The notebook's drag ends where the page is (none if it's gone).
    void release() override
    {
        if (!pressed_) return;
        const int at = gtk_notebook_page_num(GTK_NOTEBOOK(nb_), pressed_);
        if (at >= 0) g_signal_emit_by_name(nb_, "page-reordered", pressed_, guint(at));
        g_object_unref(pressed_);
        pressed_ = nullptr;
        spin();
    }
    GtkWidget* pressed_ = nullptr;
    // Pressed: current (on_select, which may change the tabs); its drag
    // ending where the page is now, as page-reordered says when the
    // notebook's scrolled with no move.
    void drag_back(int i) override
    {
        GtkWidget* page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb_), i);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(nb_), i);
        spin();
        g_signal_emit_by_name(nb_, "page-reordered", page, guint(gtk_notebook_page_num(GTK_NOTEBOOK(nb_), page)));
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
        case k::page_up: code = Qt::Key_PageUp; break;
        case k::page_down: code = Qt::Key_PageDown; break;
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
    // Pressed on tab `from`, moved along in steps to tab `to`'s middle (the
    // bar moving it live as it passes each), let go there.
    void drag(int from, int to) override
    {
        const QPoint start = bar_->tabRect(from).center(), end = bar_->tabRect(to).center();
        auto send = [this](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons)
        {
            QMouseEvent e(type, at, bar_->mapToGlobal(at), button, buttons, Qt::NoModifier);
            QApplication::sendEvent(bar_, &e);
            QApplication::processEvents();
        };
        send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        const int steps = 12;
        for (int i = 1; i <= steps; ++i)
            send(QEvent::MouseMove, start + (end - start) * i / steps, Qt::NoButton, Qt::LeftButton);
        reordered_mid_drag = told ? told() : 0;
        send(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    }
    std::function<size_t()> told;  // how many on_reorder so far
    void press(int i) override
    {
        pressed_at_ = bar_->tabRect(i).center();
        QMouseEvent e(QEvent::MouseButtonPress, pressed_at_, bar_->mapToGlobal(pressed_at_), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bar_, &e);
        QApplication::processEvents();
    }
    void release() override
    {
        QMouseEvent e(QEvent::MouseButtonRelease, pressed_at_, bar_->mapToGlobal(pressed_at_), Qt::LeftButton,
                      Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(bar_, &e);
        QApplication::processEvents();
    }
    QPoint pressed_at_;
    void drag_back(int i) override
    {
        const QPoint start = bar_->tabRect(i).center();
        auto send = [this](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons)
        {
            QMouseEvent e(type, at, bar_->mapToGlobal(at), button, buttons, Qt::NoModifier);
            QApplication::sendEvent(bar_, &e);
            QApplication::processEvents();
        };
        send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        for (int dx : {10, 20, 30, 20, 10, 0})
            send(QEvent::MouseMove, start + QPoint(dx, 0), Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, start, Qt::LeftButton, Qt::NoButton);
    }
    size_t reordered_mid_drag = 0;
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
    // "R&D" shows as itself, with no Alt+D mnemonic.
    bool literal_title(int i, const std::string& title) override
    {
        const QString shown = bar_->tabText(i);
        return QKeySequence::mnemonic(shown).isEmpty() && QString(shown).replace("&&", "&") == QString::fromStdString(title);
    }
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

// Stage 1's tab control (before Windows 11, and under a menu bar), or
// Windows 11's strip in the caption, which says where its parts are and what
// it shows (its "rapport-caption-tabs" message) and is clicked by messages.
class native : public driver
{
public:
    bool ready() override
    {
        if (!tab_ && !strip_) EnumThreadWindows(GetCurrentThreadId(), find, reinterpret_cast<LPARAM>(this));
        return control() && IsWindowVisible(control());
    }
    int count() override { return strip_ ? ask(0) : TabCtrl_GetItemCount(tab_); }
    int current() override { return strip_ ? ask(1) : TabCtrl_GetCurSel(tab_); }
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
        case k::page_up: vk = VK_PRIOR; break;
        case k::page_down: vk = VK_NEXT; break;
        }
        post_key(vk, true, shift);
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
    void choose(int i) override
    {
        if (!strip_)
        {
            TabCtrl_SetCurFocus(tab_, i);  // selects, and notifies
            return;
        }
        const LPARAM at = centre(rect(3, i));
        SendMessageW(strip_, WM_LBUTTONDOWN, MK_LBUTTON, at);
        SendMessageW(strip_, WM_LBUTTONUP, 0, at);
    }
    void middle_click(int i) override
    {
        RECT r{};
        if (strip_)
            r = rect(3, i);
        else
            TabCtrl_GetItemRect(tab_, i, &r);
        const LPARAM at = centre(r);
        SendMessageW(control(), WM_MBUTTONDOWN, MK_MBUTTON, at);
        SendMessageW(control(), WM_MBUTTONUP, 0, at);
    }
    // Pressed, moved past the drag threshold and on to tab `to`'s middle,
    // let go there: sent to the strip, as the mouse's captured by it.
    void drag(int from, int to) override
    {
        RECT a{}, b{};
        if (strip_)
        {
            a = rect(3, from);
            b = rect(3, to);
        }
        else
        {
            TabCtrl_GetItemRect(tab_, from, &a);
            TabCtrl_GetItemRect(tab_, to, &b);
        }
        const LPARAM start = centre(a), end = centre(b);
        SendMessageW(control(), WM_LBUTTONDOWN, MK_LBUTTON, start);
        const int x = GET_X(start), y = GET_Y(start);
        SendMessageW(control(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + (GET_X(end) > x ? 20 : -20), y));
        SendMessageW(control(), WM_MOUSEMOVE, MK_LBUTTON, end);
        SendMessageW(control(), WM_LBUTTONUP, 0, end);
    }
    void drag_back(int i) override
    {
        RECT r{};
        if (strip_)
            r = rect(3, i);
        else
            TabCtrl_GetItemRect(tab_, i, &r);
        const LPARAM start = centre(r);
        const int x = GET_X(start), y = GET_Y(start);
        SendMessageW(control(), WM_LBUTTONDOWN, MK_LBUTTON, start);
        SendMessageW(control(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + 30, y));
        SendMessageW(control(), WM_MOUSEMOVE, MK_LBUTTON, start);
        SendMessageW(control(), WM_LBUTTONUP, 0, start);
    }
    void press(int i) override
    {
        RECT r{};
        if (strip_)
            r = rect(3, i);
        else
            TabCtrl_GetItemRect(tab_, i, &r);
        pressed_at_ = centre(r);
        SendMessageW(control(), WM_LBUTTONDOWN, MK_LBUTTON, pressed_at_);
    }
    void release() override { SendMessageW(control(), WM_LBUTTONUP, 0, pressed_at_); }
    LPARAM pressed_at_ = 0;
    static int GET_X(LPARAM p) { return short(LOWORD(p)); }
    static int GET_Y(LPARAM p) { return short(HIWORD(p)); }
    // Stage 1's tabs have none; the caption's have.
    bool close_button(int i) override
    {
        if (!strip_) return false;
        const LPARAM at = centre(rect(4, i));
        SendMessageW(strip_, WM_LBUTTONDOWN, MK_LBUTTON, at);
        SendMessageW(strip_, WM_LBUTTONUP, 0, at);
        return true;
    }
    // Posted, as a right click's WM_CONTEXTMENU is: the menu's loop runs in
    // the backend's, and the next step finds the menu there.
    bool open_menu(int i) override
    {
        RECT r{};
        if (strip_)
            r = rect(3, i);
        else
            TabCtrl_GetItemRect(tab_, i, &r);
        POINT at{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        ClientToScreen(control(), &at);
        PostMessageW(control(), WM_CONTEXTMENU, WPARAM(control()), MAKELPARAM(at.x, at.y));
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

    // The caption's strip, which takes the focus (F6) and its keys.
    bool caption() const { return strip_ != nullptr; }
    void plain_key(WPARAM vk) { post_key(vk, false, false); }
    // The focused tab (count(): "+"), -1 when the strip hasn't the focus.
    int strip_focus() { return strip_ ? ask(2) : -1; }
    bool strip_has_focus() const { return strip_ && GetFocus() == strip_; }

private:
    HWND control() const { return strip_ ? strip_ : tab_; }
    void post_key(WPARAM vk, bool ctrl, bool shift)
    {
        BYTE state[256] = {};
        GetKeyboardState(state);
        state[VK_CONTROL] = state[VK_LCONTROL] = ctrl ? 0x80 : 0;
        state[VK_SHIFT] = state[VK_LSHIFT] = shift ? 0x80 : 0;
        SetKeyboardState(state);
        const LPARAM lp = 1 | (LPARAM(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC)) << 16);
        HWND to = GetFocus() ? GetFocus() : GetParent(control());
        PostMessageW(to, WM_KEYDOWN, vk, lp);
        PostMessageW(to, WM_KEYUP, vk, lp | (LPARAM(3) << 30));
        pending_keys_ = true;
    }
    int ask(int what, int index = 0, RECT* r = nullptr)
    {
        static const UINT m = RegisterWindowMessageW(L"rapport-caption-tabs");
        return int(SendMessageW(strip_, m, MAKEWPARAM(what, index), reinterpret_cast<LPARAM>(r)));
    }
    RECT rect(int what, int index)
    {
        RECT r{};
        ask(what, index, &r);
        return r;
    }
    static LPARAM centre(const RECT& r) { return MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2); }
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
                if (!_wcsicmp(name, L"rapportCaptionTabs")) self->strip_ = c;
                return TRUE;
            },
            p);
        return TRUE;
    }
    HWND tab_ = nullptr, strip_ = nullptr;
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
    std::vector<std::pair<std::string, int>> reordered;
    int added = 0, next_id = 0;
    bool insert_on_select = false;  // the next on_select adds a tab before the one chosen
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
            // Wide enough for Windows 11's tabs in the caption, beside its
            // buttons, to show each of the three whole, close button and all.
            .size(90_ch, 12_lines)
            // --base: on a base surface (Windows 11 paints the strip anew there)
            .base_surface(argc > 1 && std::string_view(argv[1]) == "--base")
            .tabs(document_tabs{{{"a", "Alpha", rap::icon::for_file("alpha/"), "/home/me/alpha"},
                                 {"b", "Beta", rap::icon::named("folder"), "/home/me/beta"},
                                 {"c", "Gamma", {}, {}}}}
                      .current("b")
                      .on_select(
                          [&](const std::string& id)
                          {
                              selected.push_back(id);
                              if (!std::exchange(insert_on_select, false)) return;
                              // As an app might: a tab opened beside the one chosen, before it.
                              const std::vector<std::string> now = tabs->ids();
                              const auto at = std::find(now.begin(), now.end(), id);
                              tabs->insert(int(at - now.begin()), {"x", "Inserted", {}, {}});
                          })
                      .on_close([&](const std::string& id) { closed.push_back(id); })
                      .on_reorder([&](const std::string& id, int i) { reordered.emplace_back(id, i); })
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
#if defined(RAPPORT_TEST_QT6)
    d.told = [&] { return reordered.size(); };
#endif

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
            tabs->update({"a", "R&D", {}, "elsewhere"});
            tabs->select("c");
            expect(d.current() == 2 && selected.size() == 1,
                   "the app's update and select fire nothing");
            expect(d.literal_title(0, "R&D"), "an '&' in a title shows as itself, no access key");
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
            once(closed, {"a"}, "the right-click menu's Close tab: on_close(a) once");
            if (d.open_menu(0)) return;
            step += 1;
        });
    steps.push_back([&] { d.choose_in_menu(0); });
    steps.push_back(
        [&]
        {
#if defined(RAPPORT_TEST_WIN32)
            native::close_menu();
            expect(added == 1 && closed.size() == 1, "the right-click menu's New tab: on_new once");
#endif
        });
    // Reordering: the current tab moved right and back by the keys, then
    // dragged two places right; on_reorder once each, the bind's ids()
    // following.
    std::vector<std::string> before;
    auto moved = [](std::vector<std::string> v, int from, int to)
    {
        const std::string t = v[size_t(from)];
        v.erase(v.begin() + from);
        v.insert(v.begin() + to, t);
        return v;
    };
    steps.push_back(
        [&]
        {
            before = tabs->ids();
            expect(before.size() >= 3, "three tabs or more to reorder");
            tabs->select(before[0]);
            reordered.clear();
            selected.clear();
            if (field) field->focus();
            d.key(k::page_down, true);
        });
    steps.push_back(
        [&]
        {
            using r = std::vector<std::pair<std::string, int>>;
            expect(reordered == r{{before[0], 1}} && tabs->ids() == moved(before, 0, 1) && d.current() == 1 &&
                       tabs->current() == before[0],
                   "Ctrl+Shift+PgDn: the current tab one right, on_reorder once (" +
                       std::to_string(reordered.size()) + ")");
            d.key(k::page_up, true);
        });
    steps.push_back(
        [&]
        {
            expect(reordered.size() == 2 && reordered[1] == std::pair<std::string, int>{before[0], 0} &&
                       tabs->ids() == before && d.current() == 0,
                   "Ctrl+Shift+PgUp: back, on_reorder once");
            d.key(k::page_up, true);
        });
    steps.push_back(
        [&]
        {
            expect(reordered.size() == 2 && tabs->ids() == before, "Ctrl+Shift+PgUp at the start: nothing");
            reordered.clear();
            d.drag(0, 2);
        });
    steps.push_back(
        [&]
        {
            using r = std::vector<std::pair<std::string, int>>;
            expect(reordered == r{{before[0], 2}} && tabs->ids() == moved(before, 0, 2) && d.count() == int(before.size()),
                   "dragged two places right: on_reorder once, the bind in the new order (" +
                       std::to_string(reordered.size()) + " told" +
                       (reordered.empty() ? std::string()
                                          : ", " + reordered[0].first + " to " + std::to_string(reordered[0].second)) +
                       "; " + std::to_string(tabs->ids().size()) + " tabs, the moved one at " +
                       std::to_string(
                           [&]
                           {
                               const std::vector<std::string> now = tabs->ids();
                               return int(std::find(now.begin(), now.end(), before[0]) - now.begin());
                           }()) +
                       ", " + std::to_string(d.count()) + " shown)");
#if defined(RAPPORT_TEST_QT6)
            expect(d.reordered_mid_drag == 0, "and none of it before the drag was let go");
#endif
            expect(selected.empty() || selected == std::vector<std::string>{before[0]},
                   "no on_select but the press's, if it wasn't current");
            // A tab not current pressed, whose on_select adds a tab before it
            // (moving it one on), dragged a little and back: not reordered.
            reordered.clear();
            selected.clear();
            before = tabs->ids();
            tabs->select(before[0]);
            insert_on_select = true;
            d.drag_back(1);
        });
    steps.push_back(
        [&]
        {
            std::vector<std::string> want = before;
            want.insert(want.begin() + 1, "x");
            expect(selected == std::vector<std::string>{before[1]} && tabs->ids() == want,
                   "a press whose on_select adds a tab before it: the tab one on");
            expect(reordered.empty(), "and dragged back where it was, no on_reorder (" +
                                          std::to_string(reordered.size()) + ")");
            insert_on_select = false;
            tabs->remove("x");
        });
    // Between a press and its release, the app changes the tabs: the
    // press's drag is cancelled, and its release tells nothing.
    std::string pressed_id;
    auto press_second = [&]
    {
        tabs->select(tabs->ids()[0]);
        reordered.clear();
        pressed_id = tabs->ids()[1];
        d.press(1);
    };
    steps.push_back(
        [&]
        {
            press_second();
            before = tabs->ids();
            tabs->insert(0, {"y", "Inserted", {}, {}});
            d.release();
            expect(reordered.empty(), "a tab added before the pressed one, then let go: no on_reorder (" +
                                          std::to_string(reordered.size()) + ")");
            std::vector<std::string> want = before;
            want.insert(want.begin(), "y");
            expect(tabs->ids() == want, "and the tabs as the app made them");
            tabs->remove("y");
        });
    steps.push_back(
        [&]
        {
            press_second();
            tabs->remove(pressed_id);
            d.release();
            expect(reordered.empty(), "the pressed tab removed, then let go: no on_reorder (" +
                                          std::to_string(reordered.size()) + ")");
            tabs->insert(1, {pressed_id, "Back", {}, {}});
        });
    steps.push_back(
        [&]
        {
            press_second();
            before = tabs->ids();
            tabs->remove(pressed_id);
            tabs->insert(2, {pressed_id, "Again", {}, {}});
            d.release();
            expect(reordered.empty(), "the pressed tab removed and added again elsewhere, then let go: no "
                                      "on_reorder (" +
                                          std::to_string(reordered.size()) + ")");
            expect(tabs->ids() == moved(before, 1, 2), "and the tabs as the app made them");
        });
    steps.push_back(
        [&]
        {
            press_second();
            tabs->select(pressed_id);  // the key moves the current tab: the pressed one
            before = tabs->ids();
            d.key(k::page_down, true);
        });
    steps.push_back(
        [&]
        {
            using r = std::vector<std::pair<std::string, int>>;
            expect(reordered == r{{pressed_id, 2}} && tabs->ids() == moved(before, 1, 2),
                   "Ctrl+Shift+PgDn while pressed: the key's on_reorder, once (" +
                       std::to_string(reordered.size()) + ")");
            d.release();
            expect(reordered.size() == 1, "and let go: nothing more (" + std::to_string(reordered.size()) + ")");
        });
#if defined(RAPPORT_TEST_WIN32)
    // Windows 11's tabs in the caption take the focus, from F6: the arrows
    // move it along them, Enter makes the focused one current, Delete
    // closes it, each handler once; F6 again, the focus back.
    std::vector<std::string> at_f6;
    steps.push_back(
        [&]
        {
            if (!d.caption())
            {
                step = steps.size();  // stage 1's take no focus
                return;
            }
            at_f6 = tabs->ids();
            tabs->select(at_f6[0]);
            if (field) field->focus();
            d.plain_key(VK_F6);
        });
    steps.push_back(
        [&]
        {
            expect(d.strip_has_focus() && d.strip_focus() == 0, "F6: the focus on the tabs, the current one (" +
                                                                    std::to_string(d.strip_focus()) + ")");
            selected.clear();
            closed.clear();
            d.plain_key(VK_RIGHT);
        });
    steps.push_back(
        [&]
        {
            expect(d.strip_focus() == 1 && d.current() == 0 && selected.empty(),
                   "Right: the focus on the next tab, the current one kept");
            d.plain_key(VK_RETURN);
        });
    steps.push_back(
        [&]
        {
            once(selected, {at_f6[1]}, "Enter: on_select once, for the focused tab");
            expect(d.current() == 1, "and it's current");
            d.plain_key(VK_DELETE);
        });
    steps.push_back(
        [&]
        {
            once(closed, {at_f6[1]}, "Delete: on_close once, for the focused tab");
            d.plain_key(VK_F6);
        });
    steps.push_back(
        [&]
        {
            expect(!d.strip_has_focus() && field && field->has_focus(), "F6 again: the focus back in the field");
        });
#endif

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
