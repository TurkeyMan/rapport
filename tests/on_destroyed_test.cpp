// window::on_destroyed: once, at the start of the window's destructor, its
// widgets still there; whoever destroys it, the app or the backend's own
// callback after a close; not at all once cleared; the last one set.
//   rapport-on-destroyed-test         (the null backend)
//   rapport-on-destroyed-test-win32   (windows never shown; closed by WM_CLOSE)
//   rapport-on-destroyed-test-gtk3    (Broadway)
//   rapport-on-destroyed-test-gtk4    (Broadway)
//   rapport-on-destroyed-test-qt6     (offscreen)
#include <rapport/rapport.h>

#if defined(RAPPORT_TEST_WIN32)
#include <windows.h>
#elif defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
#include <gtk/gtk.h>
#elif defined(RAPPORT_TEST_QT6)
#include <QApplication>
#include <QLabel>
#include <QWidget>
#endif

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(RAPPORT_TEST_WIN32) && defined(RAPPORT_TEST_BUILTIN_WIN32)
RAPPORT_BUILTIN_BACKEND(win32);
#endif
#if defined(RAPPORT_TEST_BUILTIN_NULL)
RAPPORT_BUILTIN_BACKEND(null);
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

#if defined(RAPPORT_TEST_WIN32)
constexpr const char* backend_name = "win32";
constexpr bool shows = false;  // never shown: the user's desktop
#elif defined(RAPPORT_TEST_GTK3)
constexpr const char* backend_name = "gtk3";
constexpr bool shows = true;  // realized, so GTK 3 closes it
#elif defined(RAPPORT_TEST_GTK4)
constexpr const char* backend_name = "gtk4";
constexpr bool shows = true;
#elif defined(RAPPORT_TEST_QT6)
constexpr const char* backend_name = "qt6";
constexpr bool shows = true;
#else
constexpr const char* backend_name = "null";
constexpr bool shows = false;
#endif
constexpr bool native = std::string_view(backend_name) != "null";

// The toolkit's own window titled `title`, whether it's there, whether a
// label in it shows `text`, and closing it as its close box would. The
// null backend has no window to look at or close.
#if defined(RAPPORT_TEST_WIN32)
std::wstring wide(const char* s) { return std::wstring(s, s + std::strlen(s)); }
HWND find_window(const char* title)
{
    struct q
    {
        std::wstring title;
        HWND found = nullptr;
    } s{wide(title)};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM p) -> BOOL
        {
            auto* s = reinterpret_cast<q*>(p);
            wchar_t t[64] = {};
            GetWindowTextW(h, t, 64);
            if (s->title == t && IsWindow(h)) s->found = h;
            return TRUE;
        },
        LPARAM(&s));
    return s.found;
}
bool native_window(const char* title) { return find_window(title) != nullptr; }
bool native_label(const char* title, const char* text)
{
    HWND h = find_window(title);
    if (!h) return false;
    struct q
    {
        std::wstring text;
        bool found = false;
    } s{wide(text)};
    EnumChildWindows(
        h,
        [](HWND c, LPARAM p) -> BOOL
        {
            auto* s = reinterpret_cast<q*>(p);
            wchar_t t[128] = {};
            GetWindowTextW(c, t, 128);
            if (s->text == t) s->found = true;
            return TRUE;
        },
        LPARAM(&s));
    return s.found;
}
// WM_CLOSE, as the close box sends it; the window stays hidden.
void native_close(const char* title)
{
    if (HWND h = find_window(title)) SendMessageW(h, WM_CLOSE, 0, 0);
}
#elif defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
GtkWidget* find_window(const char* title)
{
    GtkWidget* found = nullptr;
#if defined(RAPPORT_TEST_GTK3)
    GList* all = gtk_window_list_toplevels();
    for (GList* l = all; l; l = l->next)
        if (const char* t = gtk_window_get_title(GTK_WINDOW(l->data)); t && !std::strcmp(t, title))
            found = GTK_WIDGET(l->data);
    g_list_free(all);
#else
    GListModel* all = gtk_window_get_toplevels();
    for (guint i = 0, n = g_list_model_get_n_items(all); i < n; ++i)
    {
        auto* w = static_cast<GtkWidget*>(g_list_model_get_item(all, i));
        if (const char* t = gtk_window_get_title(GTK_WINDOW(w)); t && !std::strcmp(t, title))
            found = w;
        g_object_unref(w);
    }
#endif
    return found;
}
bool native_window(const char* title) { return find_window(title) != nullptr; }
bool has_label(GtkWidget* w, const char* text)
{
    if (GTK_IS_LABEL(w) && !std::strcmp(gtk_label_get_text(GTK_LABEL(w)), text)) return true;
#if defined(RAPPORT_TEST_GTK3)
    if (!GTK_IS_CONTAINER(w)) return false;
    struct q
    {
        const char* text;
        bool found = false;
    } s{text};
    gtk_container_forall(
        GTK_CONTAINER(w),
        +[](GtkWidget* c, gpointer p)
        {
            auto* s = static_cast<q*>(p);
            if (!s->found && has_label(c, s->text)) s->found = true;
        },
        &s);
    return s.found;
#else
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (has_label(c, text)) return true;
    return false;
#endif
}
bool native_label(const char* title, const char* text)
{
    GtkWidget* w = find_window(title);
    return w && has_label(w, text);
}
// As the close box would: GTK 3 sends delete-event from an idle, GTK 4
// emits close-request.
void native_close(const char* title)
{
    if (GtkWidget* w = find_window(title)) gtk_window_close(GTK_WINDOW(w));
}
#elif defined(RAPPORT_TEST_QT6)
QWidget* find_window(const char* title)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->windowTitle() == QString::fromUtf8(title)) return w;
    return nullptr;
}
bool native_window(const char* title) { return find_window(title) != nullptr; }
bool native_label(const char* title, const char* text)
{
    QWidget* w = find_window(title);
    if (!w) return false;
    for (QLabel* l : w->findChildren<QLabel*>())
        if (l->text() == QString::fromUtf8(text)) return true;
    return false;
}
// QWidget::close: the closeEvent the close box gives.
void native_close(const char* title)
{
    if (QWidget* w = find_window(title)) w->close();
}
#else
bool native_window(const char*) { return true; }
bool native_label(const char*, const char*) { return true; }
void native_close(const char*) {}
#endif

