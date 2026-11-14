// window::present: a window never shown is shown; minimised, it's restored;
// minimised from maximised, it's maximised again; closed (hidden), it's shown
// again. With RAPPORT_NO_ACTIVATE (Win32) the focus isn't taken; without it
// the window is restored all the same, and either has the focus or its
// taskbar button flashes (Windows decides; reported, not checked).
//   rapport-present-test-win32  (shows and minimises windows: only on a CI
//                                runner, GITHUB_ACTIONS=true and
//                                RAPPORT_TEST_SEND_INPUT=1; else skipped, 77)
//   rapport-present-test-gtk3   (Broadway: shown and hidden only; Broadway's
//   rapport-present-test-gtk4    windows can't be minimised)
//   rapport-present-test-qt6    (offscreen)
#include <rapport/rapport.h>

#if defined(RAPPORT_TEST_WIN32)
#include <windows.h>
#elif defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
#include <gtk/gtk.h>
#elif defined(RAPPORT_TEST_QT6)
#include <QApplication>
#include <QWidget>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
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

#if defined(RAPPORT_TEST_WIN32)
bool env_is(const char* name, const char* value)
{
    const char* v = std::getenv(name);
    return v && !std::strcmp(v, value);
}
#endif

constexpr const char* title = "present test";
const bool quiet = std::getenv("RAPPORT_NO_ACTIVATE") != nullptr;

// The toolkit's window: whether it's shown, minimised, maximised, and
// minimising or maximising it as the user would.
#if defined(RAPPORT_TEST_WIN32)
constexpr const char* backend_name = "win32";
constexpr bool can_minimize = true;
HWND find_window()
{
    struct q
    {
        HWND found = nullptr;
    } s;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM p) -> BOOL
        {
            wchar_t t[64] = {};
            GetWindowTextW(h, t, 64);
            if (!std::wcscmp(t, L"present test")) reinterpret_cast<q*>(p)->found = h;
            return TRUE;
        },
        LPARAM(&s));
    return s.found;
}
bool visible() { HWND h = find_window(); return h && IsWindowVisible(h); }
bool minimized() { HWND h = find_window(); return h && IsIconic(h); }
bool maximized() { HWND h = find_window(); return h && IsZoomed(h); }
// As the minimise box does: the next window in the z-order activated.
void minimize() { ShowWindow(find_window(), SW_MINIMIZE); }
void maximize() { ShowWindow(find_window(), SW_SHOWMAXIMIZED); }
bool focused() { HWND h = find_window(); return h && GetForegroundWindow() == h; }
#elif defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
#if defined(RAPPORT_TEST_GTK3)
constexpr const char* backend_name = "gtk3";
#else
constexpr const char* backend_name = "gtk4";
#endif
constexpr bool can_minimize = false;
GtkWidget* find_window()
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
        if (const char* t = gtk_window_get_title(GTK_WINDOW(w)); t && !std::strcmp(t, title)) found = w;
        g_object_unref(w);
    }
#endif
    return found;
}
bool visible() { GtkWidget* w = find_window(); return w && gtk_widget_get_visible(w); }
bool minimized() { return false; }
bool maximized() { return false; }
void minimize() {}
void maximize() {}
bool focused() { return false; }
#elif defined(RAPPORT_TEST_QT6)
constexpr const char* backend_name = "qt6";
constexpr bool can_minimize = true;
QWidget* find_window()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->windowTitle() == QString::fromUtf8(title)) return w;
    return nullptr;
}
bool visible() { QWidget* w = find_window(); return w && w->isVisible(); }
bool minimized() { QWidget* w = find_window(); return w && w->isMinimized(); }
bool maximized() { QWidget* w = find_window(); return w && w->isMaximized(); }
void minimize() { find_window()->setWindowState(find_window()->windowState() | Qt::WindowMinimized); }
void maximize() { find_window()->setWindowState(Qt::WindowMaximized); }
bool focused() { QWidget* w = find_window(); return w && w->isActiveWindow(); }
#endif

// Steps run in turn, each waiting (up to 5 s) for what it expects.
struct step
{
    std::string what;
    std::function<void()> act;
    std::function<bool()> done;
};

struct runner
{
    rap::ui* ui = nullptr;
    std::unique_ptr<rap::window> win;
    std::vector<step> steps;
    size_t at = 0;
    int tries = -1;
    rap::subscription poll;

    void tick()
    {
        if (at == steps.size())
        {
            poll = {};
            win.reset();
            ui->quit(fails ? 1 : 0);
            return;
        }
        step& s = steps[at];
        if (tries < 0)
        {
            s.act();
            tries = 0;
        }
        if (!s.done() && ++tries < 100) return;
        expect(s.done(), s.what);
        ++at;
        tries = -1;
    }
};

}  // namespace

int main(int argc, char** argv)
{
#if defined(RAPPORT_TEST_WIN32)
    if (!env_is("GITHUB_ACTIONS", "true") || !env_is("RAPPORT_TEST_SEND_INPUT", "1"))
    {
        std::printf("skipped: shows and minimises windows, only on a CI runner "
                    "(GITHUB_ACTIONS=true, RAPPORT_TEST_SEND_INPUT=1)\n");
        return 77;
    }
#endif
    rap::load_options opts;
    opts.backend = backend_name;
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::printf("FAIL %s\n", ui.error().c_str());
        return 2;
    }
    runner r;
    r.ui = ui->get();
    using namespace rap::layout;
    r.win = r.ui->create(rap::layout::window{title, {rap::layout::label{"presented"}}}.size(
        rap::extent{30, rap::extent::unit::chars}, 6_lines));
    auto present = [&r] { r.win->present(); };

    r.steps.push_back({"never shown: present() shows it", present, [] { return visible(); }});
    if (can_minimize)
    {
        r.steps.push_back({"minimised", [] { minimize(); }, [] { return minimized(); }});
        // Whether it had the focus just before: with RAPPORT_NO_ACTIVATE,
        // present() mayn't give it the focus it didn't have.
        static bool had_focus = false;
        r.steps.push_back({"minimised: present() restores it",
                           [&r]
                           {
                               had_focus = focused();
                               std::printf("     the foreground window before: %s\n",
                                           had_focus ? "this one (the focus check is vacuous)" : "another");
                               r.win->present();
                           },
                           [] { return visible() && !minimized(); }});
#if defined(RAPPORT_TEST_WIN32)
        if (quiet)
            r.steps.push_back({"RAPPORT_NO_ACTIVATE: restored without taking the focus", [] {},
                               [] { return had_focus || !focused(); }});
        else
            r.steps.push_back({"activating: restored",
                               []
                               {
                                   std::printf("     the foreground window: %s\n",
                                               focused() ? "this one" : "another (the button flashed)");
                               },
                               [] { return !minimized(); }});
#endif
        r.steps.push_back({"maximised", [] { maximize(); }, [] { return maximized(); }});
        r.steps.push_back({"maximised, then minimised", [] { minimize(); }, [] { return minimized(); }});
        r.steps.push_back({"minimised from maximised: present() maximises it again", present,
                           [] { return visible() && !minimized() && maximized(); }});
    }
    r.steps.push_back({"closed: hidden", [&r] { r.win->close(); }, [] { return !visible(); }});
    r.steps.push_back({"closed: present() shows it again", present, [] { return visible(); }});
    r.steps.push_back({"presented again while shown: still shown", present,
                       [] { return visible() && !minimized(); }});

    r.poll = r.ui->timer(50, true, [&r] { r.tick(); });
    return (*ui)->run();
}
