// GTK 3: a setter given the value a widget has does nothing (#88): no
// redraw, no resize, no change notification. Each setter's set once to a
// value (not counted), then to the same one again (counted: none of the
// three may come), then to another (a visible one must still draw or
// resize; notices then are #77's, and only noted). The window's drawing ("draw" on the toplevel, which GTK 3 draws
// its children through) and resizing (its "check-resize") are counted,
// off the desktop on Broadway.
//   rapport-setters-noop-test-gtk3
#include <rapport/rapport.h>

#include <gtk/gtk.h>

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

int g_draws = 0, g_resizes = 0, g_notes = 0;

// The main loop run for `ms`: everything queued drawn and laid out.
void settle(int ms = 200)
{
    const gint64 until = g_get_monotonic_time() + gint64(ms) * 1000;
    while (g_get_monotonic_time() < until)
    {
        if (gtk_events_pending())
            gtk_main_iteration_do(false);
        else
            g_usleep(2000);
    }
}

class rows : public rap::list_model
{
public:
    size_t row_count() const override { return 3; }
    std::string text(size_t row, size_t col) const override
    {
        return (col == 0 ? "Name " : "Note ") + std::to_string(row);
    }
};
rows g_rows;

rap::tree_item item(std::string id)
{
    rap::tree_item t;
    t.label = id;
    t.id = std::move(id);
    return t;
}

