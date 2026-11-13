// A window's shortcut chords with an entry focused, through a real backend:
// Ctrl+N and Ctrl+, are the window's on_key's (Expedition's new window and
// settings, as Explorer's work from its address bar), Ctrl+A and Ctrl+V
// stay the entry's editing keys, a chord the handler declines goes on to
// the entry, Alt+D taken stays from it, plain Alt and Ctrl+Alt with a
// character (AltGr) are never the window's, nothing at all (no chord, no
// Escape, no function key) while an input method is composing - on win32 the
// backend's test hook says one is - and Alt+F, declined, reaches the menu
// bar.
//   rapport-window-chords-test-win32  (a window never shown: messages posted
//                                      to its edit, taken by the backend's
//                                      message loop, as typing is)
//   rapport-window-chords-test-gtk3   (offscreen.sh gtk3: Broadway)
//   rapport-window-chords-test-gtk4   (offscreen.sh gtk4)
//   rapport-window-chords-test-qt6    (offscreen.sh qt6)
// GTK 4 can't synthesize events: the window's capture-phase key controllers
// are given the keys, and a key none takes is one GTK carries on to the
// entry; so there the entry's own handling (pasting, the menu bar's access
// keys) isn't seen.
#include <rapport/rapport.h>

#if defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
#include <gtk/gtk.h>
#elif defined(RAPPORT_TEST_QT6)
#include <QApplication>
#include <QClipboard>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QShortcut>
#include <QTest>
#elif defined(RAPPORT_TEST_WIN32)
#include <windows.h>
// After windows.h, which it needs.
#include <commctrl.h>
#endif

#include <cstdio>
#include <functional>
#include <optional>
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
    n,
    comma,
    a,
    v,
    k,
    j,
    d,
    f,
    q,
    alt,  // Alt alone
    f1,
    escape,
};

// What a key is, as both sides record it.
std::string token(k key)
{
    switch (key)
    {
    case k::n: return "n";
    case k::comma: return ",";
    case k::a: return "a";
    case k::v: return "v";
    case k::k: return "k";
    case k::j: return "j";
    case k::d: return "d";
    case k::f: return "f";
    case k::q: return "q";
    case k::alt: return "alt";
    case k::f1: return "f1";
    case k::escape: return "esc";
    }
    return "?";
}

constexpr unsigned ctrl = rap::mod_ctrl, alt = rap::mod_alt;

// The entry, found, given keys and read back.
class driver
{
public:
    virtual ~driver() = default;
    virtual bool ready() = 0;
    // The keys from now on go to the entry, as they do once it has the
    // focus (rapport's focus() where the window can have it).
    virtual void focus(rap::entry&) {}
    // Pressed and let go, as the user would with the entry focused.
    virtual void press(k, unsigned mods) = 0;
    // After a step: the keys' effects are in.
    virtual void settle() {}
    // An input method starting a composition in the entry, or ending it.
    virtual bool compose(bool) { return false; }
    // Whether the whole text is selected; nullopt where it can't be seen.
    virtual std::optional<bool> all_selected() { return std::nullopt; }
    // The clipboard the entry pastes from, the toolkit's own (never the
    // desktop's): false where the test can't have one of its own.
    virtual bool set_clipboard(const std::string&) { return false; }
    // The menu bar opened (an access key); nullopt where it can't be seen.
    virtual std::optional<bool> menu_open() { return std::nullopt; }
    virtual void close_menu() {}
    // A window-context shortcut of the toolkit's own (Qt's QShortcut) on
    // Ctrl+J, fired from the entry; false where there's none.
    virtual bool add_shortcut(std::function<void()>) { return false; }
    // The presses the entry was given, in order.
    std::vector<std::string> entry_keys;
};

#if defined(RAPPORT_TEST_GTK3)

guint keyval_of(k key)
{
    switch (key)
    {
    case k::n: return GDK_KEY_n;
    case k::comma: return GDK_KEY_comma;
    case k::a: return GDK_KEY_a;
    case k::v: return GDK_KEY_v;
    case k::k: return GDK_KEY_k;
    case k::j: return GDK_KEY_j;
    case k::d: return GDK_KEY_d;
    case k::f: return GDK_KEY_f;
    case k::q: return GDK_KEY_q;
    case k::alt: return GDK_KEY_Alt_L;
    case k::f1: return GDK_KEY_F1;
    case k::escape: return GDK_KEY_Escape;
    }
    return GDK_KEY_VoidSymbol;
}

