// A list driven by synthesized input through a real backend, off the desktop
// (GTK 3 on Broadway, Qt on its offscreen platform): Ctrl pressed before a
// drag still copies, Ctrl+click toggles a row at the release, and Space on a
// mixed selection takes the new state from the cursor row.
//   rapport-list-input-test-gtk3   (GDK_BACKEND=broadway, broadwayd running)
//   rapport-list-input-test-qt6    (QT_QPA_PLATFORM=offscreen)
// GTK 4 has no way to synthesize events (its event constructors are private
// and gtk_main_do_event is gone), so it's covered by list_press_test's rule.
#include <rapport/rapport.h>

#if defined(RAPPORT_TEST_GTK3)
#include <gtk/gtk.h>
#elif defined(RAPPORT_TEST_QT6)
#include <QAbstractItemView>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTreeView>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
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

std::string show(const std::vector<size_t>& rows)
{
    std::string s = "{";
    for (size_t r : rows) s += (s.size() > 1 ? "," : "") + std::to_string(r);
    return s + "}";
}

// Rows with boxes; a box clicked (or Space) sets the selection to the
// clicked row's new state, as Tortilla's Commit and Browse References do.
class rows_model : public rap::list_model
{
public:
    size_t row_count() const override { return checked_.size(); }
    std::string text(size_t row, size_t) const override { return "Row " + std::to_string(row); }
    rap::check_state checked(size_t row) const override
    {
        return checked_[row] ? rap::check_state::checked : rap::check_state::unchecked;
    }
    void toggle_checked(size_t row) override
    {
        toggled.push_back(row);
        std::vector<size_t> rows = list ? list->selection() : std::vector<size_t>{};
        if (!std::ranges::contains(rows, row)) rows = {row};
        const bool on = !checked_[row];
        for (size_t r : rows) checked_[r] = on;
        notify_reset();
    }
    void set(std::vector<bool> c)
    {
        checked_ = std::move(c);
        notify_reset();
    }
    bool is(size_t row) const { return checked_[row]; }

    rap::list* list = nullptr;
    std::vector<size_t> toggled;

private:
    std::vector<bool> checked_ = std::vector<bool>(10, false);
};

struct reorder_call
{
    std::vector<size_t> rows;
    size_t dest = 0;
    rap::drop_action action{};
};

// Input to the list's native view: presses, moves and releases on rows
// (Ctrl held or not), and Space.
class driver
{
public:
    virtual ~driver() = default;
    virtual bool ready() = 0;
    virtual void press(size_t row, bool ctrl, bool shift = false) = 0;
    virtual void move(size_t row, bool ctrl) = 0;
    virtual void release(size_t row, bool ctrl, bool shift = false) = 0;
    virtual void space() = 0;
    virtual void spin() = 0;
};

#if defined(RAPPORT_TEST_GTK3)

