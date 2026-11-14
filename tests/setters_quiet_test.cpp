// The app's setters don't fire on_changed, on every backend: an entry's
// set_text (a plain one, and one with actions: GTK 4 builds that another
// way), a drop-down's set_items and set_current, a combo's set_items and
// set_text. The user's changes still do, through the toolkit as the user's
// come: text typed into an entry, the clear button (where the test can press
// it: GTK 3, Qt), a drop-down's choice. A setter called from on_changed
// itself doesn't make it fire again, once the loop's run.
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
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QWidget>
#endif

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

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

constexpr const char* plain_text = "the plain entry";
constexpr const char* action_text = "the entry with actions";
constexpr const char* title = "setters quiet";

// The user, through the toolkit: " typed" inserted at the end of the entry
// showing `text`; that entry's clear button pressed (false where the test
// can't press it); the drop-down's item `i` chosen. Each false if the
// control isn't found. The null backend has nothing native.
#if defined(RAPPORT_TEST_WIN32)
constexpr const char* backend_name = "win32";
HWND find_child(const std::function<bool(HWND)>& want)
{
    struct q
    {
        const std::function<bool(HWND)>* want;
        HWND found = nullptr;
    } s{&want};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND top, LPARAM p) -> BOOL
        {
            EnumChildWindows(
                top,
                [](HWND h, LPARAM p) -> BOOL
                {
                    auto* s = reinterpret_cast<q*>(p);
                    if (!s->found && (*s->want)(h)) s->found = h;
                    return TRUE;
                },
                p);
            return TRUE;
        },
        LPARAM(&s));
    return s.found;
}
bool class_is(HWND h, const wchar_t* name)
{
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    return !_wcsicmp(cls, name);
}
HWND find_edit(const char* text)
{
    const std::wstring want(text, text + std::strlen(text));
    return find_child(
        [&](HWND h)
        {
            wchar_t t[64] = {};
            GetWindowTextW(h, t, 64);
            return class_is(h, L"Edit") && want == t;
        });
}
bool type_into(const char* text)
{
    HWND e = find_edit(text);
    if (!e) return false;
    SendMessageW(e, EM_SETSEL, WPARAM(-1), LPARAM(-1));
    SendMessageW(e, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L" typed"));
    return true;
}
bool press_clear(const char*) { return false; }
bool choose(int i)
{
    // The drop-down list (CBS_DROPDOWNLIST), as the user's choice comes:
    // dropped (filled), the item selected, CBN_SELCHANGE to its parent.
    HWND c = find_child([](HWND h)
                        { return class_is(h, L"ComboBox") && (GetWindowLongPtrW(h, GWL_STYLE) & 3) == CBS_DROPDOWNLIST; });
    if (!c) return false;
    const WPARAM id = WPARAM(GetDlgCtrlID(c));
    SendMessageW(GetParent(c), WM_COMMAND, MAKEWPARAM(id, CBN_DROPDOWN), reinterpret_cast<LPARAM>(c));
    SendMessageW(c, CB_SETCURSEL, WPARAM(i), 0);
    SendMessageW(GetParent(c), WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(c));
    return true;
}
#elif defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
#if defined(RAPPORT_TEST_GTK3)
constexpr const char* backend_name = "gtk3";
#else
constexpr const char* backend_name = "gtk4";
#endif
GtkWidget* find(GtkWidget* w, const std::function<bool(GtkWidget*)>& want)
{
    if (want(w)) return w;
#if defined(RAPPORT_TEST_GTK3)
    if (!GTK_IS_CONTAINER(w)) return nullptr;
    GList* kids = gtk_container_get_children(GTK_CONTAINER(w));
    GtkWidget* found = nullptr;
    for (GList* l = kids; l && !found; l = l->next) found = find(GTK_WIDGET(l->data), want);
    g_list_free(kids);
    return found;
#else
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* f = find(c, want)) return f;
    return nullptr;
