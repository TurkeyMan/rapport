// A stack's pages (layout::stack) through a real toolkit, off the desktop:
// a GtkStack or QStackedWidget with a page per id, the hidden ones not
// shown (unmapped); homogeneous, the window's size not changing as another
// page shows, and sized to the page shown when not; the focus moving to the
// page shown when it was in the one hidden (to the control that had it
// there, else the first); a page added later built with its binds; and a
// page removed from one of its own callbacks out of the stack and ids() at
// once, destroyed once the main loop is reached.
//   rapport-stack-test-gtk3   (GDK_BACKEND=broadway, broadwayd running)
//   rapport-stack-test-gtk4   (GDK_BACKEND=broadway, gtk4-broadwayd running)
//   rapport-stack-test-qt6    (QT_QPA_PLATFORM=offscreen)
#include <rapport/rapport.h>

#if defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
#include <gtk/gtk.h>
#elif defined(RAPPORT_TEST_QT6)
#include <QApplication>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#endif

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
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

using ids = std::vector<std::string>;

class empty_model : public rap::list_model
{
public:
    size_t row_count() const override { return 0; }
    std::string text(size_t, size_t) const override { return {}; }
};

#if defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)

#if defined(RAPPORT_TEST_GTK4)
constexpr const char* backend_name = "gtk4";
#else
constexpr const char* backend_name = "gtk3";
#endif

using native_widget = GtkWidget*;

std::vector<GtkWidget*> children(GtkWidget* w)
{
    std::vector<GtkWidget*> out;
#if defined(RAPPORT_TEST_GTK4)
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        out.push_back(c);
#else
    if (GTK_IS_CONTAINER(w))
        gtk_container_forall(
            GTK_CONTAINER(w),
            +[](GtkWidget* c, gpointer p)
            { static_cast<std::vector<GtkWidget*>*>(p)->push_back(c); },
            &out);
#endif
    return out;
}

GtkWidget* find(GtkWidget* w, const std::function<bool(GtkWidget*)>& match)
{
    if (match(w)) return w;
    for (GtkWidget* c : children(w))
        if (GtkWidget* f = find(c, match)) return f;
    return nullptr;
}

std::vector<GtkWidget*> toplevels()
{
    std::vector<GtkWidget*> out;
#if defined(RAPPORT_TEST_GTK4)
    GListModel* tops = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(tops); ++i)
    {
        auto* w = GTK_WIDGET(g_list_model_get_item(tops, i));
        out.push_back(w);
        g_object_unref(w);
    }
#else
    GList* l = gtk_window_list_toplevels();
    for (GList* i = l; i; i = i->next) out.push_back(GTK_WIDGET(i->data));
    g_list_free(l);
#endif
    return out;
}

GtkWidget* window_titled(const char* title)
{
    for (GtkWidget* w : toplevels())
        if (GTK_IS_WINDOW(w) && gtk_window_get_title(GTK_WINDOW(w)) &&
            std::string(gtk_window_get_title(GTK_WINDOW(w))) == title)
            return w;
    return nullptr;
}

GtkWidget* stack_in(GtkWidget* top)
{
    return find(top, [](GtkWidget* w) { return GTK_IS_STACK(w); });
}

GtkWidget* entry_with(GtkWidget* in, const std::string& text)
{
    return find(in,
                [&](GtkWidget* w)
                {
#if defined(RAPPORT_TEST_GTK4)
                    return GTK_IS_ENTRY(w) && text == gtk_editable_get_text(GTK_EDITABLE(w));
#else
                    return GTK_IS_ENTRY(w) && text == gtk_entry_get_text(GTK_ENTRY(w));
#endif
                });
}

GtkWidget* button_with(GtkWidget* in, const std::string& label)
{
    return find(in,
                [&](GtkWidget* w)
                {
                    return GTK_IS_BUTTON(w) && gtk_button_get_label(GTK_BUTTON(w)) &&
                           label == gtk_button_get_label(GTK_BUTTON(w));
                });
}

int page_count(GtkWidget* stack)
{
    return int(children(stack).size());
}

bool shown(GtkWidget* w)
{
    return gtk_widget_get_mapped(w);
}

bool is_visible_page(GtkWidget* stack, GtkWidget* inside)
{
    GtkWidget* v = gtk_stack_get_visible_child(GTK_STACK(stack));
    return v && (v == inside || gtk_widget_is_ancestor(inside, v));
}

GtkWidget* focus_of(GtkWidget* top)
{
#if defined(RAPPORT_TEST_GTK4)
    return gtk_root_get_focus(GTK_ROOT(top));
#else
    return gtk_window_get_focus(GTK_WINDOW(top));
#endif
}

bool focus_in(GtkWidget* top, GtkWidget* w)
{
    GtkWidget* f = focus_of(top);
    return f && (f == w || gtk_widget_is_ancestor(f, w));
}