// `set(a)` once, uncounted; `set(a)` again: no draw, resize or notice;
// `set(b)`: no notice, and (`visible`) a draw or a resize.
template <class T>
void probe(const char* what, const std::function<void(const T&)>& set, const T& a, const T& b, bool visible = true)
{
    set(a);
    // Long enough for the theme's transitions (a toggle's, a frame's) to end.
    settle(1200);
    g_draws = g_resizes = g_notes = 0;
    set(a);
    settle();
    expect(g_draws == 0 && g_resizes == 0 && g_notes == 0,
           std::string(what) + ", unchanged: nothing (" + std::to_string(g_draws) + " draws, " +
               std::to_string(g_resizes) + " resizes, " + std::to_string(g_notes) + " notices)");
    g_draws = g_resizes = g_notes = 0;
    set(b);
    settle();
    // (Whether a changed value's setter tells the app is #77's; noted only.)
    if (g_notes) std::printf("note %s, changed: %d notices\n", what, g_notes);
    if (visible)
        expect(g_draws > 0 || g_resizes > 0, std::string(what) + ", changed: drawn or laid out again (" +
                                                  std::to_string(g_draws) + " draws, " +
                                                  std::to_string(g_resizes) + " resizes)");
}

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options options;
    options.backend = "gtk3";
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui)
    {
        std::printf("FAIL no gtk3 backend: %s\n", ui.error().c_str());
        return 1;
    }
    rap::label* label_h = nullptr;
    rap::button* button_h = nullptr;
    rap::check* check_box_h = nullptr;
    rap::entry* entry_h = nullptr;
    rap::spin* spin_h = nullptr;
    rap::slider* slider_h = nullptr;
    rap::progress_bar* progress_h = nullptr;
    rap::drop_down* drop_h = nullptr;
    rap::combo* combo_h = nullptr;
    rap::text_view* text_h = nullptr;
    rap::list* list_h = nullptr;
    rap::tree* tree_h = nullptr;
    rap::menu_button* menu_h = nullptr;
    rap::tabs* tabs_h = nullptr;
    rap::date_picker* date_h = nullptr;
    rap::color_button* colour_h = nullptr;
    using namespace rap::layout;
    using namespace rap::literals;
    auto note = [] { ++g_notes; };
    auto win = (*ui)->create(window{
        "setters",
        {vbox{hbox{label{"Label"}.bind(label_h), button{"Button"}.bind(button_h).on_click(note),
                   check{"Check"}.bind(check_box_h).on_toggled([&](bool) { note(); }),
                   menu_button{"Menu"}.bind(menu_h)},
              hbox{entry{}.bind(entry_h).on_changed([&](const std::string&) { note(); }).stretch(),
                   spin{0, 100, 5}.bind(spin_h).on_changed([&](double) { note(); }),
                   slider{0, 100, 5}.bind(slider_h).on_changed([&](double) { note(); }).stretch()},
              hbox{progress_bar{}.bind(progress_h).stretch(),
                   drop_down{{"One", "Two", "Three"}, 0}.bind(drop_h).on_changed([&](int) { note(); }),
                   combo{{"one", "two"}, "one"}.bind(combo_h).on_changed([&](const std::string&) { note(); }),
                   date_picker{{2026, 11, 16}}.bind(date_h).on_changed([&](rap::date) { note(); }),
                   color_button{{10, 20, 30, 255}}.bind(colour_h).on_changed([&](rap::color) { note(); })},
              hbox{list{{{"Name", 100_px}, {"Notes", 100_px}}, &g_rows}
                       .bind(list_h)
                       .on_selection_changed(note)
                       .stretch(),
                   tree{{item("a"), item("b")}}.bind(tree_h).on_selection_changed(note).stretch(),
                   text_view{}.bind(text_h).on_changed(note).stretch()}
                  .stretch(),
              tabs{{"One", label{"one"}}, {"Two", label{"two"}}}.bind(tabs_h).on_changed([&](int) { note(); })}}}
                                 .size(900_px, 600_px));
    win->show();
    settle(500);

    GtkWidget* top = nullptr;
    GList* tops = gtk_window_list_toplevels();
    for (GList* l = tops; l; l = l->next)
        if (const char* t = gtk_window_get_title(GTK_WINDOW(l->data)); t && std::string(t) == "setters")
            top = GTK_WIDGET(l->data);
    g_list_free(tops);
    expect(top && label_h && button_h && check_box_h && entry_h && spin_h && slider_h && progress_h && drop_h && combo_h && text_h &&
               list_h && tree_h && menu_h && tabs_h && date_h && colour_h,
           "the window and its widgets");
    if (!top) return 1;
    g_signal_connect(top, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t*, gpointer) -> gboolean {
                         ++g_draws;
                         return false;
                     }),
                     nullptr);
    g_signal_connect(top, "check-resize", G_CALLBACK(+[](GtkContainer*, gpointer) { ++g_resizes; }), nullptr);
    settle();

    using S = std::string;
    probe<S>("label::set_text", [&](const S& v) { label_h->set_text(v); }, "Alpha", "Beta");
    probe<S>("button::set_text", [&](const S& v) { button_h->set_text(v); }, "Go", "Stop");
    probe<rap::icon>("button::set_icon", [&](const rap::icon& v) { button_h->set_icon(v); },
                     rap::icon::named("go-up"), rap::icon::named("go-down"));
    probe<bool>("check::set_checked", [&](const bool& v) { check_box_h->set_checked(v); }, true, false);
    probe<rap::check_state>("check::set_state", [&](const rap::check_state& v) { check_box_h->set_state(v); },
                            rap::check_state::indeterminate, rap::check_state::checked);
    probe<S>("menu_button::set_text", [&](const S& v) { menu_h->set_text(v); }, "Menu", "More");
    probe<bool>("menu_button::set_pushed", [&](const bool& v) { menu_h->set_pushed(v); }, true, false);
    probe<S>("entry::set_text", [&](const S& v) { entry_h->set_text(v); }, "abc", "abcd");
    probe<S>("entry::set_placeholder", [&](const S& v) { entry_h->set_placeholder(v); }, "Find", "Search");
    probe<bool>("entry::set_read_only", [&](const bool& v) { entry_h->set_read_only(v); }, true, false, false);
    probe<bool>("entry::set_frame", [&](const bool& v) { entry_h->set_frame(v); }, false, true);
    probe<double>("spin::set_value", [&](const double& v) { spin_h->set_value(v); }, 10, 20);
    probe<double>("slider::set_value", [&](const double& v) { slider_h->set_value(v); }, 10, 60);
    probe<std::pair<double, double>>("slider::set_range",
                                     [&](const std::pair<double, double>& v) { slider_h->set_range(v.first, v.second); },
                                     {0, 100}, {0, 200});
    probe<float>("progress_bar::set_fraction", [&](const float& v) { progress_h->set_fraction(v); }, 0.25f, 0.75f);
    probe<int>("drop_down::set_current", [&](const int& v) { drop_h->set_current(v); }, 1, 2);
    probe<std::vector<S>>("drop_down::set_items", [&](const std::vector<S>& v) { drop_h->set_items(v); },
                          {"One", "Two", "Three"}, {"Uno", "Dos", "Tres"}, false);
    probe<S>("combo::set_text", [&](const S& v) { combo_h->set_text(v); }, "two", "three");
    probe<rap::date>("date_picker::set_date", [&](const rap::date& v) { date_h->set_date(v); }, {2026, 1, 2},
                     {2026, 3, 4});
    probe<rap::color>("color_button::set_color", [&](const rap::color& v) { colour_h->set_color(v); },
                      {200, 10, 10, 255}, {10, 200, 10, 255});
    probe<S>("text_view::set_text", [&](const S& v) { text_h->set_text(v); }, "Some text", "Other text");
    probe<bool>("text_view::set_read_only", [&](const bool& v) { text_h->set_read_only(v); }, true, false, false);
    probe<size_t>("list::set_current", [&](const size_t& v) { list_h->set_current(v); }, 1, 2);
    probe<std::vector<size_t>>("list::select", [&](const std::vector<size_t>& v) { list_h->select(v); }, {0, 2},
                               {1});
    probe<bool>("list::set_column_visible", [&](const bool& v) { list_h->set_column_visible(1, v); }, false, true);
    probe<bool>("tree::set_expanded", [&](const bool& v) { tree_h->set_expanded("a", v); }, false, false, false);
    probe<int>("tabs::set_current", [&](const int& v) { tabs_h->set_current(v); }, 1, 0);
    probe<bool>("widget::set_enabled", [&](const bool& v) { button_h->set_enabled(v); }, false, true);
    probe<bool>("widget::set_visible", [&](const bool& v) { check_box_h->set_visible(v); }, false, true);
    probe<S>("widget::set_tooltip", [&](const S& v) { label_h->set_tooltip(v); }, "Tip", "Other tip", false);
    probe<S>("window::set_title", [&](const S& v) { win->set_title(v); }, "setters", "setters again", false);

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