#endif
}
GtkWidget* find_any(const std::function<bool(GtkWidget*)>& want)
{
    GtkWidget* found = nullptr;
#if defined(RAPPORT_TEST_GTK3)
    GList* all = gtk_window_list_toplevels();
    for (GList* l = all; l && !found; l = l->next) found = find(GTK_WIDGET(l->data), want);
    g_list_free(all);
#else
    GListModel* all = gtk_window_get_toplevels();
    for (guint i = 0, n = g_list_model_get_n_items(all); i < n && !found; ++i)
    {
        auto* w = static_cast<GtkWidget*>(g_list_model_get_item(all, i));
        found = find(w, want);
        g_object_unref(w);
    }
#endif
    return found;
}
GtkWidget* find_entry(const char* text)
{
    return find_any(
        [&](GtkWidget* w)
        {
#if defined(RAPPORT_TEST_GTK3)
            return GTK_IS_ENTRY(w) && !std::strcmp(gtk_entry_get_text(GTK_ENTRY(w)), text);
#else
            // An entry with actions is a GtkText inside a box.
            return (GTK_IS_ENTRY(w) || GTK_IS_TEXT(w)) && !std::strcmp(gtk_editable_get_text(GTK_EDITABLE(w)), text);
#endif
        });
}
bool type_into(const char* text)
{
    GtkWidget* e = find_entry(text);
    if (!e) return false;
    int at = -1;
    gtk_editable_insert_text(GTK_EDITABLE(e), " typed", -1, &at);
    return true;
}
bool press_clear(const char* text)
{
#if defined(RAPPORT_TEST_GTK3)
    // The clear icon's press, as a click on it gives.
    GtkWidget* e = find_entry(text);
    if (!e) return false;
    g_signal_emit_by_name(e, "icon-press", GTK_ENTRY_ICON_SECONDARY, static_cast<GdkEvent*>(nullptr));
    return true;
#else
    (void)text;
    return false;
#endif
}
bool choose(int i)
{
#if defined(RAPPORT_TEST_GTK3)
    GtkWidget* d = find_any([](GtkWidget* w)
                            { return GTK_IS_COMBO_BOX(w) && !gtk_combo_box_get_has_entry(GTK_COMBO_BOX(w)); });
    if (!d) return false;
    gtk_combo_box_set_active(GTK_COMBO_BOX(d), i);
#else
    GtkWidget* d = find_any([](GtkWidget* w) { return GTK_IS_DROP_DOWN(w); });
    if (!d) return false;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d), guint(i));
#endif
    return true;
}
#elif defined(RAPPORT_TEST_QT6)
constexpr const char* backend_name = "qt6";
QLineEdit* find_edit(const char* text)
{
    for (QWidget* top : QApplication::topLevelWidgets())
        for (QLineEdit* e : top->findChildren<QLineEdit*>())
            if (e->text() == QString::fromUtf8(text)) return e;
    return nullptr;
}
bool type_into(const char* text)
{
    QLineEdit* e = find_edit(text);
    if (!e) return false;
    e->end(false);
    e->insert(QStringLiteral(" typed"));
    return true;
}
bool press_clear(const char* text)
{
    // Qt's own clear button is this action of the line edit's.
    QLineEdit* e = find_edit(text);
    if (!e) return false;
    for (QAction* a : e->findChildren<QAction*>())
        if (a->objectName() == QLatin1String("_q_qlineeditclearaction"))
        {
            a->trigger();
            return true;
        }
    return false;
}
bool choose(int i)
{
    for (QWidget* top : QApplication::topLevelWidgets())
        for (QComboBox* c : top->findChildren<QComboBox*>())
            if (!c->isEditable())
            {
                c->setCurrentIndex(i);
                return true;
            }
    return false;
}
#else
constexpr const char* backend_name = "null";
bool type_into(const char*) { return false; }
bool press_clear(const char*) { return false; }
bool choose(int) { return false; }
#endif
const bool native = std::string_view(backend_name) != "null";

