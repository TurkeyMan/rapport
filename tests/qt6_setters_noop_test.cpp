// A setter given the value it has does nothing on Qt (#88): no layout
// request, no repaint, no change handler fired. One of each widget in a
// window shown off the desktop (QT_QPA_PLATFORM=offscreen); each setter set
// to a value, then set to it again with every QEvent::LayoutRequest,
// UpdateRequest and Paint in the application counted: none. Then a new
// value still does as before (where it shows: some events).
//   rapport-setters-noop-test-qt6   (QT_QPA_PLATFORM=offscreen)
#include <rapport/rapport.h>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QThread>
#include <QWindowStateChangeEvent>
#include <QTreeView>

#include <dlfcn.h>
#include <link.h>

#include <cstdio>
#include <cstring>
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

// Every layout request, update request and paint the application sees.
class counter : public QObject
{
public:
    int layouts = 0, updates = 0, paints = 0;
    void reset() { layouts = updates = paints = 0; }
    int total() const { return layouts + updates + paints; }
    bool eventFilter(QObject* o, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::LayoutRequest: ++layouts; break;
        case QEvent::UpdateRequest: ++updates; break;
        case QEvent::Paint: ++paints; break;
        default: break;
        }
        return QObject::eventFilter(o, e);
    }
};

// A test seam of the backend's (`name`): linked in (a static build), or in
// its module, loaded already (`module`, "rapport_gtk3" say) by ui::load.
void* backend_seam(const char* module, const char* name)
{
    if (void* f = dlsym(RTLD_DEFAULT, name)) return f;
    struct found
    {
        const char* module;
        std::string path;
    } f{module, {}};
    dl_iterate_phdr(
        +[](dl_phdr_info* info, size_t, void* p) -> int
        {
            auto* f = static_cast<found*>(p);
            if (!info->dlpi_name || !std::strstr(info->dlpi_name, f->module)) return 0;
            f->path = info->dlpi_name;
            return 1;
        },
        &f);
    if (f.path.empty()) return nullptr;
    void* h = dlopen(f.path.c_str(), RTLD_NOW | RTLD_NOLOAD);  // (loaded already: only its handle)
    return h ? dlsym(h, name) : nullptr;
}

