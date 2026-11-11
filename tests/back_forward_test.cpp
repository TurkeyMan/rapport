// The mouse's side buttons and the keyboard's Back and Forward keys, as the
// window's key::back and key::forward, through a real backend: pressed over
// a list, an entry being typed in, a canvas that takes them (no key), one
// that doesn't (the key) and one that takes back but not forward, with both
// buttons held at once and let go in either order; presses only, each once.
//   rapport-back-forward-test-win32   (messages sent to its controls)
//   rapport-back-forward-test-gtk3    (GDK_BACKEND=broadway, broadwayd running)
//   rapport-back-forward-test-qt6     (QT_QPA_PLATFORM=offscreen)
// GTK 4 has no way to synthesize events (see list_input_test.cpp).
#include <rapport/rapport.h>

#if defined(RAPPORT_TEST_WIN32)
#include <windows.h>
#include <commctrl.h>
#elif defined(RAPPORT_TEST_GTK3)
#include <gtk/gtk.h>
#elif defined(RAPPORT_TEST_QT6)
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTreeView>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
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

class rows : public rap::list_model
{
public:
    size_t row_count() const override { return 10; }
    std::string text(size_t row, size_t) const override { return "Row " + std::to_string(row); }
};

enum class target
{
    list,
    taking,   // a canvas that takes the side buttons
    leaving,  // a canvas that doesn't
    mixed,    // a canvas that takes back and not forward
    entry,
};

// Input to the window's controls: a side button clicked (pressed and let
// go) over one, a Back or Forward key pressed and let go where the focus is.
class driver
{
public:
    virtual ~driver() = default;
    virtual bool ready() = 0;
    virtual void focus(target) = 0;
    // One side button pressed (down) or let go over a control.
    virtual void button(target, bool back, bool down) = 0;
    virtual void key(bool back) = 0;
    virtual void spin() = 0;
    void side(target t, bool back)
    {
        button(t, back, true);
        button(t, back, false);
    }
};

#if defined(RAPPORT_TEST_WIN32)