std::string token_of(guint keyval)
{
    if (keyval == GDK_KEY_Alt_L) return "alt";
    if (keyval == GDK_KEY_F1) return "f1";
    if (keyval == GDK_KEY_Escape) return "esc";
    const gunichar c = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
    return c ? std::string(1, char(c)) : "other";
}

class native : public driver
{
public:
    bool ready() override
    {
        if (!entry_)
            for (GList* l = gtk_window_list_toplevels(); l; l = l->next) find(GTK_WIDGET(l->data));
        if (entry_ && !hooked_)
        {
            hooked_ = true;
            g_signal_connect(entry_, "key-press-event",
                             G_CALLBACK(+[](GtkWidget*, GdkEventKey* e, gpointer p) -> gboolean
                                        {
                                            static_cast<native*>(p)->entry_keys.push_back(token_of(e->keyval));
                                            return false;
                                        }),
                             this);
        }
        return entry_ && gtk_widget_get_mapped(entry_);
    }
    void focus(rap::entry& e) override
    {
        e.focus();
        spin();
    }
    void press(k key, unsigned mods) override
    {
        const guint keyval = keyval_of(key);
        GdkWindow* top = gtk_widget_get_window(gtk_widget_get_toplevel(entry_));
        for (GdkEventType type : {GDK_KEY_PRESS, GDK_KEY_RELEASE})
        {
            GdkEvent* e = gdk_event_new(type);
            e->key.window = GDK_WINDOW(g_object_ref(top));
            e->key.send_event = true;
            e->key.time = ++time_;
            e->key.keyval = keyval;
            e->key.state = GdkModifierType(((mods & ctrl) ? GDK_CONTROL_MASK : 0) |
                                           ((mods & alt) ? GDK_MOD1_MASK : 0));
            if (key == k::alt) e->key.state = GdkModifierType(type == GDK_KEY_PRESS ? 0 : GDK_MOD1_MASK);
            GdkKeymapKey* keys = nullptr;
            gint n = 0;
            if (gdk_keymap_get_entries_for_keyval(gdk_keymap_get_for_display(gdk_display_get_default()), keyval,
                                                  &keys, &n) &&
                n > 0)
                e->key.hardware_keycode = guint16(keys[0].keycode);
            g_free(keys);
            e->key.is_modifier = key == k::alt;
            e->key.string = g_strdup("");
            gdk_event_set_device(e, gdk_seat_get_keyboard(gdk_display_get_default_seat(gdk_display_get_default())));
            gtk_main_do_event(e);
            gdk_event_free(e);
        }
        spin();
    }
    void settle() override { spin(); }
    bool compose(bool on) override
    {
        g_signal_emit_by_name(entry_, "preedit-changed", on ? "ka" : "");
        return true;
    }
    std::optional<bool> all_selected() override
    {
        int from = 0, to = 0;
        return gtk_editable_get_selection_bounds(GTK_EDITABLE(entry_), &from, &to) && from == 0 &&
               to == gtk_entry_get_text_length(GTK_ENTRY(entry_));
    }
    bool set_clipboard(const std::string& s) override
    {
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), s.c_str(), -1);
        spin();
        return true;
    }
    std::optional<bool> menu_open() override
    {
        return bar_ && gtk_menu_shell_get_selected_item(GTK_MENU_SHELL(bar_)) != nullptr;
    }
    void close_menu() override
    {
        if (bar_) gtk_menu_shell_deactivate(GTK_MENU_SHELL(bar_));
        spin();
    }

private:
    void find(GtkWidget* w)
    {
        if (GTK_IS_ENTRY(w) && !entry_) entry_ = w;
        if (GTK_IS_MENU_BAR(w) && !bar_) bar_ = w;
        if (GTK_IS_CONTAINER(w))
            gtk_container_forall(
                GTK_CONTAINER(w), +[](GtkWidget* c, gpointer p) { static_cast<native*>(p)->find(c); }, this);
    }
    static void spin()
    {
        while (gtk_events_pending()) gtk_main_iteration_do(false);
    }
    GtkWidget* entry_ = nullptr;
    GtkWidget* bar_ = nullptr;
    bool hooked_ = false;
    guint32 time_ = 1000;
};
constexpr const char* backend_name = "gtk3";

