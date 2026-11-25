// Link labels (label::link) through a real toolkit, off the desktop: an
// ellipsized link measures as an ellipsized label does (a small minimum, its
// whole text's natural width), shows its text cut short when it's given less
// (and still runs its click), and has its whole text for its tooltip and
// accessible name; focusable(false) takes it out of the Tab order and it
// still clicks; and a link that's only an icon is the icon alone, no text
// widget, no taller than a line of text, clicked with the pointer's hand,
// named by its tooltip.
//   rapport-link-label-test-gtk3   (GDK_BACKEND=broadway, broadwayd running)
//   rapport-link-label-test-gtk4   (GDK_BACKEND=broadway, gtk4-broadwayd running)
//   rapport-link-label-test-qt6    (QT_QPA_PLATFORM=offscreen)
#include <rapport/rapport.h>

#if defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
#include <gtk/gtk.h>
#elif defined(RAPPORT_TEST_QT6)
#include <QApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
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

const std::string long_text =
    "/home/someone/Pictures/a folder with a rather long name/and another one inside it/"
    "the picture with the longest name of them all.png";
const std::string copy_tip = "Copy the path";

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
            +[](GtkWidget* c, gpointer p) { static_cast<std::vector<GtkWidget*>*>(p)->push_back(c); },
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

GtkWidget* window_titled(const char* title)
{
#if defined(RAPPORT_TEST_GTK4)
    GListModel* tops = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(tops); ++i)
    {
        auto* w = GTK_WIDGET(g_list_model_get_item(tops, i));
        g_object_unref(w);
        const char* t = gtk_window_get_title(GTK_WINDOW(w));
        if (t && std::string(t) == title) return w;
    }
#else
    GList* l = gtk_window_list_toplevels();
    GtkWidget* found = nullptr;
    for (GList* i = l; i; i = i->next)
    {
        const char* t = gtk_window_get_title(GTK_WINDOW(i->data));
        if (t && std::string(t) == title) found = GTK_WIDGET(i->data);
    }
    g_list_free(l);
    if (found) return found;
#endif
    return nullptr;
}

std::string tooltip_of(GtkWidget* w)
{
    // (The tooltip of the label's whole: on it or the box around its icon.)
    for (; w; w = gtk_widget_get_parent(w))
    {
#if defined(RAPPORT_TEST_GTK4)
        if (const char* t = gtk_widget_get_tooltip_text(w)) return t;
#else
        if (gchar* t = gtk_widget_get_tooltip_text(w))
        {
            std::string s = t;
            g_free(t);
            return s;
        }
#endif
    }
    return {};
}

GtkWidget* label_with(GtkWidget* in, const std::string& text)
{
    return find(in, [&](GtkWidget* w) { return GTK_IS_LABEL(w) && text == gtk_label_get_text(GTK_LABEL(w)); });
}

GtkWidget* with_tooltip(GtkWidget* in, const std::string& tip)
{
    return find(in,
                [&](GtkWidget* w)
                {
#if defined(RAPPORT_TEST_GTK4)
                    const char* t = gtk_widget_get_tooltip_text(w);
                    return t && tip == t;
#else
                    gchar* t = gtk_widget_get_tooltip_text(w);
                    const bool same = t && tip == t;
                    g_free(t);
                    return same;
#endif
                });
}

void natural_width(GtkWidget* w, int& min, int& nat)
{
#if defined(RAPPORT_TEST_GTK4)
    gtk_widget_measure(w, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, nullptr, nullptr);
#else
    gtk_widget_get_preferred_width(w, &min, &nat);
#endif
}
int natural_height(GtkWidget* w)
{
    int min = 0, nat = 0;
#if defined(RAPPORT_TEST_GTK4)
    gtk_widget_measure(w, GTK_ORIENTATION_VERTICAL, -1, &min, &nat, nullptr, nullptr);
#else
    gtk_widget_get_preferred_height(w, &min, &nat);
#endif
    return nat;
}

// The label given `width` across: is its text shown cut short?
bool ellipsized_at(GtkWidget* label, int width)
{
    int min = 0, nat = 0;
    natural_width(label, min, nat);
    const int h = natural_height(label);
    GtkAllocation a{0, 0, width, h};
#if defined(RAPPORT_TEST_GTK4)
    gtk_widget_size_allocate(label, &a, -1);
#else
    gtk_widget_size_allocate(label, &a);
#endif
    return pango_layout_is_ellipsized(gtk_label_get_layout(GTK_LABEL(label)));
}

