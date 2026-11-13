// Several windows in one process: ui::save_snapshot falls back on the window
// shown most recently that's still there, or saves the one it's given; and
// window::begin_busy is counted and each window's own, gone with it.
//   rapport-busy-snapshot-test         (the null backend: the counts only)
//   rapport-busy-snapshot-test-win32   (windows never shown; the pointer
//                                       each gives in WM_SETCURSOR)
//   rapport-busy-snapshot-test-gtk3    (offscreen.sh gtk3)
//   rapport-busy-snapshot-test-gtk4    (offscreen.sh gtk4)
//   rapport-busy-snapshot-test-qt6     (offscreen.sh qt6)
#include <rapport/rapport.h>

#if defined(RAPPORT_TEST_WIN32)
#include <windows.h>
#elif defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
#include <gtk/gtk.h>
#elif defined(RAPPORT_TEST_QT6)
#include <QApplication>
#include <QWidget>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

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
constexpr bool shows = true;
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

// Whether the toolkit's own window titled `title` shows the busy pointer;
// true for none such (the null backend has none to look at).
#if defined(RAPPORT_TEST_WIN32)
HWND find_window(const char* title)
{
    struct q
    {
        std::wstring title;
        HWND found = nullptr;
    } s{std::wstring(title, title + std::strlen(title))};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM p) -> BOOL
        {
            auto* s = reinterpret_cast<q*>(p);
            wchar_t t[64] = {};
            GetWindowTextW(h, t, 64);
            if (s->title == t) s->found = h;
            return TRUE;
        },
        LPARAM(&s));
    return s.found;
}
// The pointer the window gives over its client area (as WM_SETCURSOR asks
// it), checked on this thread's own cursor state.
bool shows_busy(HWND h)
{
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    SendMessageW(h, WM_SETCURSOR, WPARAM(h), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    return GetCursor() == LoadCursor(nullptr, IDC_APPSTARTING);
}
bool native_busy(const char* title)
{
    HWND h = find_window(title);
    return h && shows_busy(h);
}
// Any of this thread's top-level windows giving the busy pointer.
bool any_native_busy()
{
    bool any = false;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM p) -> BOOL
        {
            if (shows_busy(h)) *reinterpret_cast<bool*>(p) = true;
            return TRUE;
        },
        LPARAM(&any));
    return any;
}
#elif defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
std::vector<GtkWidget*> toplevels()
{
    std::vector<GtkWidget*> out;
#if defined(RAPPORT_TEST_GTK3)
    GList* all = gtk_window_list_toplevels();
    for (GList* l = all; l; l = l->next) out.push_back(GTK_WIDGET(l->data));
    g_list_free(all);
#else
    GListModel* all = gtk_window_get_toplevels();
    for (guint i = 0, n = g_list_model_get_n_items(all); i < n; ++i)
    {
        auto* w = static_cast<GtkWidget*>(g_list_model_get_item(all, i));
        out.push_back(w);
        g_object_unref(w);
    }
#endif
    return out;
}
bool shows_busy(GtkWidget* w)
{
#if defined(RAPPORT_TEST_GTK3)
    GdkWindow* g = gtk_widget_get_window(w);
    return g && gdk_window_get_cursor(g);
#else
    GdkCursor* c = gtk_widget_get_cursor(w);
    return c && gdk_cursor_get_name(c) && !std::strcmp(gdk_cursor_get_name(c), "progress");
#endif
}
bool native_busy(const char* title)
{
    for (GtkWidget* w : toplevels())
        if (const char* t = gtk_window_get_title(GTK_WINDOW(w)); t && !std::strcmp(t, title))
            return shows_busy(w);
    return false;
}
bool any_native_busy()
{
    for (GtkWidget* w : toplevels())
        if (shows_busy(w)) return true;
    return false;
}
#elif defined(RAPPORT_TEST_QT6)
bool shows_busy(QWidget* w)
{
    return w->testAttribute(Qt::WA_SetCursor) && w->cursor().shape() == Qt::BusyCursor;
}
bool native_busy(const char* title)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->windowTitle() == QString::fromUtf8(title)) return shows_busy(w);
    return false;
}
bool any_native_busy()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (shows_busy(w)) return true;
    return false;
}
#else
bool native_busy(const char*) { return true; }
bool any_native_busy() { return false; }
#endif

// The window not showing the busy pointer (true where there's none to see).
bool native_idle(const char* title) { return !native || !native_busy(title); }

// A PNG's width, from its header; 0 if it can't be read.
uint32_t png_width(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    unsigned char h[24] = {};
    if (!f.read(reinterpret_cast<char*>(h), sizeof h)) return 0;
    if (std::memcmp(h + 12, "IHDR", 4)) return 0;
    return uint32_t(h[16]) << 24 | uint32_t(h[17]) << 16 | uint32_t(h[18]) << 8 | h[19];
}