#elif defined(RAPPORT_TEST_GTK4)

guint keyval_of(k key)
{
    switch (key)
    {
    case k::n: return GDK_KEY_n;
    case k::comma: return GDK_KEY_comma;
    case k::a: return GDK_KEY_a;
    case k::v: return GDK_KEY_v;
    case k::k: return GDK_KEY_k;
    case k::j: return GDK_KEY_j;
    case k::d: return GDK_KEY_d;
    case k::f: return GDK_KEY_f;
    case k::q: return GDK_KEY_q;
    case k::alt: return GDK_KEY_Alt_L;
    case k::f1: return GDK_KEY_F1;
    case k::escape: return GDK_KEY_Escape;
    }
    return GDK_KEY_VoidSymbol;
}

class native : public driver
{
public:
    bool ready() override
    {
        if (!text_)
        {
            GListModel* tops = gtk_window_get_toplevels();
            for (guint i = 0; i < g_list_model_get_n_items(tops) && !text_; ++i)
            {
                auto* w = GTK_WIDGET(g_list_model_get_item(tops, i));
                win_ = w;
                find(w);
                g_object_unref(w);
            }
        }
        return text_ && gtk_widget_get_mapped(text_);
    }
    void focus(rap::entry& e) override
    {
        e.focus();
        spin();
    }
    // The window's capture-phase key controllers, the last added first: the
    // first to take the key has it; none taking it, GTK carries it on to
    // the entry.
    void press(k key, unsigned mods) override
    {
        const guint keyval = keyval_of(key);
        auto state = GdkModifierType(((mods & ctrl) ? GDK_CONTROL_MASK : 0) | ((mods & alt) ? GDK_ALT_MASK : 0));
        if (key == k::alt) state = GdkModifierType(0);
        GListModel* controllers = gtk_widget_observe_controllers(win_);
        const guint n = g_list_model_get_n_items(controllers);
        gboolean handled = false;
        for (guint i = n; i-- > 0 && !handled;)
        {
            auto* c = GTK_EVENT_CONTROLLER(g_list_model_get_item(controllers, i));
            if (GTK_IS_EVENT_CONTROLLER_KEY(c) && gtk_event_controller_get_propagation_phase(c) == GTK_PHASE_CAPTURE)
            {
                g_signal_emit_by_name(c, "key-pressed", keyval, 0u, state, &handled);
                g_signal_emit_by_name(c, "key-released", keyval, 0u, state);
            }
            g_object_unref(c);
        }
        g_object_unref(controllers);
        if (!handled) entry_keys.push_back(token(key));
        spin();
    }
    void settle() override { spin(); }
    bool compose(bool on) override
    {
        g_signal_emit_by_name(text_, "preedit-changed", on ? "ka" : "");
        return true;
    }

private:
    void find(GtkWidget* w)
    {
        if (GTK_IS_TEXT(w) && !text_) text_ = w;
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) find(c);
    }
    static void spin()
    {
        while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, false);
    }
    GtkWidget* text_ = nullptr;
    GtkWidget* win_ = nullptr;
};
constexpr const char* backend_name = "gtk4";

#elif defined(RAPPORT_TEST_QT6)