class gtk3_driver : public driver
{
public:
    bool ready() override
    {
        spin();
        if (!view_)
            for (GList* l = gtk_window_list_toplevels(); l && !view_; l = l->next)
                find(GTK_WIDGET(l->data));
        if (!view_ || !gtk_widget_get_realized(view_)) return false;
        GdkRectangle r;
        GtkTreePath* path = gtk_tree_path_new_from_indices(9, -1);
        gtk_tree_view_get_background_area(GTK_TREE_VIEW(view_), path, nullptr, &r);
        gtk_tree_path_free(path);
        return r.height > 0;
    }
    void press(size_t row, bool ctrl, bool shift) override
    {
        button(GDK_BUTTON_PRESS, row, mods(ctrl, shift));
    }
    void release(size_t row, bool ctrl, bool shift) override
    {
        button(GDK_BUTTON_RELEASE, row, GDK_BUTTON1_MASK | mods(ctrl, shift));
    }
    void move(size_t row, bool ctrl) override
    {
        // In steps, as a pointer moves: the threshold first, then the row.
        const auto [x, y] = at(row);
        for (int i = 1; i <= 4; ++i)
        {
            GdkEvent* e = gdk_event_new(GDK_MOTION_NOTIFY);
            e->motion.window = GDK_WINDOW(g_object_ref(bin()));
            e->motion.send_event = true;
            e->motion.time = ++time_;
            e->motion.x = last_x_ + (x - last_x_) * i / 4;
            e->motion.y = last_y_ + (y - last_y_) * i / 4;
            e->motion.state = GDK_BUTTON1_MASK | (ctrl ? GDK_CONTROL_MASK : 0);
            gdk_event_set_device(e, pointer());
            gdk_event_set_source_device(e, pointer());
            gtk_main_do_event(e);
            gdk_event_free(e);
            spin();
        }
        last_x_ = x;
        last_y_ = y;
    }
    void space() override
    {
        gtk_widget_grab_focus(view_);
        GdkWindow* top = gtk_widget_get_window(gtk_widget_get_toplevel(view_));
        for (GdkEventType type : {GDK_KEY_PRESS, GDK_KEY_RELEASE})
        {
            GdkEvent* e = gdk_event_new(type);
            e->key.window = GDK_WINDOW(g_object_ref(top));
            e->key.send_event = true;
            e->key.time = ++time_;
            e->key.keyval = GDK_KEY_space;
            GdkKeymapKey* keys = nullptr;
            gint n = 0;
            if (gdk_keymap_get_entries_for_keyval(
                    gdk_keymap_get_for_display(gdk_display_get_default()), GDK_KEY_space, &keys,
                    &n) &&
                n > 0)
                e->key.hardware_keycode = guint16(keys[0].keycode);
            g_free(keys);
            e->key.string = g_strdup(" ");
            e->key.length = 1;
            gdk_event_set_device(
                e, gdk_seat_get_keyboard(gdk_display_get_default_seat(gdk_display_get_default())));
            gtk_main_do_event(e);
            gdk_event_free(e);
        }
        spin();
    }
    void spin() override
    {
        while (gtk_events_pending()) gtk_main_iteration_do(false);
    }

private:
    void find(GtkWidget* w)
    {
        if (view_) return;
        if (GTK_IS_TREE_VIEW(w))
        {
            view_ = w;
            return;
        }
        if (GTK_IS_CONTAINER(w))
            gtk_container_forall(
                GTK_CONTAINER(w),
                +[](GtkWidget* c, gpointer p) { static_cast<gtk3_driver*>(p)->find(c); }, this);
    }
    GdkWindow* bin() const { return gtk_tree_view_get_bin_window(GTK_TREE_VIEW(view_)); }
    GdkDevice* pointer() const
    {
        return gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
    }
    // A row's middle, bin window coordinates.
    std::pair<double, double> at(size_t row) const
    {
        GdkRectangle r;
        GtkTreePath* path = gtk_tree_path_new_from_indices(int(row), -1);
        gtk_tree_view_get_background_area(GTK_TREE_VIEW(view_), path, nullptr, &r);
        gtk_tree_path_free(path);
        return {r.x + 60.0, r.y + r.height / 2.0};
    }
    static guint mods(bool ctrl, bool shift)
    {
        return (ctrl ? GDK_CONTROL_MASK : 0) | (shift ? GDK_SHIFT_MASK : 0);
    }
    void button(GdkEventType type, size_t row, guint state)
    {
        const auto [x, y] = at(row);
        GdkEvent* e = gdk_event_new(type);
        e->button.window = GDK_WINDOW(g_object_ref(bin()));
        e->button.send_event = true;
        // Far apart, so presses don't count as a double click.
        time_ += type == GDK_BUTTON_PRESS ? 5000 : 1;
        e->button.time = time_;
        e->button.x = x;
        e->button.y = y;
        e->button.state = state;
        e->button.button = GDK_BUTTON_PRIMARY;
        gdk_event_set_device(e, pointer());
        gdk_event_set_source_device(e, pointer());
        gtk_main_do_event(e);
        gdk_event_free(e);
        last_x_ = x;
        last_y_ = y;
        spin();
    }

    GtkWidget* view_ = nullptr;
    guint32 time_ = 1000;
    double last_x_ = 0, last_y_ = 0;
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
        if (!view_)
            for (QWidget* w : QApplication::topLevelWidgets())
                if ((view_ = w->findChild<QTreeView*>())) break;
        return view_ && view_->isVisible() &&
               view_->visualRect(view_->model()->index(9, 0)).height() > 0;
    }
    void press(size_t row, bool ctrl, bool shift) override
    {
        mouse(QEvent::MouseButtonPress, at(row), Qt::LeftButton, Qt::LeftButton, ctrl, shift);
    }
    void release(size_t row, bool ctrl, bool shift) override
    {
        mouse(QEvent::MouseButtonRelease, at(row), Qt::LeftButton, Qt::NoButton, ctrl, shift);
    }
    void move(size_t row, bool ctrl) override
    {
        const QPoint to = at(row);
        for (int i = 1; i <= 4; ++i)
            mouse(QEvent::MouseMove, last_ + (to - last_) * i / 4, Qt::NoButton, Qt::LeftButton,
                  ctrl);
    }
    void space() override
    {
        view_->setFocus();
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, " ");
        QApplication::sendEvent(view_, &down);
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, " ");
        QApplication::sendEvent(view_, &up);
        spin();
    }
    void spin() override { QApplication::processEvents(); }

private:
    QPoint at(size_t row) const
    {
        const QRect r = view_->visualRect(view_->model()->index(int(row), 0));
        return {r.left() + 60, r.center().y()};
    }
    void mouse(QEvent::Type type, QPoint pos, Qt::MouseButton button, Qt::MouseButtons held,
               bool ctrl, bool shift = false)
    {
        QWidget* vp = view_->viewport();
        Qt::KeyboardModifiers mods;
        if (ctrl) mods |= Qt::ControlModifier;
        if (shift) mods |= Qt::ShiftModifier;
        QMouseEvent e(type, pos, vp->mapToGlobal(pos), button, held, mods);
        QApplication::sendEvent(vp, &e);
        last_ = pos;
        spin();
    }

    QTreeView* view_ = nullptr;
    QPoint last_;
};
using native_driver = qt6_driver;
constexpr const char* backend_name = "qt6";