void activate_link(GtkWidget* label)
{
    gboolean handled = false;
    g_signal_emit_by_name(label, "activate-link", "rapport:", &handled);
}

bool focusable(GtkWidget* w)
{
#if defined(RAPPORT_TEST_GTK4)
    return gtk_widget_get_focusable(w);
#else
    return gtk_widget_get_can_focus(w);
#endif
}

bool has_text_widget(GtkWidget* w)
{
    return find(w, [](GtkWidget* c) { return GTK_IS_LABEL(c); }) != nullptr;
}

// The pointer's left button pressed and let go on `w` (its own click).
void click_icon(GtkWidget* w)
{
#if defined(RAPPORT_TEST_GTK4)
    GListModel* controllers = gtk_widget_observe_controllers(w);
    for (guint i = 0; i < g_list_model_get_n_items(controllers); ++i)
    {
        auto* c = G_OBJECT(g_list_model_get_item(controllers, i));
        if (GTK_IS_GESTURE_CLICK(c)) g_signal_emit_by_name(c, "released", 1, 4.0, 4.0);
        g_object_unref(c);
    }
    g_object_unref(controllers);
#else
    GdkEvent* e = gdk_event_new(GDK_BUTTON_RELEASE);
    e->button.button = 1;
    e->button.window = gtk_widget_get_window(w) ? GDK_WINDOW(g_object_ref(gtk_widget_get_window(w))) : nullptr;
    gboolean handled = false;
    g_signal_emit_by_name(w, "button-release-event", e, &handled);
    gdk_event_free(e);
#endif
}

bool hand_pointer(GtkWidget* w)
{
#if defined(RAPPORT_TEST_GTK4)
    GdkCursor* c = gtk_widget_get_cursor(w);
    return c && gdk_cursor_get_name(c) && std::string(gdk_cursor_get_name(c)) == "pointer";
#else
    GdkWindow* win = gtk_widget_get_window(w);
    return win && gdk_window_get_cursor(win) != nullptr;
#endif
}