constexpr const char* label_text = "bound label";
constexpr const char* entry_text = "bound entry";
constexpr const char* handler_text = "set in on_destroyed";

// A window with a bound label and entry, for the handler to read.
struct made
{
    std::unique_ptr<rap::window> win;
    rap::label* label = nullptr;
    rap::entry* entry = nullptr;
};

made make(rap::ui& ui, const char* title, bool dialog = false)
{
    made m;
    using namespace rap::layout;
    const rap::extent width{30, rap::extent::unit::chars};
    if (dialog)
        m.win = ui.create(rap::layout::dialog{title,
                                              {rap::layout::label{label_text}.bind(m.label),
                                               rap::layout::entry{entry_text}.bind(m.entry)}}
                              .size(width, 6_lines));
    else
        m.win = ui.create(rap::layout::window{title,
                                              {rap::layout::label{label_text}.bind(m.label),
                                               rap::layout::entry{entry_text}.bind(m.entry)}}
                              .size(width, 6_lines));
    // Given here too: the null backend's entry keeps only what it's set.
    m.entry->set_text(entry_text);
    if (shows) m.win->show();
    return m;
}

// What a handler saw of its window's widgets.
struct seen
{
    int count = 0;
    bool window = false;   // the native window, still there
    bool label = false;    // the bound label, showing its text
    bool set = false;      // the bound label, given new text from the handler
    std::string entry;     // the bound entry's text
};

// Counts, and reads the window's widgets as the handler runs.
std::function<void()> reader(seen& s, made& m, const char* title)
{
    return [&s, &m, title]
    {
        ++s.count;
        s.window = native_window(title);
        s.label = native_label(title, label_text);
        s.entry = m.entry->text();
        m.label->set_text(handler_text);
        s.set = native_label(title, handler_text);
    };
}

void check_seen(const seen& s, const std::string& what)
{
    expect(s.count == 1, what + ": fired exactly once (" + std::to_string(s.count) + ")");
    expect(s.window, what + ": the native window still there in the handler");
    expect(s.label && s.set, what + ": the bound label read and set from the handler");
    expect(s.entry == entry_text, what + ": the bound entry's text read from the handler (\"" +
                                      s.entry + "\")");
}

