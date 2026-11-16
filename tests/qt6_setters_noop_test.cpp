// A setter given the value it has does nothing on Qt (#88): no layout
// request, no repaint, no change handler fired. One of each widget in a
// window shown off the desktop (QT_QPA_PLATFORM=offscreen); each setter set
// to a value, then set to it again with every QEvent::LayoutRequest,
// UpdateRequest and Paint in the application counted: none. Then a new
// value still does as before (where it shows: some events).
//   rapport-setters-noop-test-qt6   (QT_QPA_PLATFORM=offscreen)
#include <rapport/rapport.h>

#include <QApplication>
#include <QEvent>
#include <QThread>

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
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
