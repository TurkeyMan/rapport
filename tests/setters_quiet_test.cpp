// The app's setters don't fire on_changed, on every backend: an entry's
// set_text, a drop-down's set_items and set_current, a combo's set_items and
// set_text. The user's change still does: text inserted into the entry
// through the toolkit, as typing does.
//   rapport-setters-quiet-test         (null: the contract, nothing native)
//   rapport-setters-quiet-test-win32   (a window never shown)
//   rapport-setters-quiet-test-gtk3    (Broadway)
//   rapport-setters-quiet-test-gtk4    (Broadway)
//   rapport-setters-quiet-test-qt6     (offscreen)
#include <rapport/rapport.h>

#if defined(RAPPORT_TEST_WIN32)
#include <windows.h>
#elif defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
#include <gtk/gtk.h>
#elif defined(RAPPORT_TEST_QT6)
#include <QApplication>
#include <QLineEdit>
#include <QWidget>
#endif

#include <cstdio>
#include <cstring>
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

constexpr const char* entry_text = "the entry";
constexpr const char* title = "setters quiet";

// The user's typing: " typed" inserted at the end of the entry showing
// `text`, through the toolkit (false: no such entry found). The null
// backend has nothing to type into.
#if defined(RAPPORT_TEST_WIN32)
constexpr const char* backend_name = "win32";
bool type_into(const char* text)
{
    struct q
    {
        std::wstring want;
        HWND found = nullptr;
    } s{std::wstring(text, text + std::strlen(text))};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND top, LPARAM p) -> BOOL
        {
            EnumChildWindows(
                top,
                [](HWND h, LPARAM p) -> BOOL
                {
                    auto* s = reinterpret_cast<q*>(p);
                    wchar_t cls[16] = {}, t[64] = {};
                    GetClassNameW(h, cls, 16);
                    GetWindowTextW(h, t, 64);
                    if (!_wcsicmp(cls, L"Edit") && s->want == t) s->found = h;
                    return TRUE;
                },
                p);
            return TRUE;
        },
        LPARAM(&s));
    if (!s.found) return false;
    SendMessageW(s.found, EM_SETSEL, WPARAM(-1), LPARAM(-1));
    SendMessageW(s.found, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L" typed"));
    return true;
}
#elif defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
#if defined(RAPPORT_TEST_GTK3)
constexpr const char* backend_name = "gtk3";
#else
constexpr const char* backend_name = "gtk4";
#endif
GtkWidget* find_entry(GtkWidget* w, const char* text)
{
    if (GTK_IS_ENTRY(w) && !std::strcmp(gtk_editable_get_text(GTK_EDITABLE(w)), text)) return w;
#if defined(RAPPORT_TEST_GTK3)
    if (!GTK_IS_CONTAINER(w)) return nullptr;
    GList* kids = gtk_container_get_children(GTK_CONTAINER(w));
    GtkWidget* found = nullptr;
    for (GList* l = kids; l && !found; l = l->next) found = find_entry(GTK_WIDGET(l->data), text);
    g_list_free(kids);
    return found;
#else
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* f = find_entry(c, text)) return f;
    return nullptr;
#endif
}
bool type_into(const char* text)
{
    GtkWidget* found = nullptr;
#if defined(RAPPORT_TEST_GTK3)
    GList* all = gtk_window_list_toplevels();
    for (GList* l = all; l && !found; l = l->next) found = find_entry(GTK_WIDGET(l->data), text);
    g_list_free(all);
#else
    GListModel* all = gtk_window_get_toplevels();
    for (guint i = 0, n = g_list_model_get_n_items(all); i < n && !found; ++i)
    {
        auto* w = static_cast<GtkWidget*>(g_list_model_get_item(all, i));
        found = find_entry(w, text);
        g_object_unref(w);
    }
#endif
    if (!found) return false;
    int at = -1;
    gtk_editable_insert_text(GTK_EDITABLE(found), " typed", -1, &at);
    return true;
}
#elif defined(RAPPORT_TEST_QT6)
constexpr const char* backend_name = "qt6";
bool type_into(const char* text)
{
    for (QWidget* top : QApplication::topLevelWidgets())
        for (QLineEdit* e : top->findChildren<QLineEdit*>())
            if (e->text() == QString::fromUtf8(text))
            {
                e->end(false);
                e->insert(QStringLiteral(" typed"));
                return true;
            }
    return false;
}
#else
constexpr const char* backend_name = "null";
bool type_into(const char*) { return false; }
#endif
constexpr bool native = std::strcmp(backend_name, "null") != 0;

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
    rap::entry* entry = nullptr;
    rap::drop_down* drop = nullptr;
    rap::combo* combo = nullptr;
    using namespace rap::layout;
    auto win = (*ui)->create(
        rap::layout::window{title,
                            {rap::layout::entry{entry_text}.bind(entry),
                             rap::layout::drop_down{{"one", "two", "three"}}.bind(drop),
                             rap::layout::combo{{"red", "green"}, "red"}.bind(combo)}}
            .size(rap::extent{30, rap::extent::unit::chars}, 8_lines));
    int entry_fired = 0, drop_fired = 0, combo_fired = 0;
    std::string entry_seen;
    entry->on_changed(
        [&](const std::string& t)
        {
            ++entry_fired;
            entry_seen = t;
        });
    drop->on_changed([&](int) { ++drop_fired; });
    combo->on_changed([&](const std::string&) { ++combo_fired; });
#if !defined(RAPPORT_TEST_WIN32)
    if (native) win->show();
#endif

    (*ui)->post(
        [&]
        {
            entry->set_text("set by the app");
            expect(entry->text() == "set by the app", "entry: set_text took");
            expect(entry_fired == 0, "entry: set_text didn't fire on_changed (" + std::to_string(entry_fired) + ")");

            drop->set_current(2);
            expect(drop->current() == 2, "drop-down: set_current took");
            expect(drop_fired == 0, "drop-down: set_current didn't fire on_changed");
            drop->set_items({"four", "five"});
            expect(drop_fired == 0, "drop-down: set_items didn't fire on_changed");
            drop->set_current(1);
            expect(drop_fired == 0, "drop-down: set_current after set_items didn't fire on_changed");

            combo->set_text("blue");
            expect(combo->text() == "blue", "combo: set_text took");
            expect(combo_fired == 0, "combo: set_text didn't fire on_changed");
            combo->set_items({"cyan", "magenta"});
            expect(combo->text() == "blue", "combo: set_items kept its text");
            expect(combo_fired == 0, "combo: set_items didn't fire on_changed");

            if (native)
            {
                // Back to text the test can find it by, quietly, then typed.
                entry->set_text(entry_text);
                expect(type_into(entry_text), "entry: found to type into");
                expect(entry_fired >= 1, "entry: the user's typing fired on_changed");
                expect(entry_seen == std::string(entry_text) + " typed",
                       "entry: on_changed saw the typed text (\"" + entry_seen + "\")");
            }
            win.reset();
            (*ui)->quit(fails ? 1 : 0);
        });
    return (*ui)->run();
}