#endif

void run(driver& d, rap::list& list, rows_model& model, std::optional<reorder_call>& reordered)
{
    // Ctrl held from the press: the press is a possible drag, not a toggle.
    list.select({1});
    reordered.reset();
    d.press(4, true);
    expect(list.selection() == std::vector<size_t>{1},
           "Ctrl+press: no toggle yet, got " + show(list.selection()));
    d.move(7, true);
    d.release(7, true);
    expect(reordered && reordered->action == rap::drop_action::copy,
           "Ctrl+press then drag: a copy");
    expect(reordered && reordered->rows == std::vector<size_t>{1, 4},
           "Ctrl+press then drag from an unselected row: it joins the selection, got " +
               (reordered ? show(reordered->rows) : "nothing"));

    // From a selected row: the selection as it is.
    list.select({1, 2});
    reordered.reset();
    d.press(2, true);
    d.move(6, true);
    d.release(6, true);
    expect(reordered && reordered->action == rap::drop_action::copy &&
               reordered->rows == std::vector<size_t>{1, 2},
           "Ctrl+press on a selected row then drag: the selection copied, got " +
               (reordered ? show(reordered->rows) : "nothing"));

    // Without Ctrl at the drop: a move.
    list.select({3});
    reordered.reset();
    d.press(3, false);
    d.move(8, false);
    d.release(8, false);
    expect(reordered && reordered->action == rap::drop_action::move &&
               reordered->rows == std::vector<size_t>{3},
           "a plain drag: a move");

    // Ctrl+click, let go where it was pressed: the toggle, at the release.
    list.select({1, 3});
    reordered.reset();
    d.press(3, true);
    expect(list.selection() == std::vector<size_t>{1, 3},
           "Ctrl+press on a selected row: kept until the release");
    d.release(3, true);
    expect(list.selection() == std::vector<size_t>{1},
           "Ctrl+click unselects a selected row, got " + show(list.selection()));
    expect(list.current() == std::optional<size_t>(3), "Ctrl+click moves the cursor to the row");
    d.press(5, true);
    d.release(5, true);
    expect(list.selection() == std::vector<size_t>{1, 5},
           "Ctrl+click adds an unselected row, got " + show(list.selection()));
    expect(!reordered, "Ctrl+clicks reorder nothing");

    // The toolkit's own clicks still work after the drags above (a press
    // it saw needs its release).
    list.select({});
    d.press(2, false);
    d.release(2, false);
    expect(list.selection() == std::vector<size_t>{2},
           "a plain click after drags selects, got " + show(list.selection()));

    // A plain click inside a multi-selection: that row alone.
    list.select({1, 5, 6});
    d.press(5, false);
    d.release(5, false);
    expect(list.selection() == std::vector<size_t>{5},
           "a click in a multi-selection selects that row, got " + show(list.selection()));

    // Space on a mixed selection: the cursor row (1, unchecked) decides, so
    // all three are checked, whichever row is lowest (0, checked).
    model.set({true, false, true, false, false, false, false, false, false, false});
    list.set_current(1);
    list.select({0, 1, 2});
    model.toggled.clear();
    d.space();
    expect(model.toggled == std::vector<size_t>{1},
           "Space toggles the cursor row's box, got " + show(model.toggled));
    expect(model.is(0) && model.is(1) && model.is(2),
           "Space on a mixed selection: all take the cursor row's new state");
}

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
    rows_model model;
    rap::list* list = nullptr;
    std::optional<reorder_call> reordered;
    using namespace rap::layout;
    auto win = (*ui)->create(window{
        "List input",
        {vbox{
            rap::layout::list{{{"Name", {}, true}}, &model}
                .checkboxes(true)
                .on_reorder_or_copy(
                    [&](const std::vector<size_t>& rows, size_t dest, rap::drop_action action)
                    {
                        reordered = reorder_call{rows, dest, action};
                        return true;
                    },
                    rap::mod_ctrl)
                .bind(list)
                .stretch(),
        }}}.size(30_ch, 16_lines));
    model.list = list;
    win->show();
    native_driver d;
    (*ui)->post(
        [&]
        {
            bool ok = false;
            for (int i = 0; i < 500 && !(ok = d.ready()); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            expect(ok, std::string("the list is shown (") + backend_name + ")");
            if (ok && list) run(d, *list, model, reordered);
            std::printf(fails ? "%d failed\n" : "all passed\n", fails);
            std::fflush(stdout);
            (*ui)->quit(fails ? 1 : 0);
        });
    return (*ui)->run();
}