int natural_height(GtkWidget* w)
{
#if defined(RAPPORT_TEST_GTK4)
    int nat = 0;
    gtk_widget_measure(w, GTK_ORIENTATION_VERTICAL, -1, nullptr, &nat, nullptr, nullptr);
    return nat;
#else
    GtkRequisition nat{};
    gtk_widget_get_preferred_size(w, nullptr, &nat);
    return nat.height;
#endif
}

void click(GtkWidget* b)
{
    g_signal_emit_by_name(b, "clicked");  // (GTK 4 has no gtk_button_clicked)
}

// Destroyed: the button's "destroy" (GTK 3 as it's destroyed, GTK 4 as it's
// disposed).
void watch_destroy(GtkWidget* w, bool* gone)
{
    g_signal_connect(w, "destroy",
                     G_CALLBACK(+[](GtkWidget*, gpointer p) { *static_cast<bool*>(p) = true; }),
                     gone);
}

void activate(GtkWidget*) {}
// No control focused in the window.
void clear_focus(GtkWidget* top)
{
#if defined(RAPPORT_TEST_GTK4)
    gtk_root_set_focus(GTK_ROOT(top), nullptr);
#else
    gtk_window_set_focus(GTK_WINDOW(top), nullptr);
#endif
}

#elif defined(RAPPORT_TEST_QT6)

constexpr const char* backend_name = "qt6";
using native_widget = QWidget*;

QWidget* window_titled(const char* title)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->windowTitle() == QString::fromUtf8(title)) return w;
    return nullptr;
}
QWidget* stack_in(QWidget* top)
{
    return top->findChild<QStackedWidget*>();
}
QWidget* entry_with(QWidget* in, const std::string& text)
{
    for (auto* e : in->findChildren<QLineEdit*>())
        if (e->text() == QString::fromStdString(text)) return e;
    return nullptr;
}
QWidget* button_with(QWidget* in, const std::string& label)
{
    for (auto* b : in->findChildren<QPushButton*>())
        if (b->text().remove('&') == QString::fromStdString(label)) return b;
    return nullptr;
}
int page_count(QWidget* stack)
{
    return static_cast<QStackedWidget*>(stack)->count();
}
bool shown(QWidget* w)
{
    return w->isVisible();
}
bool is_visible_page(QWidget* stack, QWidget* inside)
{
    QWidget* v = static_cast<QStackedWidget*>(stack)->currentWidget();
    return v && (v == inside || v->isAncestorOf(inside));
}
bool focus_in(QWidget* top, QWidget* w)
{
    QWidget* f = top->focusWidget();
    return f && (f == w || w->isAncestorOf(f));
}
int natural_height(QWidget* w)
{
    // As a parent layout takes it: an explicit minimum too.
    return std::max({w->sizeHint().height(), w->minimumSizeHint().height(), w->minimumHeight()});
}
void click(QWidget* b)
{
    static_cast<QPushButton*>(b)->click();
}
void watch_destroy(QWidget* w, bool* gone)
{
    QObject::connect(w, &QObject::destroyed, [gone] { *gone = true; });
}
void activate(QWidget* top)
{
    top->activateWindow();
}
void clear_focus(QWidget* top)
{
    if (QWidget* f = top->focusWidget()) f->clearFocus();
}