class native : public driver, public QObject
{
public:
    bool ready() override
    {
        if (!edit_)
            for (QWidget* w : QApplication::topLevelWidgets())
            {
                if (auto* e = w->findChild<QLineEdit*>()) edit_ = e;
                if (auto* b = w->findChild<QMenuBar*>()) bar_ = b;
            }
        if (edit_ && !hooked_)
        {
            hooked_ = true;
            edit_->installEventFilter(this);  // after the application's: what reaches the edit
        }
        return edit_ && edit_->isVisible();
    }
    void focus(rap::entry& e) override
    {
        edit_->window()->activateWindow();
        QApplication::processEvents();
        e.focus();
        QApplication::processEvents();
    }
    void press(k key, unsigned mods) override
    {
        Qt::KeyboardModifiers m;
        if (mods & ctrl) m |= Qt::ControlModifier;
        if (mods & alt) m |= Qt::AltModifier;
        QWidget* to = QApplication::focusWidget() ? QApplication::focusWidget() : edit_;
        if (key == k::alt)
        {
            saw_alt_ = false;
            QTest::keyPress(to, Qt::Key_Alt, Qt::NoModifier);
            QTest::keyRelease(to, Qt::Key_Alt, Qt::AltModifier);
            if (saw_alt_) entry_keys.push_back("alt");
        }
        else
            QTest::keyClick(to, qt_key(key), m);
        QApplication::processEvents();
    }
    void settle() override { QApplication::processEvents(); }
    bool compose(bool on) override
    {
        QInputMethodEvent e(on ? QStringLiteral("ka") : QString(), {});
        QApplication::sendEvent(edit_, &e);
        QApplication::processEvents();
        return true;
    }
    std::optional<bool> all_selected() override
    {
        return edit_->hasSelectedText() && edit_->selectedText() == edit_->text();
    }
    bool set_clipboard(const std::string& s) override
    {
        QApplication::clipboard()->setText(QString::fromStdString(s));
        return true;
    }
    std::optional<bool> menu_open() override { return bar_ && bar_->activeAction() != nullptr; }
    // A menu open, or the menu bar taking the keys (Alt alone, where the
    // style navigates with it): closed, and the keys back to the entry.
    void close_menu() override
    {
        if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
        QApplication::processEvents();
        if (bar_ && (bar_->activeAction() || bar_->hasFocus())) QTest::keyClick(bar_, Qt::Key_Escape);
        QApplication::processEvents();
        edit_->setFocus();
        QApplication::processEvents();
    }
    bool add_shortcut(std::function<void()> f) override
    {
        auto* s = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_J), edit_->window());
        s->setContext(Qt::WindowShortcut);
        QObject::connect(s, &QShortcut::activated, s, std::move(f));
        return true;
    }

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        // QTest presses a chord's modifiers first, each a key of its own:
        // those aren't the chord's (Alt alone is noted, for its step).
        if (e->type() == QEvent::KeyPress)
        {
            const int key = static_cast<QKeyEvent*>(e)->key();
            if (key == Qt::Key_Alt)
                saw_alt_ = true;
            else if (key == Qt::Key_F1)
                entry_keys.push_back("f1");
            else if (key == Qt::Key_Escape)
                entry_keys.push_back("esc");
            else if (key != Qt::Key_Control && key != Qt::Key_Shift && key != Qt::Key_Meta)
                entry_keys.push_back(std::string(1, char(QChar::toLower(char32_t(key)))));
        }
        return false;
    }

private:
    static Qt::Key qt_key(k key)
    {
        switch (key)
        {
        case k::n: return Qt::Key_N;
        case k::comma: return Qt::Key_Comma;
        case k::a: return Qt::Key_A;
        case k::v: return Qt::Key_V;
        case k::k: return Qt::Key_K;
        case k::j: return Qt::Key_J;
        case k::d: return Qt::Key_D;
        case k::f: return Qt::Key_F;
        case k::q: return Qt::Key_Q;
        case k::alt: return Qt::Key_Alt;
        case k::f1: return Qt::Key_F1;
        case k::escape: return Qt::Key_Escape;
        }
        return Qt::Key_unknown;
    }
    QLineEdit* edit_ = nullptr;
    QMenuBar* bar_ = nullptr;
    bool hooked_ = false;
    bool saw_alt_ = false;
};
constexpr const char* backend_name = "qt6";

#elif defined(RAPPORT_TEST_WIN32)