// Destroyed by the app, outside any of the backend's callbacks.
void sync_checks(rap::ui& ui)
{
    {
        const char* title = "on_destroyed reset";
        made m = make(ui, title);
        seen s;
        m.win->on_destroyed(reader(s, m, title));
        expect(s.count == 0, "not fired while the window lives");
        m.win.reset();
        check_seen(s, "the app's pointer reset");
    }
    {
        made m = make(ui, "on_destroyed cleared");
        int count = 0;
        m.win->on_destroyed([&] { ++count; });
        m.win->on_destroyed({});
        m.win.reset();
        expect(count == 0, "cleared with an empty function: not fired");
    }
    {
        const char* title = "on_destroyed replaced";
        made m = make(ui, title);
        int first = 0;
        seen s;
        m.win->on_destroyed([&] { ++first; });
        m.win->on_destroyed(reader(s, m, title));
        m.win.reset();
        expect(first == 0, "set again: the first handler not fired");
        check_seen(s, "set again: the second");
    }
    {
        // Other windows made and destroyed from the handler.
        made x = make(ui, "on_destroyed x");
        made y = make(ui, "on_destroyed y");
        made z;
        int xs = 0, ys = 0, zs = 0;
        y.win->on_destroyed([&] { ++ys; });
        x.win->on_destroyed(
            [&]
            {
                ++xs;
                y.win.reset();
                z = make(ui, "on_destroyed z");
                z.win->on_destroyed([&] { ++zs; });
            });
        x.win.reset();
        expect(xs == 1 && ys == 1, "another window destroyed from the handler: each fired once");
        expect(z.win && zs == 0, "another window made from the handler: there, not fired");
        z.win.reset();
        expect(zs == 1 && xs == 1 && ys == 1, "and once that's destroyed, it fired once");
    }
}

// Closed as by the close box, then destroyed: a dialog from its on_finished
// (inside the backend's callback), a window by the app afterwards.
struct close_state
{
    rap::ui* ui = nullptr;
    rap::subscription poll;
    int tries = 0;
    int stage = 0;
    made dialog, win;
    seen dialog_seen, win_seen;
    bool finished = false;
    int count_at_finish = -1;
};

const char* dialog_title = "on_destroyed dialog";
const char* window_title = "on_destroyed window";

void finish(close_state& s)
{
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    std::fflush(stdout);
    s.ui->quit(fails ? 1 : 0);
}

void close_checks(close_state& s)
{
    s.dialog = make(*s.ui, dialog_title, true);
    s.dialog.win->on_destroyed(reader(s.dialog_seen, s.dialog, dialog_title));
    s.dialog.win->on_finished(
        [&s](rap::dialog_role)
        {
            s.finished = true;
            s.count_at_finish = s.dialog_seen.count;
            s.dialog.win.reset();  // from the backend's own callback
        });
    s.win = make(*s.ui, window_title);
    s.win.win->on_destroyed(reader(s.win_seen, s.win, window_title));
    // The null backend has no close box, nor an on_finished to call.
    if (native)
    {
        native_close(dialog_title);
        native_close(window_title);
    }
    else
    {
        s.dialog.win->close();
        s.dialog.win.reset();
        s.win.win->close();
        s.finished = true;
        s.count_at_finish = 0;
    }
    s.poll = s.ui->timer(
        50, true,
        [&s]
        {
            if (s.stage) return;
            if (!s.finished && ++s.tries < 100) return;
            s.stage = 1;
            expect(s.finished, "the dialog closed from its close box: on_finished came");
            expect(s.count_at_finish == 0, "closed: not fired by the close itself");
            expect(!s.dialog.win, "the dialog destroyed in its on_finished");
            check_seen(s.dialog_seen, "destroyed from on_finished after a close");
            expect(s.win_seen.count == 0, "a window closed from its close box: not fired");
            expect(s.win.win != nullptr, "and still there, hidden");
            s.win.win.reset();
            check_seen(s.win_seen, "closed, then destroyed by the app");
            // A few more turns of the loop: nothing fires again.
            s.ui->post(
                [&s]
                {
                    expect(s.dialog_seen.count == 1 && s.win_seen.count == 1,
                           "nothing fired again afterwards");
                    s.poll = {};
                    finish(s);
                });
        });
}

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options opts;
    opts.backend = backend_name;
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::printf("FAIL %s\n", ui.error().c_str());
        return 2;
    }
    close_state s;
    s.ui = ui->get();
    (*ui)->post(
        [&]
        {
            sync_checks(**ui);
            close_checks(s);
        });
    return (*ui)->run();
}