#endif

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options opts;
    opts.backend = backend_name;
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::printf("FAIL no %s backend: %s\n", backend_name, ui.error().c_str());
        return 1;
    }
    using namespace rap::layout;
    using namespace rap::literals;

    empty_model model;
    rap::stack* st = nullptr;
    rap::entry *a_entry = nullptr, *b_entry = nullptr;
    auto win = (*ui)->create(
        window{"rapport stack test",
               {stack{{"a", vbox{button{"First"}, entry{"a-entry"}.bind(a_entry)}},
                      {"b", vbox{entry{"b-entry"}.bind(b_entry),
                                 list{{{"Name"}}, &model}.width(400_px).height(300_px)}}}
                    .bind(st)}});
    rap::stack* sized = nullptr;
    auto win2 = (*ui)->create(
        window{"rapport stack test 2",
               {stack{{"small", label{"Small"}},
                      {"large", list{{{"Name"}}, &model}.width(400_px).height(300_px)}}
                    .homogeneous(false)
                    .bind(sized)}});
    if (!st || !sized || !a_entry || !b_entry)
    {
        std::printf("FAIL binds\n");
        return 1;
    }
    win->show();
    win2->show();

    native_widget top = nullptr, stack = nullptr, top2 = nullptr, stack2 = nullptr;
    native_widget edit_a = nullptr, edit_b = nullptr, late_native = nullptr;
    rap::size_f size_before{};
    rap::button* late = nullptr;
    bool clicked = false, late_gone = false;

    std::vector<std::function<void()>> steps;
    size_t step = 0;
    steps.push_back(
        [&]
        {
            expect(page_count(stack) == 2, "a native page per id");
            expect(st->ids() == ids{"a", "b"} && st->shown() == "a", "the first page is shown");
            expect(edit_a && edit_b && is_visible_page(stack, edit_a),
                   "the native stack shows the first page");
            expect(shown(edit_a) && !shown(edit_b), "the hidden page's controls aren't shown");
            expect(natural_height(stack) >= 300,
                   "homogeneous: sized to its largest page from the start");
            expect(natural_height(stack2) < 300, "homogeneous(false): sized to the page shown");
            size_before = win->size();
            activate(top);
            a_entry->focus();
            st->show("b");
            sized->show("large");
        });
    steps.push_back(
        [&]
        {
            expect(st->shown() == "b" && is_visible_page(stack, edit_b), "show: b");
            expect(!shown(edit_a) && shown(edit_b), "show: a's controls hidden, b's shown");
            const rap::size_f now = win->size();
            expect(now.w == size_before.w && now.h == size_before.h,
                   "homogeneous: the window's size doesn't change on show");
            expect(natural_height(stack2) >= 300,
                   "homogeneous(false): sized to the page shown, once it changes");
            expect(focus_in(top, edit_b),
                   "show: the focus moves to the page shown's first control");
            st->show("a");
            expect(focus_in(top, edit_a),
                   "show: back to the control that had it in that page, not its first");
            st->show("b");
            st->add("c",
                    vbox{button{"Late"}.bind(late).on_click(
                        [&]
                        {
                            clicked = true;
                            st->remove("c");
                            expect(st->ids() == ids{"a", "b"},
                                   "removed in its own callback: out of ids() at once");
                            expect(st->shown() == "b",
                                   "the shown page, the last, removed: the previous shows");
                            expect(page_count(stack) == 2 && is_visible_page(stack, edit_b),
                                   "removed in its own callback: out of the native stack at once");
                            expect(!late_gone, "removed in its own callback: not destroyed yet");
                        })});
            expect(late != nullptr && st->ids() == ids{"a", "b", "c"} && st->shown() == "b",
                   "add: built, its binds set, last and hidden");
            late_native = button_with(stack, "Late");
            expect(late_native && page_count(stack) == 3 && !shown(late_native),
                   "add: a native page, hidden");
            st->show("c");
            late->focus();
        });
    steps.push_back(
        [&]
        {
            expect(late_native && shown(late_native) && focus_in(top, late_native),
                   "the page added, shown and focused");
            if (!late_native) return;
            watch_destroy(late_native, &late_gone);
            click(late_native);
            expect(clicked, "its button's callback ran");
            expect(!late_gone, "after the callback returned: not destroyed before the loop runs");
            expect(focus_in(top, edit_b),
                   "the focus, in the page removed, moves to the page shown's");
        });
    steps.push_back(
        [&]
        {
            expect(late_gone, "once the loop has run: the page's widgets are destroyed");
            // A page that's a control itself, and one with a dialog button
            // whose role the window answers (its click connection the page's).
            st->add("d", entry{"d-entry"});
            st->add("e", vbox{buttons{{{rap::dialog_role::cancel}}}});
            activate(top);
            st->show("d");
        });
    steps.push_back(
        [&]
        {
            native_widget edit_d = entry_with(stack, "d-entry");
            expect(edit_d && focus_in(top, edit_d),
                   "show: a page that's a control itself takes the focus");
            clear_focus(top);
            st->focus();
            expect(edit_d && focus_in(top, edit_d),
                   "focus(): a page that's a control itself takes it");
            st->remove("e");
        });
    // The page with the role button destroyed by the loop; the window,
    // destroyed after run(), mustn't reach that button.
    steps.push_back([&] {});

    int waited = 0;
    auto tick = (*ui)->backend().timer(50, true,
                                       [&]
                                       {
                                           if (!top)
                                           {
                                               top = window_titled("rapport stack test");
                                               top2 = window_titled("rapport stack test 2");
                                               if (top) stack = stack_in(top);
                                               if (top2) stack2 = stack_in(top2);
                                               if (stack)
                                               {
                                                   edit_a = entry_with(stack, "a-entry");
                                                   edit_b = entry_with(stack, "b-entry");
                                               }
                                           }
                                           if (!stack || !stack2 || !shown(stack) || !shown(stack2))
                                           {
                                               top = nullptr;
                                               if (++waited > 100)
                                               {
                                                   expect(false, "the windows show");
                                                   (*ui)->quit(1);
                                               }
                                               return;
                                           }
                                           if (step < steps.size())
                                               steps[step++]();
                                           else
                                               (*ui)->quit(fails ? 1 : 0);
                                       });
    (*ui)->run();
    win.reset();  // after a page with a role button was removed
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