// The window is never shown, so never active and never has the focus: the
// keys are posted to its edit, with the modifiers in this thread's key state
// (as the document tabs' test does), for the backend's message loop to take
// as it takes typing - the window's on_key first where it's to have them.
class native : public driver
{
public:
    bool ready() override
    {
        if (!edit_) EnumThreadWindows(GetCurrentThreadId(), find, reinterpret_cast<LPARAM>(this));
        if (edit_ && !hooked_) hooked_ = SetWindowSubclass(edit_, record, 0x6368, reinterpret_cast<DWORD_PTR>(this));
        return edit_ && hooked_;
    }
    void press(k key, unsigned mods) override
    {
        WPARAM vk = 0;
        switch (key)
        {
        case k::n: vk = 'N'; break;
        case k::comma: vk = VK_OEM_COMMA; break;
        case k::a: vk = 'A'; break;
        case k::v: vk = 'V'; break;
        case k::k: vk = 'K'; break;
        case k::j: vk = 'J'; break;
        case k::d: vk = 'D'; break;
        case k::f: vk = 'F'; break;
        case k::q: vk = 'Q'; break;
        case k::alt: vk = VK_MENU; break;
        case k::f1: vk = VK_F1; break;
        case k::escape: vk = VK_ESCAPE; break;
        }
        const bool with_alt = (mods & alt) || key == k::alt;
        BYTE state[256] = {};
        GetKeyboardState(state);
        state[VK_CONTROL] = state[VK_LCONTROL] = (mods & ctrl) ? 0x80 : 0;
        state[VK_MENU] = state[VK_LMENU] = with_alt ? 0x80 : 0;
        SetKeyboardState(state);
        // Alt without Ctrl makes the system's keys (as the menu bar has them).
        const bool sys = with_alt && !(mods & ctrl);
        LPARAM lp = 1 | (LPARAM(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC)) << 16);
        if (with_alt) lp |= LPARAM(1) << 29;
        PostMessageW(edit_, sys ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, lp);
        PostMessageW(edit_, sys ? WM_SYSKEYUP : WM_KEYUP, vk, lp | (LPARAM(3) << 30));
        pending_ = true;
    }
    // The posted keys have been taken (a timer tick after them): the thread's
    // key state as it was.
    void settle() override
    {
        if (!pending_) return;
        pending_ = false;
        BYTE state[256] = {};
        GetKeyboardState(state);
        state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_MENU] = state[VK_LMENU] = 0;
        SetKeyboardState(state);
    }
    // No IME can be driven without the system's input: the backend's test
    // hook says one is composing in the edit.
    bool compose(bool on) override
    {
        if (on)
            SetPropW(edit_, L"rapport-test-composing", HANDLE(1));
        else
            RemovePropW(edit_, L"rapport-test-composing");
        return true;
    }
    std::optional<bool> all_selected() override
    {
        DWORD from = 0, to = 0;
        SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&from), reinterpret_cast<LPARAM>(&to));
        return from == 0 && to == DWORD(GetWindowTextLengthW(edit_)) && to > 0;
    }

private:
    static BOOL CALLBACK find(HWND top, LPARAM p)
    {
        EnumChildWindows(
            top,
            [](HWND c, LPARAM p) -> BOOL
            {
                wchar_t cls[16] = {};
                GetClassNameW(c, cls, 16);
                if (_wcsicmp(cls, L"Edit")) return TRUE;
                reinterpret_cast<native*>(p)->edit_ = c;
                return FALSE;
            },
            p);
        return !reinterpret_cast<native*>(p)->edit_;
    }
    static std::string token_of(WPARAM vk)
    {
        if (vk == VK_MENU) return "alt";
        if (vk == VK_F1) return "f1";
        if (vk == VK_ESCAPE) return "esc";
        if (vk == VK_OEM_COMMA) return ",";
        if (vk >= 'A' && vk <= 'Z') return std::string(1, char(vk - 'A' + 'a'));
        return "other";
    }
    // What reaches the edit. Past it, Alt's keys would go on (DefWindowProc)
    // to the menu bar's loop, and the control characters to a beep or the
    // user's clipboard: those stop here, once seen. Ctrl+A's goes on, to
    // select all.
    static LRESULT CALLBACK record(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<native*>(data);
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) self->entry_keys.push_back(token_of(wp));
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_SYSCHAR || msg == WM_SYSDEADCHAR) return 0;
        if (msg == WM_CHAR && wp < 0x20 && wp != 0x01) return 0;
        return DefSubclassProc(h, msg, wp, lp);
    }
    HWND edit_ = nullptr;
    bool hooked_ = false;
    bool pending_ = false;
};
constexpr const char* backend_name = "win32";

#endif