// Everything due handled, posted events and paints included.
void settle()
{
    for (int i = 0; i < 20; ++i)
    {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}

class rows_model : public rap::list_model
{
public:
    size_t row_count() const override { return 3; }
    std::string text(size_t row, size_t col) const override
    {
        return (col == 0 ? "Name " : "Note ") + std::to_string(row);
    }
};

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options opts;
    opts.backend = "qt6";
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 2;
    }
    // No caret blinking in the time counted: it's no setter's.
    QApplication::setCursorFlashTime(0);

    rap::label* label = nullptr;
    rap::button* button = nullptr;
    rap::check* check = nullptr;
    rap::entry* entry = nullptr;
    rap::drop_down* drop = nullptr;
    rap::combo* combo = nullptr;
    rap::text_view* text = nullptr;
    rap::slider* slider = nullptr;
    rap::spin* spin = nullptr;
    rap::progress_bar* progress = nullptr;
    rap::color_button* colour = nullptr;
    rap::date_picker* date = nullptr;
    rap::tool_button* tool = nullptr;
    rap::menu_button* menu = nullptr;
    rap::tabs* tabs = nullptr;
    rap::splitter* split = nullptr;
    rap::tree* tree = nullptr;
    rap::list* list = nullptr;
    rows_model model;
    int changes = 0;  // change handlers fired

    using namespace rap::layout;
    using namespace rap::literals;
    const std::vector<rap::tree_item> items{{"Alpha", "a", {{"Child", "c"}}}, {"Beta", "b"}};
    auto win = (*ui)->create(
        window{"Setters",
               {vbox{hbox{rap::layout::label{"Text"}.bind(label), rap::layout::button{"Press"}.bind(button),
                          rap::layout::check{"Check"}.bind(check).on_toggled([&](bool) { ++changes; }),
                          rap::layout::entry{"Entry"}.bind(entry).on_changed([&](const std::string&) { ++changes; }),
                          rap::layout::drop_down{{"One", "Two"}, 0}.bind(drop).on_changed([&](int) { ++changes; }),
                          rap::layout::combo{{"One", "Two"}, "One"}.bind(combo).on_changed(
                              [&](const std::string&) { ++changes; })},
                     hbox{rap::layout::slider{0, 100, 10}.bind(slider).on_changed([&](double) { ++changes; }),
                          rap::layout::spin{0, 100, 10}.bind(spin).on_changed([&](double) { ++changes; }),
                          rap::layout::progress_bar{}.bind(progress),
                          rap::layout::color_button{rap::color{10, 20, 30}}.bind(colour),
                          rap::layout::date_picker{rap::date{2026, 1, 2}}.bind(date),
                          rap::layout::tool_button{rap::icon::named("go-up"), "Tool"}.toggle(true).bind(tool),
                          rap::layout::menu_button{"Menu"}.bind(menu)},
                     rap::layout::tabs{{"One", rap::layout::label{"one"}}, {"Two", rap::layout::label{"two"}}}.bind(
                         tabs),
                     rap::layout::splitter(rap::orientation::horizontal, rap::layout::label{"Left"},
                                           rap::layout::label{"Right"})
                         .bind(split),
                     hbox{rap::layout::text_view{"Text"}.bind(text).stretch(),
                          rap::layout::tree{items}.bind(tree).stretch(),
                          rap::layout::list{{{"Name", 100_px}, {"Notes", 100_px}}, &model}.bind(list).stretch()}
                         .stretch()}}}
            .size(1000_px, 600_px));
    win->show();
    settle();
    const bool all = label && button && check && entry && drop && combo && text && slider && spin && progress &&
                     colour && date && tool && menu && tabs && split && tree && list;
    expect(all, "one of each widget");
    if (!all) return 1;

    counter count;
    qApp->installEventFilter(&count);

    // Each setter: set to `same`, then again (counted: nothing at all); then
    // to `other` (counted: some events where `shows`).
    struct setter
    {
        const char* what;
        std::function<void()> same, other;
        bool shows;
    };
    const std::vector<setter> setters = {
        {"label::set_text", [&] { label->set_text("A"); }, [&] { label->set_text("Longer B"); }, true},
        {"button::set_text", [&] { button->set_text("Go"); }, [&] { button->set_text("Go on"); }, true},
        {"button::set_enabled", [&] { button->set_enabled(false); }, [&] { button->set_enabled(true); }, true},
        {"button::set_tooltip", [&] { button->set_tooltip("Tip"); }, [&] { button->set_tooltip("Other"); }, false},
        {"label::set_visible", [&] { label->set_visible(false); }, [&] { label->set_visible(true); }, true},
        {"check::set_checked", [&] { check->set_checked(true); }, [&] { check->set_checked(false); }, true},
        {"check::set_state", [&] { check->set_state(rap::check_state::indeterminate); },
         [&] { check->set_state(rap::check_state::checked); }, true},
        {"entry::set_text", [&] { entry->set_text("Same"); }, [&] { entry->set_text("Other"); }, true},
        {"entry::set_placeholder", [&] { entry->set_placeholder("Hint"); },
         [&] { entry->set_placeholder("Other hint"); }, false},
        {"entry::set_read_only", [&] { entry->set_read_only(true); }, [&] { entry->set_read_only(false); }, true},
        {"drop_down::set_items", [&] { drop->set_items({"One", "Two"}); },
         [&] { drop->set_items({"One", "Two", "Three"}); }, false},
        {"drop_down::set_current", [&] { drop->set_current(1); }, [&] { drop->set_current(0); }, true},
        {"combo::set_text", [&] { combo->set_text("Two"); }, [&] { combo->set_text("Three"); }, true},
        {"combo::set_items", [&] { combo->set_items({"One", "Two"}); },
         [&] { combo->set_items({"One", "Two", "Three"}); }, false},
        {"text_view::set_text", [&] { text->set_text("Plain"); }, [&] { text->set_text("Other text"); }, true},
        {"text_view::set_read_only", [&] { text->set_read_only(true); }, [&] { text->set_read_only(false); },
         false},
        {"slider::set_value", [&] { slider->set_value(40); }, [&] { slider->set_value(60); }, true},
        {"slider::set_range", [&] { slider->set_range(0, 200); }, [&] { slider->set_range(0, 100); }, true},
        {"spin::set_value", [&] { spin->set_value(40); }, [&] { spin->set_value(60); }, true},
        {"spin::set_range", [&] { spin->set_range(0, 200); }, [&] { spin->set_range(0, 100); }, false},
        {"progress_bar::set_fraction", [&] { progress->set_fraction(0.25f); },
         [&] { progress->set_fraction(0.75f); }, true},
        {"color_button::set_color", [&] { colour->set_color({200, 100, 50}); },
         [&] { colour->set_color({50, 100, 200}); }, true},
        {"date_picker::set_date", [&] { date->set_date({2026, 3, 4}); }, [&] { date->set_date({2026, 5, 6}); },
         true},
        {"tool_button::set_checked", [&] { tool->set_checked(true); }, [&] { tool->set_checked(false); }, true},
        {"tool_button::set_icon", [&] { tool->set_icon(rap::icon::named("go-down")); },
         [&] { tool->set_icon(rap::icon::named("go-up")); }, true},
        {"menu_button::set_text", [&] { menu->set_text("Menu"); }, [&] { menu->set_text("Other menu"); }, true},
        {"menu_button::set_pushed", [&] { menu->set_pushed(true); }, [&] { menu->set_pushed(false); }, true},
        {"tabs::set_current", [&] { tabs->set_current(1); }, [&] { tabs->set_current(0); }, true},
        {"splitter::set_sizes", [&] { split->set_sizes({300}); }, [&] { split->set_sizes({400}); }, true},
        {"tree::set_expanded", [&] { tree->set_expanded("a", true); }, [&] { tree->set_expanded("a", false); },
         true},
        {"tree::set_separator_before", [&] { tree->set_separator_before("b", true); },
         [&] { tree->set_separator_before("b", false); }, true},
        {"list::set_current", [&] { list->set_current(1); }, [&] { list->set_current(2); }, true},
        {"list::set_sort_indicator", [&] { list->set_sort_indicator(0, rap::sort_order::ascending); },
         [&] { list->set_sort_indicator(0, rap::sort_order::descending); }, true},
        {"list::set_column_widths", [&] { list->set_column_widths({120, 120}); },
         [&] { list->set_column_widths({140, 120}); }, true},
        {"list::set_column_visible", [&] { list->set_column_visible(1, false); },
         [&] { list->set_column_visible(1, true); }, true},
        {"window::set_title", [&] { win->set_title("Setters"); }, [&] { win->set_title("Setters 2"); }, false},
    };
    for (const setter& s : setters)
    {
        s.same();
        settle();
        count.reset();
        changes = 0;
        s.same();
        settle();
        expect(count.total() == 0 && changes == 0,
               std::string(s.what) + " given the value it has: nothing (" + std::to_string(count.layouts) +
                   " layout requests, " + std::to_string(count.updates) + " update requests, " +
                   std::to_string(count.paints) + " paints, " + std::to_string(changes) + " change handlers)");
        count.reset();
        changes = 0;
        s.other();
        settle();
        if (s.shows)
            expect(count.total() > 0, std::string(s.what) + " given a new value: shown as before (" +
                                          std::to_string(count.total()) + " events)");
        expect(changes == 0, std::string(s.what) + " from the app: no change handler (" +
                                 std::to_string(changes) + ")");
    }
    qApp->removeEventFilter(&count);

    // -- "unchanged" is what's on screen: the cases a cached value got wrong.
    win->set_title("Setters");  // (the setter checks left it "Setters 2")
    button->set_text("Press");  // (and the button's text another)
    settle();
    auto widgets_of = [](const char* title)
    {
        std::vector<QWidget*> out;
        for (QWidget* w : QApplication::allWidgets())
            if (w->window() && w->window()->windowTitle() == title) out.push_back(w);
        return out;
    };
    auto first = [&](const char* title, auto pick) -> decltype(pick(nullptr))
    {
        for (QWidget* w : widgets_of(title))
            if (auto* t = pick(w)) return t;
        return nullptr;
    };
    // A busy bar (pulse) given the fraction it had before: determinate again,
    // at it.
    {
        auto* bar = first("Setters", [](QWidget* w) { return qobject_cast<QProgressBar*>(w); });
        progress->set_fraction(0.75f);
        progress->pulse();
        settle();
        progress->set_fraction(0.75f);
        settle();
        expect(bar && bar->maximum() != 0 && bar->value() == 750,
               "a busy bar given the fraction it had: determinate again at it (" +
                   std::to_string(bar ? bar->maximum() : -1) + ", " + std::to_string(bar ? bar->value() : -1) + ")");
    }
    // A spin box with the user's typing (not taken in, then taken in) given
    // the value it had: it shows that value.
    {
        auto* box = first("Setters", [](QWidget* w) { return qobject_cast<QDoubleSpinBox*>(w); });
        expect(box != nullptr, "the spin box");
        if (box)
        {
            auto type = [&](const char* text)
            {
                box->selectAll();
                for (const char* c = text; *c; ++c)
                {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_0 + (*c - '0'), Qt::NoModifier, QString(QChar(*c)));
                    QKeyEvent release(QEvent::KeyRelease, Qt::Key_0 + (*c - '0'), Qt::NoModifier, QString(QChar(*c)));
                    QApplication::sendEvent(box, &press);
                    QApplication::sendEvent(box, &release);
                }
                settle();
            };
            for (const bool tracking : {false, true})
            {
                box->setKeyboardTracking(tracking);
                spin->set_value(4);
                settle();
                type("7");
                const std::string typed = box->cleanText().toStdString();
                spin->set_value(4);
                settle();
                expect(box->value() == 4 && box->cleanText() == box->textFromValue(4),
                       std::string("a spin box typed into (") + (tracking ? "taken in" : "not taken in") + ", \"" +
                           typed + "\"), given the value it had: it shows it, \"" +
                           box->cleanText().toStdString() + "\"");
            }
            box->setKeyboardTracking(true);
        }
    }
    // Its own disable kept under a disabled parent: the parent enabled
    // again, it's still disabled; enabled itself, it is.
    {
        auto* press = first("Setters", [](QWidget* w)
                            {
                                auto* b = qobject_cast<QPushButton*>(w);
                                return b && b->text() == "Press" ? b : nullptr;
                            });
        QWidget* parent = press ? press->parentWidget() : nullptr;
        expect(parent != nullptr, "the button and its parent");
        if (parent)
        {
            parent->setEnabled(false);
            button->set_enabled(true);  // its own: enabled already, nothing
            button->set_enabled(false);
            parent->setEnabled(true);
            settle();
            expect(!press->isEnabled(), "disabled itself under a disabled parent: still disabled, the parent enabled");
            button->set_enabled(true);
            settle();
            expect(press->isEnabled(), "enabled itself again: enabled");
        }
    }
    // The list's header changed by the user (a sort click, a drag, a
    // resize), then the app's state given again: the header shows it.
    {
        auto* view = first("Setters", [](QWidget* w)
                           {
                               auto* v = qobject_cast<QTreeView*>(w);
                               return v && !v->isHeaderHidden() ? v : nullptr;
                           });
        expect(view != nullptr, "the list's view");
        if (view)
        {
            QHeaderView* h = view->header();
            list->set_sort_indicator(1, rap::sort_order::descending);
            settle();
            h->setSortIndicator(0, Qt::AscendingOrder);  // as a click flips it
            list->set_sort_indicator(1, rap::sort_order::descending);
            settle();
            expect(h->sortIndicatorSection() == 1 && h->sortIndicatorOrder() == Qt::DescendingOrder,
                   "the sort indicator moved by the user, given the app's again: shown (" +
                       std::to_string(h->sortIndicatorSection()) + ")");
            list->set_column_widths({120, 120});
            settle();
            h->resizeSection(0, 200);  // the user's drag
            list->set_column_widths({120, 120});
            settle();
            expect(h->sectionSize(0) == 120, "a column resized by the user, given the app's width again: " +
                                                 std::to_string(h->sectionSize(0)));
            list->set_column_order({0, 1});
            settle();
            h->moveSection(0, 1);  // the user's drag
            list->set_column_order({0, 1});
            settle();
            expect(h->logicalIndex(0) == 0, "columns moved by the user, given the app's order again: " +
                                                std::to_string(h->logicalIndex(0)) + " first");
        }
    }
    // Full screen asked for and taken back at once (before any compositor
    // acknowledges): not full screen.
    {
        win->set_fullscreen(true);
        win->set_fullscreen(false);
        settle();
        expect(!win->fullscreen(), "full screen asked for and taken back at once: not full screen");
    }
    // The requests themselves, counted by the backend's seam (offscreen
    // takes a window state at once: each request is answered as it's made);
    // full screen from outside is the frame's state set as a window
    // manager's change would set it.
    {
        using requests_fn = const char*();
        auto* requests =
            reinterpret_cast<requests_fn*>(backend_seam("rapport_qt6", "rapport_qt6_fullscreen_requests_for_tests"));
        QWidget* frame = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets())
            if (w->windowTitle() == "Setters") frame = w;
        expect(requests && frame, "full screen: the backend's request counter, the window");
        if (requests && frame)
        {
            settle();
            requests();
            win->set_fullscreen(true);
            win->set_fullscreen(false);
            std::string r = requests();
            expect(r == "FU", "full screen: on then off: both asked, in order (" + r + ")");
            win->set_fullscreen(true);
            win->set_fullscreen(true);
            r = requests();
            expect(r == "F", "full screen: on twice: asked once (" + r + ")");
            win->set_fullscreen(false);
            requests();
            win->set_fullscreen(true);
            win->set_fullscreen(false);
            frame->setWindowState(frame->windowState() | Qt::WindowFullScreen);  // from outside
            win->set_fullscreen(false);
            r = requests();
            expect(r == "FUU", "full screen: on, off, then full screen from outside: off asked again (" + r + ")");
            settle();
            expect(!win->fullscreen(), "full screen: and not full screen after");
            // On, off (both unanswered: their state changes kept from the
            // backend), the first answered (the frame full screen, its state
            // change told), on again: the second still pending, so on is asked
            // again (F, U, F), not skipped.
            {
                struct eat_state : QObject
                {
                    bool on = true;
                    bool eventFilter(QObject*, QEvent* e) override
                    {
                        return on && e->type() == QEvent::WindowStateChange;
                    }
                } eater;
                frame->installEventFilter(&eater);
                requests();
                win->set_fullscreen(true);
                win->set_fullscreen(false);
                frame->setWindowState(frame->windowState() | Qt::WindowFullScreen);  // the first's answer, kept
                eater.on = false;
                QWindowStateChangeEvent answered(frame->windowState() & ~Qt::WindowFullScreen);
                QCoreApplication::sendEvent(frame, &answered);  // told
                frame->removeEventFilter(&eater);
                win->set_fullscreen(true);
                r = requests();
                expect(r == "FUF", "full screen: on, off, the first answered, on again: asked again, in order (" + r +
                                       ")");
                win->set_fullscreen(false);
                settle();
                expect(!win->fullscreen(), "full screen: and not full screen after the race");
            }
        }
    }
    // A link label after a new palette (laid out again): its text and link;
    // a slider showing its value at its minimum: shown from the start; and
    // a new range: its handle where the value is on it, its label the same.
    {
        rap::label* link = nullptr;
        rap::label* hidden = nullptr;
        rap::slider* at_min = nullptr;
        auto win2 = (*ui)->create(
            window{"Setters B",
                   {vbox{rap::layout::label{"Go there"}.link([] {}).bind(link),
                         rap::layout::label{"Hidden from the start"}.visible(false).bind(hidden),
                         rap::layout::slider{0, 100, 0}.step(1, 10).show_value(0).bind(at_min)}}}
                .size(400_px, 200_px));
        win2->show();
        settle();
        auto* text = first("Setters B", [](QWidget* w)
                           {
                               auto* l = qobject_cast<QLabel*>(w);
                               return l && l->text().contains("Go there") ? l : nullptr;
                           });
        expect(text != nullptr, "the link label");
        if (text)
        {
            QPalette p = text->palette();
            p.setColor(QPalette::Link, QColor(200, 30, 30));
            text->setPalette(p);
            settle();
            expect(text->text().contains("Go there") && text->text().contains("href"),
                   "a link label after a new palette: its text and link, \"" + text->text().toStdString() + "\"");
        }
        // Made hidden (visible(false)): hidden once its window shows; shown
        // and hidden again as asked.
        {
            auto* h = first("Setters B", [](QWidget* w)
                            {
                                auto* l = qobject_cast<QLabel*>(w);
                                return l && l->text() == "Hidden from the start" ? l : nullptr;
                            });
            expect(h && !h->isVisible(), "a label made hidden stays hidden once its window shows");
            if (h)
            {
                hidden->set_visible(true);
                settle();
                const bool shown_now = h->isVisible();
                hidden->set_visible(false);
                settle();
                expect(shown_now && !h->isVisible(), "then shown, and hidden again, as asked");
            }
        }
        auto* track = first("Setters B", [](QWidget* w) { return qobject_cast<QSlider*>(w); });
        QLabel* shown = track ? track->parentWidget()->findChild<QLabel*>() : nullptr;
        expect(shown && shown->text() == "0", "a slider at its minimum shows its value from the start: \"" +
                                                  (shown ? shown->text().toStdString() : std::string("none")) + "\"");
        if (track && shown)
        {
            at_min->set_value(40);
            at_min->set_range(20, 120);
            settle();
            expect(at_min->value() == 40 && track->value() == 20 && shown->text() == "40",
                   "a new range: the handle where 40 is on it (position " + std::to_string(track->value()) +
                       ", 20 wanted), its label \"" + shown->text().toStdString() + "\"");
        }
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