std::string accessible_name(GtkWidget* w)
{
#if defined(RAPPORT_TEST_GTK4)
    (void)w;
    return {};  // (GTK 4 can't read an accessible's label back)
#else
    const char* n = atk_object_get_name(gtk_widget_get_accessible(w));
    return n ? n : "";
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
QLabel* link_label_named(QWidget* in, const std::string& name)
{
    for (QLabel* l : in->findChildren<QLabel*>())
        if (l->accessibleName() == QString::fromStdString(name) && l->text().contains("rapport:")) return l;
    return nullptr;
}
QLabel* label_with(QWidget* in, const std::string& text)
{
    for (QLabel* l : in->findChildren<QLabel*>())
        if (l->text() == QString::fromStdString(text)) return l;
    return nullptr;
}
QLabel* with_tooltip(QWidget* in, const std::string& tip)
{
    for (QLabel* l : in->findChildren<QLabel*>())
        if (l->toolTip() == QString::fromStdString(tip)) return l;
    return nullptr;
}
std::string tooltip_of(QWidget* w)
{
    for (; w; w = w->parentWidget())
        if (!w->toolTip().isEmpty()) return w->toolTip().toStdString();
    return {};
}
void click_icon(QWidget* w)
{
    const QPointF at(2, 2);
    QMouseEvent press(QEvent::MouseButtonPress, at, w->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, at, w->mapToGlobal(at), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QApplication::sendEvent(w, &release);
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

    int long_clicks = 0, plain_clicks = 0, icon_clicks = 0;
    auto win = (*ui)->create(
        window{"rapport link label test",
               {vbox{button{"Before"},
                     label{long_text}.ellipsize().link([&] { ++long_clicks; }),
                     label{"Plain link"}.link([&] { ++plain_clicks; }).focusable(false),
                     label{}.icon(rap::icon::named("edit-copy")).link([&] { ++icon_clicks; }).tooltip(copy_tip),
                     label{"Ag"}}}});
    win->show();

    native_widget top = nullptr;
    int waited = 0;
    bool done = false;
    auto tick = (*ui)->backend().timer(
        50, true,
        [&]
        {
            if (done) return;
            if (!top) top = window_titled("rapport link label test");
            if (!top)
            {
                if (++waited > 100)
                {
                    expect(false, "the window shows");
                    (*ui)->quit(1);
                }
                return;
            }
            done = true;
#if defined(RAPPORT_TEST_GTK3) || defined(RAPPORT_TEST_GTK4)
            GtkWidget* lng = label_with(top, long_text);
            GtkWidget* plain = label_with(top, "Plain link");
            GtkWidget* icon = with_tooltip(top, copy_tip);
            GtkWidget* line = label_with(top, "Ag");
            expect(lng && plain && icon && line, "the labels are there");
            if (!lng || !plain || !icon || !line) return (*ui)->quit(1);

            // -- an ellipsized link
            int min = 0, nat = 0;
            natural_width(lng, min, nat);
            expect(min < nat && min < 100, "ellipsized link: a small minimum width");
            int line_min = 0, line_nat = 0;
            natural_width(line, line_min, line_nat);
            expect(nat > 10 * line_nat, "ellipsized link: its whole text's natural width");
            expect(!ellipsized_at(lng, nat), "given its natural width: its whole text");
            expect(ellipsized_at(lng, 120), "given less: its text cut short");
            activate_link(lng);
            expect(long_clicks == 1, "cut short, it still runs its click");
            expect(tooltip_of(lng) == long_text, "its tooltip: the whole text");
#if !defined(RAPPORT_TEST_GTK4)
            expect(accessible_name(lng) == long_text, "its accessible name: the whole text");
#endif
            expect(focusable(lng), "a link: a Tab stop by default");

            // -- focusable(false)
            expect(!focusable(plain), "focusable(false): no Tab stop");
            activate_link(plain);
            expect(plain_clicks == 1, "focusable(false): it still runs its click");

            // -- an icon-only link
            expect(!has_text_widget(icon), "icon-only link: no text widget");
            expect(natural_height(icon) <= natural_height(line),
                   "icon-only link: no taller than a line of text");
            click_icon(icon);
            expect(icon_clicks == 1, "icon-only link: clicking the icon runs it");
            expect(hand_pointer(icon), "icon-only link: the pointer's hand over it");
#if !defined(RAPPORT_TEST_GTK4)
            expect(accessible_name(icon) == copy_tip, "icon-only link: named by its tooltip");
#endif
#elif defined(RAPPORT_TEST_QT6)
            QLabel* lng = link_label_named(top, long_text);
            QLabel* plain = link_label_named(top, "Plain link");
            QLabel* icon = with_tooltip(top, copy_tip);
            QLabel* line = label_with(top, "Ag");
            expect(lng && plain && icon && line, "the labels are there");
            if (!lng || !plain || !icon || !line) return (*ui)->quit(1);

            // -- an ellipsized link
            expect(lng->minimumSizeHint().width() < 100 &&
                       lng->sizeHint().width() > 10 * line->sizeHint().width(),
                   "ellipsized link: a small minimum width, its whole text's natural width");
            lng->resize(lng->sizeHint().width(), lng->height());
            expect(!lng->text().contains(QChar(0x2026)) && lng->text().contains("longest name of them all"),
                   "given its natural width: its whole text");
            lng->resize(120, lng->height());
            expect(lng->text().contains(QChar(0x2026)) && !lng->text().contains("longest name of them all"),
                   "given less: its text cut short");
            emit lng->linkActivated("rapport:");
            expect(long_clicks == 1, "cut short, it still runs its click");
            expect(tooltip_of(lng) == long_text, "its tooltip: the whole text");
            expect(lng->accessibleName().toStdString() == long_text, "its accessible name: the whole text");
            expect(lng->focusPolicy() != Qt::NoFocus, "a link: a Tab stop by default");

            // -- focusable(false)
            expect(plain->focusPolicy() == Qt::NoFocus, "focusable(false): no Tab stop");
            emit plain->linkActivated("rapport:");
            expect(plain_clicks == 1, "focusable(false): it still runs its click");

            // -- an icon-only link
            expect(icon->text().isEmpty() && icon->findChildren<QLabel*>().isEmpty(),
                   "icon-only link: no text widget");
            expect(icon->sizeHint().height() <= line->sizeHint().height() &&
                       icon->height() <= line->sizeHint().height(),
                   "icon-only link: no taller than a line of text");
            click_icon(icon);
            expect(icon_clicks == 1, "icon-only link: clicking the icon runs it");
            expect(icon->cursor().shape() == Qt::PointingHandCursor, "icon-only link: the pointer's hand over it");
            expect(icon->accessibleName().toStdString() == copy_tip, "icon-only link: named by its tooltip");
            expect(icon->focusPolicy() == Qt::NoFocus, "icon-only link: no Tab stop");
#endif
            (*ui)->quit(fails ? 1 : 0);
        });
    (*ui)->run();
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