std::string show(const std::vector<std::string>& v)
{
    std::string s = "[";
    for (const std::string& t : v) s += (s.size() > 1 ? " " : "") + t;
    return s + "]";
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
    rap::entry* field = nullptr;
    int file_menu_built = 0;
    using namespace rap::layout;
    auto win = (*ui)->create(
        window{"Chords", {vbox{entry{"hello"}.bind(field)}}}
            .menu_bar({{"_File",
                        [&]
                        {
                            ++file_menu_built;
                            return rap::menu{{"_Close", [] {}}};
                        }}})
            .size(30_ch, 8_lines));
    // The window's keys: what it's given, and what it takes. Ctrl+N, Ctrl+,
    // and Alt+D; Ctrl+K, Ctrl+J and Alt+F declined.
    std::vector<std::string> window_keys;
    win->on_key(
        [&](const rap::key_event& e)
        {
            if (!e.press) return false;
            const std::string t = e.key == rap::key::character ? std::string(1, char(e.codepoint))
                                  : e.key == rap::key::f1     ? "f1"
                                  : e.key == rap::key::escape ? "esc"
                                                              : "other";
            window_keys.push_back(t);
            const unsigned m = e.modifiers & (rap::mod_ctrl | rap::mod_alt | rap::mod_shift);
            return (m == ctrl && (t == "n" || t == ",")) || (m == alt && t == "d");
        });
#if !defined(RAPPORT_TEST_WIN32)
    win->show();  // off the desktop: Broadway, or Qt's offscreen platform
#endif
    native d;

    // Each step's keys, then (a tick on) what they did.
    std::vector<std::function<void()>> steps;
    size_t step = 0;
    auto chord = [&](k key, unsigned mods)
    {
        window_keys.clear();
        d.entry_keys.clear();
        d.press(key, mods);
    };
    auto window_had = [&](const std::string& what, const std::vector<std::string>& want_window,
                          const std::vector<std::string>& want_entry)
    {
        expect(window_keys == want_window && d.entry_keys == want_entry,
               what + " (window " + show(window_keys) + ", entry " + show(d.entry_keys) + ")");
    };
    bool shortcut = false, shortcut_fired = false, composing = false;
    steps.push_back(
        [&]
        {
            d.focus(*field);
            shortcut = d.add_shortcut([&] { shortcut_fired = true; });
            chord(k::n, ctrl);
        });
    steps.push_back(
        [&]
        {
            window_had("Ctrl+N with the entry focused: the window's, not the entry's", {"n"}, {});
            expect(field->text() == "hello", "and the entry's text is as it was");
            chord(k::comma, ctrl);
        });
    steps.push_back(
        [&]
        {
            window_had("Ctrl+,: the window's", {","}, {});
            chord(k::a, ctrl);
        });
    bool own_clipboard = false;
    steps.push_back(
        [&]
        {
            window_had("Ctrl+A: the entry's, never the window's", {}, {"a"});
            if (auto all = d.all_selected()) expect(*all, "and it selects all the entry's text");
            own_clipboard = d.set_clipboard("pasted");
            chord(k::v, ctrl);
        });
    steps.push_back(
        [&]
        {
            window_had("Ctrl+V: the entry's, never the window's", {}, {"v"});
            if (own_clipboard)
                expect(field->text() == "pasted", "and it pastes, over the selection (\"" + field->text() + "\")");
            chord(k::k, ctrl);
        });
    steps.push_back(
        [&]
        {
            window_had("Ctrl+K, declined: the window's first, then the entry's", {"k"}, {"k"});
            chord(k::d, alt);
        });
    steps.push_back(
        [&]
        {
            window_had("Alt+D, taken: the window's only", {"d"}, {});
            chord(k::alt, 0);
        });
    steps.push_back(
        [&]
        {
            window_had("Alt alone: the entry's (and the menu bar's), never the window's", {}, {"alt"});
            // Where Alt alone leaves the menu bar with the keys (Qt's styles
            // that navigate with it, as for the user), they're given back.
            if (auto open = d.menu_open(); open && *open) std::printf("     (Alt alone: the menu bar has the keys)\n");
            d.close_menu();
            chord(k::q, ctrl | alt);
        });
    steps.push_back(
        [&]
        {
            window_had("Ctrl+Alt+Q (AltGr+Q): typing, the entry's", {}, {"q"});
            composing = d.compose(true);
            expect(composing, "an input method's composition, started in the entry");
            chord(k::n, ctrl);
        });
    // While it composes nothing is the window's: no chord, no Escape, no
    // function key. They're the input method's (Escape cancels, Japanese
    // ones convert with F6-F10), so they go on to the entry.
    steps.push_back(
        [&]
        {
            window_had("Ctrl+N while an input method composes: the entry's, not the window's", {}, {"n"});
            chord(k::f1, ctrl);
        });
    steps.push_back(
        [&]
        {
            window_had("Ctrl+F1 while composing: the entry's, not the window's", {}, {"f1"});
            chord(k::f1, 0);
        });
    steps.push_back(
        [&]
        {
            window_had("F1 while composing: the entry's, not the window's", {}, {"f1"});
            chord(k::escape, 0);
        });
    steps.push_back(
        [&]
        {
#if defined(RAPPORT_TEST_WIN32)
            // A single-line edit never has Escape: the window's dialog
            // handling (IsDialogMessage) takes it past the edit, as ever.
            window_had("Escape while composing: not the window's", {}, {});
#else
            window_had("Escape while composing: the entry's, not the window's", {}, {"esc"});
#endif
            d.compose(false);
            chord(k::n, ctrl);
        });
    // Once it commits or cancels, the rules as before.
    steps.push_back(
        [&]
        {
            window_had("Ctrl+N once the composition ends: the window's again", {"n"}, {});
            chord(k::f1, ctrl);
        });
    steps.push_back(
        [&]
        {
            window_had("Ctrl+F1 once it ends: the window's first, declined, then the entry's", {"f1"}, {"f1"});
            chord(k::f1, 0);
        });
    steps.push_back(
        [&]
        {
            window_had("F1 once it ends: the window's first, declined, then the entry's", {"f1"}, {"f1"});
            chord(k::escape, 0);
        });
    steps.push_back(
        [&]
        {
#if defined(RAPPORT_TEST_WIN32)
            window_had("Escape once it ends: the window's again", {"esc"}, {});
#else
            window_had("Escape once it ends: the window's first, declined, then the entry's", {"esc"}, {"esc"});
#endif
            if (shortcut)
                chord(k::j, ctrl);
            else
            {
                ++step;  // no shortcut of the toolkit's to try
                chord(k::f, alt);
            }
        });
    steps.push_back(
        [&]
        {
            // Qt's QShortcut, window-wide, beats a QLineEdit for a chord the
            // edit has no use for: before the window's on_key, which never
            // sees it.
            expect(shortcut_fired, "a window-wide QShortcut (Ctrl+J) fires from the entry");
            expect(window_keys.empty() && d.entry_keys.empty(),
                   "and neither the window's on_key nor the entry has its key (window " + show(window_keys) +
                       ", entry " + show(d.entry_keys) + ")");
            chord(k::f, alt);
        });
    steps.push_back(
        [&]
        {
            if (auto open = d.menu_open())
                expect(*open && file_menu_built > 0, "Alt+F: the menu bar's File menu opens");
#if defined(RAPPORT_TEST_QT6)
            // The access key is a Qt shortcut: it's the menu bar's before
            // the window or the entry sees the key.
            window_had("Alt+F: the menu bar's, first", {}, {});
#elif defined(RAPPORT_TEST_GTK3)
            // Declined, it goes on to the window's own handling, whose
            // access keys take it before the entry.
            window_had("Alt+F, declined: the window's first, then the menu bar's", {"f"}, {});
#else
            // Declined, it goes on to the entry, and past it (DefWindowProc;
            // GTK 4's window) to the menu bar.
            window_had("Alt+F, declined: the window's first, then on to the entry", {"f"}, {"f"});
#endif
            d.close_menu();
        });

    int waited = 0;
    rap::subscription tick = (*ui)->timer(
        30, true,
        [&]
        {
            d.settle();
            if (step == 0 && !d.ready())
            {
                if (++waited < 300) return;
                expect(false, std::string("the entry is there (") + backend_name + ")");
            }
            else if (step < steps.size())
            {
                steps[step++]();
                return;
            }
            if (fails)
                std::printf("%d failed (%s)\n", fails, backend_name);
            else
                std::printf("all passed (%s)\n", backend_name);
            std::fflush(stdout);
            (*ui)->quit(fails ? 1 : 0);
        });
    return (*ui)->run();
}