struct run_state
{
    rap::ui* ui = nullptr;
    std::unique_ptr<rap::window> a, b;
    std::filesystem::path dir;
    rap::subscription poll;
    int tries = 0;
    bool done = false;
};

// A saved snapshot's width, 0 for none.
uint32_t save(run_state& s, rap::window* w, const char* name)
{
    const auto p = s.dir / name;
    std::filesystem::remove(p);
    const bool ok = w ? s.ui->save_snapshot(*w, p.string()) : s.ui->save_snapshot(p.string());
    return ok ? png_width(p) : 0;
}

void busy_checks(run_state& s)
{
    rap::window& a = *s.a;
    rap::window& b = *s.b;
    expect(!a.busy() && !b.busy() && !any_native_busy(), "nothing busy to start with");

    a.begin_busy();
    b.begin_busy();
    expect(a.busy() && b.busy(), "both windows busy");
    expect(native_busy("busy a") && native_busy("busy b"), "both show the busy pointer");

    // Nested on one window: counted.
    a.begin_busy();
    a.end_busy();
    expect(a.busy() && native_busy("busy a"), "a, begun twice and ended once, still busy");

    // b closed while it loads: a stays busy.
    s.b.reset();
    expect(a.busy() && native_busy("busy a"), "b destroyed while busy: a still busy");

    a.end_busy();
    expect(!a.busy() && native_idle("busy a"), "a's load ends: a not busy");
    expect(!any_native_busy(), "and nothing is left busy");

    // An end without a begin isn't counted against the next begin.
    a.end_busy();
    a.begin_busy();
    expect(a.busy() && native_busy("busy a"), "a stray end_busy: the next begin_busy still shows");
    a.end_busy();

    // The app-wide ui::set_busy alongside: a window's own count outlasts it.
    a.begin_busy();
    s.ui->set_busy(true);
    s.ui->set_busy(false);
    expect(a.busy() && native_busy("busy a"), "ui::set_busy(false) leaves a window's own busy");
    a.end_busy();
    expect(!any_native_busy(), "and once it ends, nothing busy");
}

// Both windows created (and shown, where they may be); b shown last.
void snapshot_checks(run_state& s)
{
    if (!native)
    {
        expect(!s.ui->save_snapshot((s.dir / "none.png").string()),
               "the null backend saves nothing");
        return;
    }
    const uint32_t wa = save(s, s.a.get(), "a.png");
    const uint32_t wb = save(s, s.b.get(), "b.png");
    expect(wa && wb && wa != wb, "each window, given, saved at its own width (" +
                                     std::to_string(wa) + ", " + std::to_string(wb) + ")");
    // Shown: b, shown last. Never shown (win32): a, the oldest.
    rap::window* first = shows ? s.b.get() : s.a.get();
    const uint32_t wfirst = first == s.a.get() ? wa : wb;
    expect(save(s, nullptr, "last.png") == wfirst,
           shows ? "no window given: b, shown last" : "no window given, none shown: a, the oldest");
    // That one destroyed: the other, still there.
    const uint32_t wother = first == s.a.get() ? wb : wa;
    (first == s.a.get() ? s.a : s.b).reset();
    expect(save(s, nullptr, "fallback.png") == wother,
           "that window destroyed: the snapshot is the other's");
    rap::window* live = s.a ? s.a.get() : s.b.get();
    expect(save(s, live, "given.png") == wother, "the one still there, given, saved");
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
    run_state s;
    s.ui = ui->get();
    s.dir = std::filesystem::temp_directory_path() / "rapport-busy-snapshot-test";
    std::filesystem::create_directories(s.dir);

    using namespace rap::layout;
    auto make = [&](const char* title, int chars)
    {
        const rap::extent width{float(chars), rap::extent::unit::chars};
        return (*ui)->create(window{title, {canvas{}.stretch()}}.size(width, 8_lines));
    };

    // The pointer first, on windows of their own.
    s.a = make("busy a", 30);
    s.b = make("busy b", 20);
    if (shows)
    {
        s.a->show();
        s.b->show();
    }
    (*ui)->post(
        [&]
        {
            busy_checks(s);
            s.a.reset();
            s.b.reset();
            // Then the snapshots.
            s.a = make("snapshot a", 40);
            s.b = make("snapshot b", 20);
            if (shows)
            {
                s.a->show();
                s.b->show();
            }
            // Drawn before they're saved (GTK 4 renders what it last drew).
            s.poll = (*ui)->timer(100, true,
                                  [&]
                                  {
                                      if (s.done) return;
                                      const bool drawn = !native || save(s, s.a.get(), "a.png");
                                      if (!drawn && ++s.tries < 50) return;
                                      s.done = true;
                                      snapshot_checks(s);
                                      s.a.reset();
                                      s.b.reset();
                                      std::printf(fails ? "%d failed\n" : "all passed\n", fails);
                                      std::fflush(stdout);
                                      (*ui)->quit(fails ? 1 : 0);
                                  });
        });
    return (*ui)->run();
}