class win32_driver : public driver
{
public:
    bool ready() override
    {
        find();
        return list_ && entry_ && canvases_.size() == 3;
    }
    // A Back key pressed with the focus in the entry comes as WM_APPCOMMAND
    // to the entry; nothing to move.
    void focus(target) override {}
    void button(target t, bool back, bool down) override
    {
        const WORD x = back ? XBUTTON1 : XBUTTON2;
        const WORD mk = back ? MK_XBUTTON1 : MK_XBUTTON2;
        held_ = down ? WORD(held_ | mk) : WORD(held_ & ~mk);
        SendMessageW(of(t), down ? WM_XBUTTONDOWN : WM_XBUTTONUP, MAKEWPARAM(held_, x), MAKELPARAM(4, 4));
    }
    // The keyboard's raw keys, as they reach a focused control: a canvas
    // leaves them to DefWindowProc, which makes them the app command.
    void raw_key(target t, bool back)
    {
        const WPARAM vk = back ? VK_BROWSER_BACK : VK_BROWSER_FORWARD;
        SendMessageW(of(t), WM_KEYDOWN, vk, 1);
        SendMessageW(of(t), WM_KEYUP, vk, LPARAM(0xC0000001));
    }
    void app_command(target t, bool back)
    {
        const int command = back ? APPCOMMAND_BROWSER_BACKWARD : APPCOMMAND_BROWSER_FORWARD;
        SendMessageW(of(t), WM_APPCOMMAND, WPARAM(of(t)), MAKELPARAM(0, command | FAPPCOMMAND_KEY));
    }
    // As a keyboard's key reaches the focused control, after DefWindowProc
    // had its WM_KEYDOWN.
    void key(bool back) override
    {
        const int command = back ? APPCOMMAND_BROWSER_BACKWARD : APPCOMMAND_BROWSER_FORWARD;
        SendMessageW(entry_, WM_APPCOMMAND, WPARAM(entry_), MAKELPARAM(0, command | FAPPCOMMAND_KEY));
    }
    void spin() override {}

private:
    HWND of(target t) const
    {
        switch (t)
        {
        case target::list: return list_;
        case target::taking: return canvases_[0];
        case target::leaving: return canvases_[1];
        case target::mixed: return canvases_[2];
        case target::entry: return entry_;
        }
        return nullptr;
    }
    void find()
    {
        canvases_.clear();
        list_ = entry_ = nullptr;
        EnumThreadWindows(
            GetCurrentThreadId(),
            [](HWND top, LPARAM p) -> BOOL
            {
                EnumChildWindows(
                    top,
                    [](HWND c, LPARAM q) -> BOOL
                    {
                        auto* self = reinterpret_cast<win32_driver*>(q);
                        wchar_t cls[32] = {};
                        GetClassNameW(c, cls, 32);
                        if (!_wcsicmp(cls, L"rapportCanvas")) self->canvases_.push_back(c);
                        if (!_wcsicmp(cls, WC_LISTVIEWW)) self->list_ = c;
                        if (!_wcsicmp(cls, L"Edit")) self->entry_ = c;
                        return TRUE;
                    },
                    p);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(this));
    }

    HWND list_ = nullptr, entry_ = nullptr;
    std::vector<HWND> canvases_;  // in creation order: taking, leaving, mixed
    WORD held_ = 0;
};
using native_driver = win32_driver;
constexpr const char* backend_name = "win32";

#elif defined(RAPPORT_TEST_GTK3)

class gtk3_driver : public driver
{
public:
    bool ready() override
    {
        spin();
        if (!list_)
            for (GList* l = gtk_window_list_toplevels(); l; l = l->next) find(GTK_WIDGET(l->data));
        for (GtkWidget* w : {list_, taking_, leaving_, mixed_, entry_})
            if (!w || !gtk_widget_get_mapped(w)) return false;
        return true;
    }
    void focus(target t) override
    {
        gtk_widget_grab_focus(of(t));
        spin();
    }
    void button(target t, bool back, bool down) override
    {
        GdkEvent* e = gdk_event_new(down ? GDK_BUTTON_PRESS : GDK_BUTTON_RELEASE);
        e->button.window = GDK_WINDOW(g_object_ref(window_of(of(t))));
        e->button.send_event = true;
        // Far apart, so presses don't count as a double click.
        time_ += down ? 5000 : 1;
        e->button.time = time_;
        e->button.x = 4;
        e->button.y = 4;
        e->button.button = back ? 8 : 9;
        gdk_event_set_device(e, pointer());
        gdk_event_set_source_device(e, pointer());
        gtk_main_do_event(e);
        gdk_event_free(e);
        spin();
    }
    void key(bool back) override
    {
        GdkWindow* top = gtk_widget_get_window(gtk_widget_get_toplevel(list_));
        for (GdkEventType type : {GDK_KEY_PRESS, GDK_KEY_RELEASE})
        {
            GdkEvent* e = gdk_event_new(type);
            e->key.window = GDK_WINDOW(g_object_ref(top));
            e->key.send_event = true;
            e->key.time = ++time_;
            e->key.keyval = back ? GDK_KEY_Back : GDK_KEY_Forward;
            e->key.string = g_strdup("");
            gdk_event_set_device(
                e, gdk_seat_get_keyboard(gdk_display_get_default_seat(gdk_display_get_default())));
            gtk_main_do_event(e);
            gdk_event_free(e);
            spin();
        }
    }
    void spin() override
    {
        while (gtk_events_pending()) gtk_main_iteration_do(false);
    }

private:
    GtkWidget* of(target t) const
    {
        switch (t)
        {
        case target::list: return list_;
        case target::taking: return taking_;
        case target::leaving: return leaving_;
        case target::mixed: return mixed_;
        case target::entry: return entry_;
        }
        return nullptr;
    }
    void find(GtkWidget* w)
    {
        if (GTK_IS_TREE_VIEW(w)) list_ = w;
        if (GTK_IS_ENTRY(w)) entry_ = w;
        if (GTK_IS_DRAWING_AREA(w))
        {
            gchar* tip = gtk_widget_get_tooltip_text(w);
            const std::string name = tip ? tip : "";
            g_free(tip);
            if (name == "taking") taking_ = w;
            if (name == "leaving") leaving_ = w;
            if (name == "mixed") mixed_ = w;
        }
        if (GTK_IS_CONTAINER(w))
            gtk_container_forall(
                GTK_CONTAINER(w), +[](GtkWidget* c, gpointer p) { static_cast<gtk3_driver*>(p)->find(c); },
                this);
    }
    // The window a press on the widget comes in: a tree's rows, an
    // entry's text area, a drawing area's own.
    static GdkWindow* window_of(GtkWidget* w)
    {
        if (GTK_IS_TREE_VIEW(w)) return gtk_tree_view_get_bin_window(GTK_TREE_VIEW(w));
        GdkWindow* own = gtk_widget_get_window(w);
        if (gtk_widget_get_has_window(w)) return own;
        GdkWindow* found = own;
        GList* children = gdk_window_get_children(own);
        for (GList* l = children; l; l = l->next)
        {
            gpointer data = nullptr;
            gdk_window_get_user_data(GDK_WINDOW(l->data), &data);
            if (data == w) found = GDK_WINDOW(l->data);
        }
        g_list_free(children);
        return found;
    }
    static GdkDevice* pointer()
    {
        return gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
    }