struct counts
{
    int plain = 0, action = 0, drop = 0, combo = 0;
    std::string plain_seen, action_seen;
    int drop_seen = -1;
};

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
    rap::entry *plain = nullptr, *action = nullptr;
    rap::drop_down* drop = nullptr;
    rap::combo* combo = nullptr;
    using namespace rap::layout;
    auto win = (*ui)->create(
        rap::layout::window{
            title,
            {rap::layout::entry{plain_text}.bind(plain),
             rap::layout::entry{action_text}
                 .clearable()
                 .trailing_action(rap::icon::named("edit-find"), "Find", [] {})
                 .bind(action),
             rap::layout::drop_down{{"one", "two", "three"}}.bind(drop),
             rap::layout::combo{{"red", "green"}, "red"}.bind(combo)}}
            .size(rap::extent{30, rap::extent::unit::chars}, 10_lines));
    counts n;
    bool reenter = false;
    plain->on_changed(
        [&](const std::string& t)
        {
            ++n.plain;
            n.plain_seen = t;
        });
    action->on_changed(
        [&](const std::string& t)
        {
            ++n.action;
            n.action_seen = t;
        });
    drop->on_changed(
        [&](int i)
        {
            ++n.drop;
            n.drop_seen = i;
            // The app answering the user's choice by choosing another.
            if (reenter)
            {
                reenter = false;
                drop->set_current(0);
            }
        });
    combo->on_changed([&](const std::string&) { ++n.combo; });
#if !defined(RAPPORT_TEST_WIN32)
    if (native) win->show();
#endif

    (*ui)->post(
        [&]
        {
            plain->set_text("set by the app");
            expect(plain->text() == "set by the app", "entry: set_text took");
            expect(n.plain == 0, "entry: set_text didn't fire on_changed (" + std::to_string(n.plain) + ")");
            action->set_text("also set by the app");
            expect(action->text() == "also set by the app", "entry with actions: set_text took");
            expect(n.action == 0, "entry with actions: set_text didn't fire on_changed");

            drop->set_current(2);
            expect(drop->current() == 2, "drop-down: set_current took");
            expect(n.drop == 0, "drop-down: set_current didn't fire on_changed");
            drop->set_items({"four", "five", "six"});
            expect(n.drop == 0, "drop-down: set_items didn't fire on_changed");
            drop->set_current(1);
            expect(n.drop == 0, "drop-down: set_current after set_items didn't fire on_changed");

            combo->set_text("blue");
            expect(combo->text() == "blue", "combo: set_text took");
            expect(n.combo == 0, "combo: set_text didn't fire on_changed");
            combo->set_items({"cyan", "magenta"});
            expect(combo->text() == "blue", "combo: set_items kept its text");
            expect(n.combo == 0, "combo: set_items didn't fire on_changed");

            if (!native)
            {
                win.reset();
                (*ui)->quit(fails ? 1 : 0);
                return;
            }
            // The user's changes: back to text the test finds them by,
            // quietly, then typed.
            plain->set_text(plain_text);
            action->set_text(action_text);
            expect(type_into(plain_text), "entry: found to type into");
            expect(n.plain >= 1 && n.plain_seen == std::string(plain_text) + " typed",
                   "entry: the user's typing fired on_changed (\"" + n.plain_seen + "\")");
            expect(type_into(action_text), "entry with actions: found to type into");
            expect(n.action >= 1 && n.action_seen == std::string(action_text) + " typed",
                   "entry with actions: the user's typing fired on_changed (\"" + n.action_seen + "\")");
            const int before_clear = n.action;
            if (press_clear((std::string(action_text) + " typed").c_str()))
                expect(n.action > before_clear && n.action_seen.empty(),
                       "entry: the clear button (the user's) fired on_changed with \"\"");
            else
                std::printf("     (the clear button: not pressable from this test here)\n");

            expect(choose(2), "drop-down: found to choose in");
            expect(n.drop == 1 && n.drop_seen == 2, "drop-down: the user's choice fired on_changed (2)");
            // Chosen again, the app's on_changed setting another: heard once.
            reenter = true;
            n.drop = 0;
            expect(choose(1), "drop-down: chosen again");
            (*ui)->post(
                [&]
                {
                    (*ui)->post(
                        [&]
                        {
                            expect(n.drop == 1 && n.drop_seen == 1,
                                   "drop-down: a set_current from on_changed fired nothing more, the loop run (" +
                                       std::to_string(n.drop) + " calls, last " + std::to_string(n.drop_seen) + ")");
                            expect(drop->current() == 0, "drop-down: and it took");
                            win.reset();
                            (*ui)->quit(fails ? 1 : 0);
                        });
                });
        });
    return (*ui)->run();
}