    GtkWidget *list_ = nullptr, *taking_ = nullptr, *leaving_ = nullptr, *mixed_ = nullptr, *entry_ = nullptr;
    guint32 time_ = 1000;
};
using native_driver = gtk3_driver;
constexpr const char* backend_name = "gtk3";

#elif defined(RAPPORT_TEST_QT6)

class qt6_driver : public driver
{
public:
    bool ready() override
    {
        spin();
        if (!list_)
            for (QWidget* top : QApplication::topLevelWidgets())
            {
                if (auto* v = top->findChild<QTreeView*>()) list_ = v->viewport();
                entry_ = top->findChild<QLineEdit*>();
                for (QWidget* w : top->findChildren<QWidget*>())
                {
                    if (w->toolTip() == "taking") taking_ = w;
                    if (w->toolTip() == "leaving") leaving_ = w;
                    if (w->toolTip() == "mixed") mixed_ = w;
                }
            }
        for (QWidget* w : {list_, taking_, leaving_, mixed_, static_cast<QWidget*>(entry_)})
            if (!w || !w->isVisible()) return false;
        return true;
    }
    void focus(target t) override
    {
        QWidget* w = t == target::entry ? entry_ : of(t);
        w->activateWindow();
        w->setFocus(Qt::OtherFocusReason);
        spin();
        if (QApplication::focusWidget() != w) std::printf("note: focus didn't move (%s)\n", "offscreen");
    }
    void button(target t, bool back, bool down) override
    {
        QWidget* w = of(t);
        const QPointF at(4, 4);
        const Qt::MouseButton b = back ? Qt::BackButton : Qt::ForwardButton;
        if (down)
            held_ |= b;
        else
            held_ &= ~Qt::MouseButtons(b);
        QMouseEvent e(down ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease, at, w->mapToGlobal(at), b, held_,
                      Qt::NoModifier);
        e.setTimestamp(time_ += down ? 5000 : 1);
        QApplication::sendEvent(w, &e);
        spin();
    }
    void key(bool back) override
    {
        QWidget* to = QApplication::focusWidget() ? QApplication::focusWidget() : list_->window();
        const int k = back ? Qt::Key_Back : Qt::Key_Forward;
        QKeyEvent down(QEvent::KeyPress, k, Qt::NoModifier);
        QApplication::sendEvent(to, &down);
        QKeyEvent up(QEvent::KeyRelease, k, Qt::NoModifier);
        QApplication::sendEvent(to, &up);
        spin();
    }
    void spin() override { QApplication::processEvents(); }

private:
    QWidget* of(target t) const
    {
        switch (t)
        {
        case target::list: return list_;
        case target::taking: return taking_;
        case target::leaving: return leaving_;
        case target::mixed: return mixed_;
        case target::entry: return entry_;
        }
        return nullptr;
    }

    QWidget *list_ = nullptr, *taking_ = nullptr, *leaving_ = nullptr, *mixed_ = nullptr;
    QLineEdit* entry_ = nullptr;
    Qt::MouseButtons held_;
    quint64 time_ = 1000;
};
using native_driver = qt6_driver;
constexpr const char* backend_name = "qt6";

#endif

struct seen
{
    rap::key key;
    bool press;
    bool operator==(const seen&) const = default;
};

std::string show(const std::vector<seen>& keys)
{
    std::string s = "{";
    for (const seen& k : keys)
        s += std::string(s.size() > 1 ? "," : "") + (k.key == rap::key::back ? "back" : k.key == rap::key::forward ? "forward" : "other") +
             (k.press ? "" : " up");
    return s + "}";
}

struct state
{
    bool take = true;            // the window's on_key takes back and forward
    std::vector<seen> keys;      // what the window's on_key heard (back and forward only)
    std::vector<int> taking, leaving, mixed;  // the side buttons pressed on each canvas
    int canvas_keys = 0;  // keys heard by the leaving canvas's on_key, which takes them all
};

void run(native_driver& d, state& s, rap::list& list)
{
    const std::vector<seen> back{{rap::key::back, true}}, forward{{rap::key::forward, true}}, none;
    auto check = [&](const std::vector<seen>& want, const std::string& what)
    {
        expect(s.keys == want, what + ", got " + show(s.keys));
        s.keys.clear();
    };

    list.select({2});
    d.side(target::list, true);
    check(back, "the back button over a list: key::back, pressed, once");
#if !defined(RAPPORT_TEST_WIN32)
    expect(list.selection() == std::vector<size_t>{2}, "and the list doesn't select with it");
#endif
    d.side(target::list, false);
    check(forward, "the forward button over a list: key::forward");

    s.take = false;
    d.side(target::list, true);
    check(back, "not taken, it's still heard once (not again on its way)");
    s.take = true;

    d.side(target::taking, true);
    check(none, "a canvas taking the back button: no key");
    expect(s.taking == std::vector<int>{4}, "and the canvas had it as button 4");
    d.side(target::taking, false);
    check(none, "nor for forward");
    expect(s.taking == std::vector<int>{4, 5}, "as button 5");

    d.side(target::leaving, true);
    check(back, "a canvas leaving the back button: key::back");
    expect(s.leaving == std::vector<int>{4}, "the canvas saw it first, as button 4");
    d.side(target::leaving, false);
    check(forward, "and forward: key::forward");
    expect(s.leaving == std::vector<int>{4, 5}, "the canvas saw it as button 5");

    // Both held at once over a canvas taking back and not forward, pressed
    // and let go in every order: forward alone is a key, once.
    for (const bool back_first : {true, false})
        for (const bool back_up_first : {true, false})
        {
            const std::string order = std::string(back_first ? "back" : "forward") + " pressed first, " +
                                      (back_up_first ? "back" : "forward") + " let go first";
            d.button(target::mixed, back_first, true);
            d.button(target::mixed, !back_first, true);
            d.button(target::mixed, back_up_first, false);
            d.button(target::mixed, !back_up_first, false);
            check(forward, "both held over a canvas taking only back (" + order + "): forward alone");
        }
    expect(s.mixed.size() == 8, "the canvas saw every press");

    d.focus(target::entry);
    d.side(target::entry, true);
    check(back, "the back button over an entry being typed in: key::back");
    d.key(true);
    check(back, "the Back key while typing: key::back, pressed only");
    d.key(false);
    check(forward, "the Forward key: key::forward");

#if defined(RAPPORT_TEST_WIN32)
    // A focused canvas taking every key: the raw Back and Forward keys go
    // past it, to DefWindowProc; their app command reaches the window only.
    s.canvas_keys = 0;
    d.raw_key(target::leaving, true);
    d.raw_key(target::leaving, false);
    expect(s.canvas_keys == 0, "a canvas taking every key never hears the browser keys");
    s.keys.clear();
    d.app_command(target::leaving, true);
    check(back, "their app command from the canvas: the window's key::back");
    expect(s.canvas_keys == 0, "and the canvas hears nothing of it");
#else
    d.focus(target::leaving);
    s.take = false;
    s.canvas_keys = 0;
    d.key(true);
    check(back, "the Back key with a canvas focused: the window's");
    expect(s.canvas_keys == 0, "and the canvas's on_key, taking every key, never hears it");
    s.take = true;
#endif
}

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options opts;
#if !defined(RAPPORT_TEST_WIN32)
    opts.backend = backend_name;
#endif
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::printf("FAIL %s\n", ui.error().c_str());
        return 2;
    }
    rows model;
    rap::list* list = nullptr;
    state s;
    // A canvas's side buttons, recorded as they're pressed; it takes those
    // in `take` (their releases too, as an app would).
    auto side = [](std::vector<int>& into, std::vector<int> take)
    {
        return [&into, take](const rap::pointer_event& e)
        {
            if (e.button != 4 && e.button != 5) return false;
            if (e.type == rap::pointer_event_type::press) into.push_back(e.button);
            return std::ranges::find(take, e.button) != take.end();
        };
    };
    // Win32's canvases would take the focus as they're pressed, and with it
    // the window's activation; nothing there needs it.
#if defined(RAPPORT_TEST_WIN32)
    constexpr bool focusable = false;
#else
    constexpr bool focusable = true;
#endif
    using namespace rap::layout;
    auto win = (*ui)->create(window{
        "Back and forward",
        {vbox{
            rap::layout::list{{{"Name", {}, true}}, &model}.bind(list).stretch(),
            hbox{canvas{}.on_pointer(side(s.taking, {4, 5})).focusable(focusable).tooltip("taking").stretch(),
                 canvas{}
                     .on_pointer(side(s.leaving, {}))
                     .on_key(
                         [&](const rap::key_event&)
                         {
                             ++s.canvas_keys;
                             return true;
                         })
                     .focusable(focusable)
                     .tooltip("leaving")
                     .stretch(),
                 canvas{}.on_pointer(side(s.mixed, {4})).focusable(focusable).tooltip("mixed").stretch()}
                .height(4_lines),
            entry{"text"},
        }}}.size(30_ch, 20_lines));
    win->on_key(
        [&](const rap::key_event& e)
        {
            if (e.key != rap::key::back && e.key != rap::key::forward) return false;
            s.keys.push_back({e.key, e.press});
            return s.take;
        });
    win->show();
    native_driver d;
    (*ui)->post(
        [&]
        {
            bool ok = false;
            for (int i = 0; i < 500 && !(ok = d.ready()); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            expect(ok, std::string("the window is shown (") + backend_name + ")");
            if (ok && list) run(d, s, *list);
            std::printf(fails ? "%d failed\n" : "all passed\n", fails);
            std::fflush(stdout);
            (*ui)->quit(fails ? 1 : 0);
        });
    return (*ui)->run();
}
