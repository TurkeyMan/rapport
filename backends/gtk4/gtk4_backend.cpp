// GTK 4 backend (package rapport-gtk4).
//
// Lists use GtkColumnView over a lazy GListModel (RapportRows) whose items
// are bare GObjects tagged with their row index; cells pull text from the
// rap::list_model at bind time. Painted columns are GtkDrawingAreas.
#include <gtk/gtk.h>

#include "builtin_icons.h"
#include "gtk_common.h"

#include <memory>

// -- RapportRows --------------------------------------------------------------

G_BEGIN_DECLS
G_DECLARE_FINAL_TYPE(RapportRows, rapport_rows, RAPPORT, ROWS, GObject)
G_END_DECLS

struct _RapportRows
{
    GObject parent_instance;
    guint count;
};

static GType rapport_rows_get_item_type(GListModel*)
{
    return G_TYPE_OBJECT;
}
static guint rapport_rows_get_n_items(GListModel* m)
{
    return RAPPORT_ROWS(m)->count;
}

static gpointer rapport_rows_get_item(GListModel* m, guint pos)
{
    if (pos >= RAPPORT_ROWS(m)->count) return nullptr;
    auto* item = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
    g_object_set_data(item, "rapport-row", GUINT_TO_POINTER(pos));
    return item;
}

static void rapport_rows_iface_init(GListModelInterface* iface)
{
    iface->get_item_type = rapport_rows_get_item_type;
    iface->get_n_items = rapport_rows_get_n_items;
    iface->get_item = rapport_rows_get_item;
}

G_DEFINE_TYPE_WITH_CODE(RapportRows, rapport_rows, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, rapport_rows_iface_init))

static void rapport_rows_class_init(RapportRowsClass*) {}
static void rapport_rows_init(RapportRows* self)
{
    self->count = 0;
}

namespace rap::gtk4
{
namespace
{

using namespace gtkcommon;

// TODO: take accent and contrast from the settings portal.
}  // namespace
}  // namespace rap::gtk4

// The theme's named colours, as GTK 3's: libadwaita's names first, then the
// classic ones GTK's own theme still defines. The lookup is deprecated in
// GTK 4 with nothing to replace it for named colours, so it's kept until
// one exists. A name that resolves transparent isn't a colour.
rap::color rap::gtkcommon::theme_color_of(GtkWidget* w, theme_color role)
{
    GdkRGBA fg;
    gtk_widget_get_color(w, &fg);
    std::array<const char*, 2> names{};  // most preferred first; unused are null
    switch (role)
    {
    case theme_color::window: names = {"window_bg_color", "theme_bg_color"}; break;
    case theme_color::window_text: names = {"window_fg_color", "theme_fg_color"}; break;
    case theme_color::base: names = {"view_bg_color", "theme_base_color"}; break;
    case theme_color::text: names = {"view_fg_color", "theme_text_color"}; break;
    case theme_color::highlight:
    case theme_color::accent: names = {"accent_bg_color", "theme_selected_bg_color"}; break;
    case theme_color::highlight_text: names = {"accent_fg_color", "theme_selected_fg_color"}; break;
    case theme_color::positive: names = {"success_color"}; break;
    case theme_color::negative: names = {"error_color"}; break;
    case theme_color::link: names = {"link_color"}; break;
    default: break;
    }
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkStyleContext* ctx = gtk_widget_get_style_context(w);
    for (const char* name : names)
    {
        GdkRGBA c;
        if (name && gtk_style_context_lookup_color(ctx, name, &c) && c.alpha > 0)
        {
            G_GNUC_END_IGNORE_DEPRECATIONS
            return to_color(c);
        }
    }
    G_GNUC_END_IGNORE_DEPRECATIONS
    return fallback_color(role, to_color(fg), is_dark_text(fg));
}

namespace rap::gtk4
{
namespace
{

// -- widgets ------------------------------------------------------------------

// A container (box, form, group, toolbar) as a plain Widget, for handles.
class plain_impl : public widget_base<rap::widget>
{
public:
    using widget_base::widget_base;
};

class label_impl : public widget_base<rap::label>
{
public:
    explicit label_impl(const spec::label& s)
        : widget_base(gtk_label_new(s.text.c_str()))
    {
        gtk_label_set_xalign(GTK_LABEL(w_), 0);
        gtk_label_set_selectable(GTK_LABEL(w_), s.selectable);
        if (s.ellipsize) gtk_label_set_ellipsize(GTK_LABEL(w_), PANGO_ELLIPSIZE_END);
        if (s.link)
        {
            // A link in the label's markup: GTK's own link look, focus and
            // keyboard activation; activate-link runs it instead of a URI.
            link_ = true;
            on_click_ = s.on_click;
            set_text(s.text);
            g_signal_connect(w_, "activate-link",
                             G_CALLBACK(+[](GtkLabel*, gchar*, gpointer p) -> gboolean
                                        {
                                            auto* self = static_cast<label_impl*>(p);
                                            if (self->on_click_) self->on_click_();
                                            return true;
                                        }),
                             this);
        }
        if (s.icon) add_icon(s.icon);
    }

    void set_text(const std::string& t) override
    {
        if (!link_) return gtk_label_set_text(GTK_LABEL(w_), t.c_str());
        gchar* escaped = g_markup_escape_text(t.c_str(), -1);
        gtk_label_set_markup(GTK_LABEL(w_),
                             (std::string("<a href=\"rapport:\">") + escaped + "</a>").c_str());
        g_free(escaped);
    }

private:
    // The icon before the text, in a box that becomes the outer widget; on a
    // link it takes the link's click and pointer.
    void add_icon(const icon& icon);

    bool link_ = false;
    std::function<void()> on_click_;
};

GtkWidget* icon_widget(const icon& icon, int size);

void label_impl::add_icon(const icon& icon)
{
    GtkWidget* img = icon_widget(icon, 16);
    if (!img) return;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(box), img);
    gtk_box_append(GTK_BOX(box), w_);
    outer_ = box;
    if (!link_) return;
    gtk_widget_set_cursor_from_name(img, "pointer");
    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "released",
                     G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer p)
                                {
                                    auto* self = static_cast<label_impl*>(p);
                                    if (self->on_click_) self->on_click_();
                                }),
                     this);
    gtk_widget_add_controller(img, GTK_EVENT_CONTROLLER(click));
}

// A button's content: its text, or its icon (named or drawn), or the icon
// before the text in a box, as GTK 3's image buttons show them (GTK 4's
// button holds one child).
GtkWidget* icon_and_text(const icon& icon, const std::string& text)
{
    GtkWidget* img = icon ? icon_widget(icon, 16) : nullptr;
    if (!img)
    {
        GtkWidget* label = gtk_label_new_with_mnemonic(text.c_str());
        return label;
    }
    if (text.empty()) return img;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(box), img);
    gtk_box_append(GTK_BOX(box), gtk_label_new_with_mnemonic(text.c_str()));
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    return box;
}

class button_impl : public widget_base<rap::button>
{
public:
    explicit button_impl(const std::string& text)
        : widget_base(gtk_button_new_with_mnemonic(text.c_str()))
        , text_(text)
    {
        connect(
            w_, "clicked",
            +[](GtkButton*, gpointer p)
            {
                auto* self = static_cast<button_impl*>(p);
                if (self->click_) self->click_();
            },
            this);
    }

    void set_text(const std::string& t) override
    {
        text_ = t;
        update();
    }
    // Named or drawn: a named icon the theme lacks falls back to the drawing.
    void set_icon(const icon& icon) override
    {
        icon_ = icon;
        update();
    }
    void on_click(std::function<void()> f) override { click_ = std::move(f); }

private:
    void update() { gtk_button_set_child(GTK_BUTTON(w_), icon_and_text(icon_, text_)); }

    std::string text_;
    icon icon_;
    std::function<void()> click_;
};

// A named icon the theme has; otherwise a drawing area (rapport's own
// drawing of that name, else icon::draw) that runs whenever it paints, so
// drawn icons follow scale and theme (the area takes its button's CSS
// colour, greyed when insensitive); else the theme's image-missing, never
// a blank. Null for no icon at all.
GtkWidget* icon_widget(const icon& icon, int size)
{
    GtkIconTheme* theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    auto named = [&](const char* name)
    {
        GtkWidget* img = gtk_image_new_from_icon_name(name);
        gtk_image_set_pixel_size(GTK_IMAGE(img), size);
        return img;
    };
    if (!icon.name.empty() && gtk_icon_theme_has_icon(theme, icon.name.c_str()))
        return named(icon.name.c_str());
    using draw_fn = std::function<void(rap::painter&, rap::size_f)>;
    draw_fn draw = icon.draw;
    if (!icon.name.empty() && builtin::has_builtin_icon(icon.name))
        draw = [name = icon.name](rap::painter& p, rap::size_f s) { builtin::draw_builtin_icon(name, p, s); };
    if (!draw) return icon.name.empty() ? nullptr : named("image-missing");
    GtkWidget* area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), size);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), size);
    // Square and centred, however tall the toolbar row is.
    gtk_widget_set_halign(area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(area, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(area),
        +[](GtkDrawingArea* a, cairo_t* cr, int w, int h, gpointer p)
        {
            cairo_painter painter(cr, GTK_WIDGET(a));
            (*static_cast<draw_fn*>(p))(painter, {float(w), float(h)});
        },
        new draw_fn(std::move(draw)), +[](gpointer p) { delete static_cast<draw_fn*>(p); });
    return area;
}

// GTK 4 has no GtkToolbar: a flat button in a box. Written without a GTK 4
// build. TODO(tortilla): toggles' pressed look.
class tool_button_impl : public widget_base<rap::tool_button>
{
public:
    explicit tool_button_impl(const spec::tool_button& s)
        : widget_base(s.toggle ? gtk_toggle_button_new() : gtk_button_new())
        , toggle_(s.toggle)
    {
        gtk_button_set_has_frame(GTK_BUTTON(w_), false);
        gtk_widget_set_tooltip_text(w_, s.label.c_str());
        set_icon(s.icon);
        if (toggle_) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w_), s.checked);
        connect(
            w_, "clicked",
            +[](GtkButton*, gpointer p)
            {
                auto* self = static_cast<tool_button_impl*>(p);
                if (self->quiet_) return;
                if (self->click_) self->click_();
                if (self->toggle_ && self->toggled_) self->toggled_(self->checked());
            },
            this);
    }
    void set_icon(const icon& icon) override
    {
        if (GtkWidget* child = icon_widget(icon, 16)) gtk_button_set_child(GTK_BUTTON(w_), child);
    }
    bool checked() const override
    {
        return toggle_ && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w_));
    }
    void set_checked(bool c) override
    {
        if (!toggle_) return;
        quiet_ = true;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w_), c);
        quiet_ = false;
    }
    void on_click(std::function<void()> f) override { click_ = std::move(f); }
    void on_toggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }

private:
    bool toggle_;
    bool quiet_ = false;
    std::function<void()> click_;
    std::function<void(bool)> toggled_;
};

// GtkScale and GtkSpinButton keep their GTK 3 API in GTK 4.
template <class Base>
class value_impl : public widget_base<Base>
{
public:
    explicit value_impl(GtkWidget* w)
        : widget_base<Base>(w)
    {
        this->connect(
            w, "value-changed",
            +[](GtkWidget*, gpointer p)
            {
                auto* self = static_cast<value_impl*>(p);
                if (!self->quiet_ && self->changed_) self->changed_(self->value());
            },
            this);
    }
    double value() const override { return gtk_range_or_spin_value(this->w_); }
    void set_value(double v) override
    {
        quiet_ = true;
        if (GTK_IS_SPIN_BUTTON(this->w_))
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(this->w_), v);
        else
            gtk_range_set_value(GTK_RANGE(this->w_), v);
        quiet_ = false;
    }
    void set_range(double min, double max) override
    {
        quiet_ = true;
        if (GTK_IS_SPIN_BUTTON(this->w_))
            gtk_spin_button_set_range(GTK_SPIN_BUTTON(this->w_), min, max);
        else
            gtk_range_set_range(GTK_RANGE(this->w_), min, max);
        quiet_ = false;
    }
    void on_changed(std::function<void(double)> f) override { changed_ = std::move(f); }

private:
    static double gtk_range_or_spin_value(GtkWidget* w)
    {
        return GTK_IS_SPIN_BUTTON(w) ? gtk_spin_button_get_value(GTK_SPIN_BUTTON(w))
                                     : gtk_range_get_value(GTK_RANGE(w));
    }
    bool quiet_ = false;
    std::function<void(double)> changed_;
};

// TODO(tortilla): GtkColorButton is deprecated since GTK 4.10 (GtkColorDialogButton).
class color_button_impl : public widget_base<rap::color_button>
{
public:
    explicit color_button_impl(const spec::color_button& s)
        : widget_base(gtk_color_button_new())
    {
        set_color(s.color);
        gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(w_), s.alpha);
        connect(
            w_, "color-set",
            +[](GtkColorButton*, gpointer p)
            {
                auto* self = static_cast<color_button_impl*>(p);
                if (self->changed_) self->changed_(self->color());
            },
            this);
    }
    rap::color color() const override
    {
        GdkRGBA c;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(w_), &c);
        return to_color(c);
    }
    void set_color(rap::color c) override
    {
        const GdkRGBA rgba{c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(w_), &rgba);
    }
    void on_changed(std::function<void(rap::color)> f) override { changed_ = std::move(f); }

private:
    std::function<void(rap::color)> changed_;
};

class check_impl : public widget_base<rap::check>
{
public:
    explicit check_impl(const spec::check& s)
        : widget_base(gtk_check_button_new_with_mnemonic(s.text.c_str()))
    {
        set_state(s.state);
        connect(
            w_, "toggled",
            +[](GtkCheckButton* b, gpointer p)
            {
                auto* self = static_cast<check_impl*>(p);
                if (self->updating_) return;
                // A click on the mixed state lands on checked.
                if (gtk_check_button_get_inconsistent(b))
                {
                    self->updating_ = true;
                    gtk_check_button_set_inconsistent(b, false);
                    gtk_check_button_set_active(b, true);
                    self->updating_ = false;
                }
                if (self->toggled_) self->toggled_(gtk_check_button_get_active(b));
            },
            this);
    }

    bool checked() const override { return state() == check_state::checked; }
    void set_checked(bool v) override { set_state(v ? check_state::checked : check_state::unchecked); }

    check_state state() const override
    {
        auto* b = GTK_CHECK_BUTTON(w_);
        if (gtk_check_button_get_inconsistent(b)) return check_state::indeterminate;
        return gtk_check_button_get_active(b) ? check_state::checked : check_state::unchecked;
    }

    void set_state(check_state s) override
    {
        auto* b = GTK_CHECK_BUTTON(w_);
        updating_ = true;
        gtk_check_button_set_inconsistent(b, s == check_state::indeterminate);
        gtk_check_button_set_active(b, s == check_state::checked);
        updating_ = false;
    }

    void on_toggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }

private:
    std::function<void(bool)> toggled_;
    bool updating_ = false;
};

// TODO(tortilla): build the menu as a GtkPopoverMenu (like WList's context
// menu) and show the pushed state; untested without GTK 4 headers.
// A rap::menu as a GMenu model plus the actions its items name, for
// popover menus. Built afresh each time a menu opens (so it shows the app's
// state then); the actions call into the Menu kept here, which lives until
// the next build. Separators become GMenu sections.
class menu_actions
{
public:
    menu_actions(GtkWidget* owner, std::string prefix)
        : prefix_(std::move(prefix))
        , group_(g_simple_action_group_new())
    {
        gtk_widget_insert_action_group(owner, prefix_.c_str(), G_ACTION_GROUP(group_));
    }
    ~menu_actions() { g_object_unref(group_); }
    menu_actions(const menu_actions&) = delete;
    menu_actions& operator=(const menu_actions&) = delete;

    // A new model for `menu`; the caller owns the reference.
    GMenu* build(menu menu)
    {
        for (auto& name : names_) g_action_map_remove_action(G_ACTION_MAP(group_), name.c_str());
        names_.clear();
        menu_ = std::move(menu);
        return model(menu_);
    }

private:
    GMenu* model(const menu& items)
    {
        GMenu* top = g_menu_new();
        GMenu* section = g_menu_new();
        auto flush = [&]
        {
            if (g_menu_model_get_n_items(G_MENU_MODEL(section)) > 0)
                g_menu_append_section(top, nullptr, G_MENU_MODEL(section));
            g_object_unref(section);
            section = g_menu_new();
        };
        for (const menu_item& item : items)
        {
            if (item.is_separator())
            {
                flush();
                continue;
            }
            if (!item.children.empty())
            {
                GMenu* sub = model(item.children);
                g_menu_append_submenu(section, item.label.c_str(), G_MENU_MODEL(sub));
                g_object_unref(sub);
                continue;
            }
            std::string name = "i" + std::to_string(names_.size());
            names_.push_back(name);
            // Checkable items (radio ones too) show a check from a boolean
            // state.
            GSimpleAction* action =
                item.checked ? g_simple_action_new_stateful(name.c_str(), nullptr, g_variant_new_boolean(*item.checked))
                             : g_simple_action_new(name.c_str(), nullptr);
            g_simple_action_set_enabled(action, item.enabled && item.action);
            g_signal_connect(action, "activate",
                             G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer p)
                                        {
                                            auto* mi = static_cast<const menu_item*>(p);
                                            if (mi->action) mi->action();
                                        }),
                             const_cast<menu_item*>(&item));
            g_action_map_add_action(G_ACTION_MAP(group_), G_ACTION(action));
            g_object_unref(action);
            GMenuItem* mi = g_menu_item_new(item.label.c_str(), (prefix_ + "." + name).c_str());
            if (!item.shortcut.empty()) g_menu_item_set_attribute(mi, "accel", "s", gtk_accel(item.shortcut).c_str());
            g_menu_append_item(section, mi);
            g_object_unref(mi);
        }
        flush();
        g_object_unref(section);
        return top;
    }

    std::string prefix_;
    GSimpleActionGroup* group_;
    std::vector<std::string> names_;
    menu menu_;
};

// A GtkMenuButton whose menu is built by `fn` each time it opens.
void attach_menu(GtkMenuButton* button, menu_actions* actions, std::function<menu()>* fn)
{
    struct context
    {
        menu_actions* actions;
        std::function<menu()>* fn;
    };
    gtk_menu_button_set_create_popup_func(
        button,
        +[](GtkMenuButton* b, gpointer p)
        {
            auto* c = static_cast<context*>(p);
            GMenu* model = c->actions->build(*c->fn ? (*c->fn)() : menu{});
            gtk_menu_button_set_menu_model(b, G_MENU_MODEL(model));
            g_object_unref(model);
        },
        new context{actions, fn}, +[](gpointer p) { delete static_cast<context*>(p); });
}

class menu_button_impl : public widget_base<rap::menu_button>
{
public:
    explicit menu_button_impl(const spec::menu_button& s)
        : widget_base(gtk_menu_button_new())
        , actions_(w_, "mb")
    {
        gtk_menu_button_set_use_underline(GTK_MENU_BUTTON(w_), true);  // "_View": V is the access key
        set_text(s.text);
        if (s.icon) set_icon(s.icon);
        attach_menu(GTK_MENU_BUTTON(w_), &actions_, &menu_fn_);
    }

    void set_text(const std::string& t) override
    {
        text_ = t;
        if (icon_)
            set_icon(icon_);
        else
            gtk_menu_button_set_label(GTK_MENU_BUTTON(w_), t.c_str());
    }
    void set_icon(const icon& icon) override
    {
        icon_ = icon;
        // Its own child for the icon; the arrow stays, as for a text one.
        gtk_menu_button_set_child(GTK_MENU_BUTTON(w_), icon_and_text(icon_, text_));
        gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(w_), true);
    }
    void set_pushed(bool) override {}
    void on_menu(std::function<menu()> f) override { menu_fn_ = std::move(f); }

private:
    std::string text_;
    icon icon_;
    std::function<menu()> menu_fn_;
    menu_actions actions_;
};

class progress_bar_impl : public widget_base<rap::progress_bar>
{
public:
    progress_bar_impl()
        : widget_base(gtk_progress_bar_new())
    {
        gtk_widget_set_valign(w_, GTK_ALIGN_CENTER);
    }

    void set_fraction(float f) override
    {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w_), std::clamp(f, 0.f, 1.f));
    }
    void pulse() override { gtk_progress_bar_pulse(GTK_PROGRESS_BAR(w_)); }
};

// A menu button showing the date, with a calendar in its popover.
class date_picker_impl : public widget_base<rap::date_picker>
{
public:
    explicit date_picker_impl(const spec::date_picker& s)
        : widget_base(gtk_menu_button_new())
    {
        calendar_ = gtk_calendar_new();
        gtk_calendar_set_show_week_numbers(GTK_CALENDAR(calendar_), true);
        GtkWidget* popover = gtk_popover_new();
        gtk_popover_set_child(GTK_POPOVER(popover), calendar_);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(w_), popover);
        set_date(s.date);

        connect(
            calendar_, "day-selected",
            +[](GtkCalendar* c, gpointer p)
            {
                auto* self = static_cast<date_picker_impl*>(p);
                if (self->updating_) return;
                GDateTime* dt = gtk_calendar_get_date(c);
                self->date_ = {g_date_time_get_year(dt), g_date_time_get_month(dt),
                               g_date_time_get_day_of_month(dt)};
                g_date_time_unref(dt);
                self->update_label();
            },
            this);
        connect(
            popover, "show",
            +[](GtkWidget*, gpointer p)
            {
                auto* self = static_cast<date_picker_impl*>(p);
                self->shown_ = self->date_;
            },
            this);
        connect(
            popover, "closed",
            +[](GtkPopover*, gpointer p)
            {
                auto* self = static_cast<date_picker_impl*>(p);
                if (self->date_ != self->shown_ && self->changed_) self->changed_(self->date_);
            },
            this);
    }

    rap::date date() const override { return date_; }
    void set_date(rap::date d) override
    {
        date_ = d;
        updating_ = true;
        GDateTime* dt = g_date_time_new_local(d.year, d.month, d.day, 0, 0, 0);
        if (dt)
        {
            gtk_calendar_select_day(GTK_CALENDAR(calendar_), dt);
            g_date_time_unref(dt);
        }
        updating_ = false;
        update_label();
    }
    void on_changed(std::function<void(rap::date)> f) override { changed_ = std::move(f); }

private:
    void update_label()
    {
        GDate* d =
            g_date_new_dmy(GDateDay(date_.day), GDateMonth(date_.month), GDateYear(date_.year));
        char buf[64] = "";
        if (g_date_valid(d)) g_date_strftime(buf, sizeof buf, "%x", d);
        g_date_free(d);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(w_), buf);
    }

    GtkWidget* calendar_;
    rap::date date_, shown_;
    std::function<void(rap::date)> changed_;
    bool updating_ = false;
};

class entry_impl : public widget_base<rap::entry>
{
public:
    explicit entry_impl(const spec::entry& s)
        : widget_base(gtk_entry_new())
    {
        if (s.password)
        {
            gtk_entry_set_visibility(GTK_ENTRY(w_), false);
            gtk_entry_set_input_purpose(GTK_ENTRY(w_), GTK_INPUT_PURPOSE_PASSWORD);
        }
        gtk_editable_set_text(GTK_EDITABLE(w_), s.text.c_str());
        if (!s.placeholder.empty()) set_placeholder(s.placeholder);
        connect(
            w_, "changed",
            +[](GtkEditable* e, gpointer p)
            {
                auto* self = static_cast<entry_impl*>(p);
                if (self->changed_) self->changed_(gtk_editable_get_text(e));
            },
            this);
        connect(
            w_, "activate",
            +[](GtkEntry*, gpointer p)
            {
                auto* self = static_cast<entry_impl*>(p);
                if (self->activate_) self->activate_();
            },
            this);
    }

    std::string text() const override { return gtk_editable_get_text(GTK_EDITABLE(w_)); }
    void set_text(const std::string& t) override
    {
        gtk_editable_set_text(GTK_EDITABLE(w_), t.c_str());
    }
    void set_read_only(bool v) override { gtk_editable_set_editable(GTK_EDITABLE(w_), !v); }
    // Frameless keeps the frame, made invisible, rather than
    // gtk_entry_set_has_frame(false): that entry is shorter, so giving it
    // its frame back (TortoiseGit's "new branch") would move the layout.
    void set_frame(bool v) override
    {
        static bool installed = false;
        if (!installed)
        {
            GtkCssProvider* css = gtk_css_provider_new();
            gtk_css_provider_load_from_string(
                css, "entry.rapport-frameless { background: none; border-color: transparent; "
                     "box-shadow: none; outline-color: transparent; }");
            gtk_style_context_add_provider_for_display(gtk_widget_get_display(w_),
                                                       GTK_STYLE_PROVIDER(css),
                                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
            installed = true;
        }
        if (v)
            gtk_widget_remove_css_class(w_, "rapport-frameless");
        else
            gtk_widget_add_css_class(w_, "rapport-frameless");
    }
    void set_placeholder(const std::string& t) override
    {
        gtk_entry_set_placeholder_text(GTK_ENTRY(w_), t.c_str());
    }
    void on_changed(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void()> f) override { activate_ = std::move(f); }

private:
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

class drop_down_impl : public widget_base<rap::drop_down>
{
public:
    explicit drop_down_impl(const spec::drop_down& s)
        : widget_base(gtk_drop_down_new(nullptr, nullptr))
    {
        set_items(s.items);
        set_current(s.current);
        connect(
            w_, "notify::selected",
            +[](GObject*, GParamSpec*, gpointer p)
            {
                auto* self = static_cast<drop_down_impl*>(p);
                if (self->changed_) self->changed_(self->current());
            },
            this);
    }

    void set_items(std::vector<std::string> items) override
    {
        GtkStringList* list = gtk_string_list_new(nullptr);
        for (auto& i : items) gtk_string_list_append(list, i.c_str());
        gtk_drop_down_set_model(GTK_DROP_DOWN(w_), G_LIST_MODEL(list));
        g_object_unref(list);
    }
    int current() const override
    {
        guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(w_));
        return i == GTK_INVALID_LIST_POSITION ? -1 : int(i);
    }
    void set_current(int i) override
    {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(w_), i < 0 ? GTK_INVALID_LIST_POSITION : guint(i));
    }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }

private:
    std::function<void(int)> changed_;
};

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

// GTK 4 has no non-deprecated editable combo; GtkComboBoxText still works.
class combo_impl : public widget_base<rap::combo>
{
public:
    explicit combo_impl(const spec::combo& s)
        : widget_base(gtk_combo_box_text_new_with_entry())
    {
        entry_ = gtk_combo_box_get_child(GTK_COMBO_BOX(w_));
        set_items(s.items);
        set_text(s.text);
        connect(
            entry_, "changed",
            +[](GtkEditable* e, gpointer p)
            {
                auto* self = static_cast<combo_impl*>(p);
                if (self->changed_) self->changed_(gtk_editable_get_text(e));
            },
            this);
        connect(
            entry_, "activate",
            +[](GtkEntry*, gpointer p)
            {
                auto* self = static_cast<combo_impl*>(p);
                if (self->activate_) self->activate_();
            },
            this);
    }

    void set_items(std::vector<std::string> items) override
    {
        auto* combo = GTK_COMBO_BOX_TEXT(w_);
        gtk_combo_box_text_remove_all(combo);
        for (auto& i : items) gtk_combo_box_text_append_text(combo, i.c_str());
    }
    std::string text() const override { return gtk_editable_get_text(GTK_EDITABLE(entry_)); }
    void set_text(const std::string& t) override
    {
        gtk_editable_set_text(GTK_EDITABLE(entry_), t.c_str());
    }
    void on_changed(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void()> f) override { activate_ = std::move(f); }

private:
    GtkWidget* entry_;
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

G_GNUC_END_IGNORE_DEPRECATIONS

class text_view_impl : public widget_base<rap::text_view>
{
public:
    explicit text_view_impl(const spec::text_view& s)
        : widget_base(gtk_text_view_new(), gtk_scrolled_window_new())
    {
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer_), w_);
        gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(outer_), true);
        auto* tv = GTK_TEXT_VIEW(w_);
        gtk_text_view_set_monospace(tv, s.monospace);
        gtk_text_view_set_wrap_mode(tv, s.wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
        gtk_text_view_set_left_margin(tv, 4);
        // TODO(tortilla): relies on a compact list having installed the
        // display-wide rapport-compact rule; untested without GTK 4 headers.
        if (s.compact) gtk_widget_add_css_class(w_, "rapport-compact");
        gtk_text_view_set_right_margin(tv, 4);
        set_read_only(s.read_only);
        buffer_ = gtk_text_view_get_buffer(tv);
        set_text(s.text);
        connect(
            buffer_, "changed",
            +[](GtkTextBuffer*, gpointer p)
            {
                auto* self = static_cast<text_view_impl*>(p);
                if (self->changed_) self->changed_();
            },
            this);
    }

    std::string text() const override
    {
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buffer_, &a, &b);
        char* s = gtk_text_buffer_get_text(buffer_, &a, &b, false);
        std::string out = s;
        g_free(s);
        return out;
    }

    void set_text(const std::string& t, std::vector<span> spans = {}) override
    {
        gtk_text_buffer_set_text(buffer_, t.c_str(), int(t.size()));
        tags_.apply(buffer_, w_, t, std::move(spans));
    }

    void append(const std::string& t, const text_style& style) override
    {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer_, &end);
        gtk_text_buffer_insert_with_tags(buffer_, &end, t.c_str(), int(t.size()),
                                         tags_.get(buffer_, w_, style), nullptr);
    }

    void set_read_only(bool ro) override
    {
        gtk_text_view_set_editable(GTK_TEXT_VIEW(w_), !ro);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(w_), !ro);
    }

    void on_changed(std::function<void()> f) override { changed_ = std::move(f); }

    // GTK counts characters; the API counts UTF-8 bytes.
    std::pair<size_t, size_t> selection() const override
    {
        GtkTextIter a, b;
        gtk_text_buffer_get_selection_bounds(buffer_, &a, &b);
        const std::string t = text();
        auto bytes = [&](const GtkTextIter& it)
        {
            const char* at = g_utf8_offset_to_pointer(t.c_str(), gtk_text_iter_get_offset(&it));
            return size_t(std::min<ptrdiff_t>(at - t.c_str(), ptrdiff_t(t.size())));
        };
        return {bytes(a), bytes(b)};
    }

    void select(size_t begin, size_t end) override
    {
        const std::string t = text();
        auto iter = [&](size_t byte)
        {
            GtkTextIter it;
            gtk_text_buffer_get_iter_at_offset(
                buffer_, &it,
                int(g_utf8_pointer_to_offset(t.c_str(), t.c_str() + std::min(byte, t.size()))));
            return it;
        };
        GtkTextIter a = iter(begin), b = iter(end);
        gtk_text_buffer_select_range(buffer_, &b, &a);  // the caret at the end
        // A mark, not an iter: scrolling to an iter needs line heights GTK
        // may not have computed yet; a mark scroll waits for them.
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(w_), gtk_text_buffer_get_insert(buffer_), 0.1,
                                     false, 0, 0);
    }

private:
    GtkTextBuffer* buffer_;
    tag_cache tags_;
    std::function<void()> changed_;
};

// A splitter of n panes is a chain of n-1 nested GtkPaneds (each marked
// "rapport-split-chain"); sizes are one per pane, along the split.
class splitter_impl : public widget_base<rap::splitter>
{
public:
    explicit splitter_impl(GtkWidget* paned)
        : widget_base(paned)
    {
    }

    // GtkPaned reports a drag no differently from layout, so a drag is a
    // press and release on a divider with the position changed in between,
    // seen in the capture phase before the paned's own gesture takes them.
    void on_moved(std::function<void()> f) override
    {
        const bool first = !moved_;
        moved_ = std::move(f);
        if (!first) return;
        for (GtkWidget* w = w_; w;)
        {
            GtkEventController* watch = gtk_event_controller_legacy_new();
            gtk_event_controller_set_propagation_phase(watch, GTK_PHASE_CAPTURE);
            connect(
                watch, "event",
                +[](GtkEventControllerLegacy*, GdkEvent* e, gpointer p) -> gboolean
                {
                    auto* self = static_cast<splitter_impl*>(p);
                    const GdkEventType type = gdk_event_get_event_type(e);
                    if (type == GDK_BUTTON_PRESS)
                        self->pressed_ = self->sizes();
                    else if (type == GDK_BUTTON_RELEASE)
                    {
                        if (!self->pressed_.empty() && self->sizes() != self->pressed_ &&
                            self->moved_)
                            self->moved_();
                        self->pressed_.clear();
                    }
                    return false;
                },
                this);
            gtk_widget_add_controller(w, watch);
            GtkWidget* next = gtk_paned_get_end_child(GTK_PANED(w));
            w = next && g_object_get_data(G_OBJECT(next), "rapport-split-chain") ? next : nullptr;
        }
    }

    // The handle is a child of its own between the panes (CSS node
    // "separator"), measured as the theme draws it; it needs the paned in a
    // window, else Adwaita's 1 px, or 5 wide.
    float divider() const override
    {
        GtkPaned* paned = GTK_PANED(w_);
        const GtkWidget* start = gtk_paned_get_start_child(paned);
        const GtkWidget* end = gtk_paned_get_end_child(paned);
        const GtkOrientation along = gtk_orientable_get_orientation(GTK_ORIENTABLE(w_));
        if (gtk_widget_get_root(w_))
            for (GtkWidget* c = gtk_widget_get_first_child(w_); c; c = gtk_widget_get_next_sibling(c))
                if (c != start && c != end)
                {
                    int min = 0, natural = 0;
                    gtk_widget_measure(c, along, -1, &min, &natural, nullptr, nullptr);
                    return float(std::max(min, natural));
                }
        return gtk_paned_get_wide_handle(paned) ? 5.f : 1.f;
    }
    std::vector<float> sizes() const override
    {
        bool vertical =
            gtk_orientable_get_orientation(GTK_ORIENTABLE(w_)) == GTK_ORIENTATION_VERTICAL;
        std::vector<float> out;
        GtkWidget* w = w_;
        for (;;)
        {
            auto* p = GTK_PANED(w);
            out.push_back(float(gtk_paned_get_position(p)));
            GtkWidget* next = gtk_paned_get_end_child(p);
            if (!next || !g_object_get_data(G_OBJECT(next), "rapport-split-chain"))
            {
                out.push_back(next ? float(vertical ? gtk_widget_get_height(next)
                                                    : gtk_widget_get_width(next))
                                   : 0);
                return out;
            }
            w = next;
        }
    }

    // Overrides the first layout from stretch factors. GtkPaned clamps a
    // position to its current length, so one asked for before the window
    // has grown to fit (a pane opening and the window widening for it, a
    // layout restored before the window reaches its saved size) would come
    // out short; each divider is set again as the splitter grows, until its
    // position fits.
    void set_sizes(const std::vector<float>& s) override
    {
        g_object_set_data(G_OBJECT(w_), "rapport-user-sized", GINT_TO_POINTER(1));
        GtkWidget* w = w_;
        for (size_t i = 0; i + 1 < s.size() && w; ++i)
        {
            auto* p = GTK_PANED(w);
            if (s[i] > 0) place_divider(p, int(s[i]));
            GtkWidget* next = gtk_paned_get_end_child(p);
            w = next && g_object_get_data(G_OBJECT(next), "rapport-split-chain") ? next : nullptr;
        }
    }

private:
    std::function<void()> moved_;
    std::vector<float> pressed_;  // the sizes when a button went down on it

    static void place_divider(GtkPaned* p, int position)
    {
        gtk_paned_set_position(p, position);
        int max_pos = 0;
        g_object_get(p, "max-position", &max_pos, nullptr);
        // G_MAXINT: not laid out with both panes yet (one was hidden), so not
        // known to fit either.
        if (max_pos >= position && max_pos != G_MAXINT) return;
        // Too long for now: again when the splitter grows (once it fits).
        auto* want = static_cast<int*>(g_object_get_data(G_OBJECT(p), "rapport-want-position"));
        if (!want)
        {
            want = new int(position);
            g_object_set_data_full(G_OBJECT(p), "rapport-want-position", want, +[](gpointer d) { delete static_cast<int*>(d); });
            g_signal_connect(p, "notify::max-position",
                             G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer)
                                        {
                                            auto* want = static_cast<int*>(g_object_get_data(obj, "rapport-want-position"));
                                            if (!want || *want < 0) return;
                                            int max_pos = 0;
                                            g_object_get(obj, "max-position", &max_pos, nullptr);
                                            if (max_pos == G_MAXINT) return;
                                            gtk_paned_set_position(GTK_PANED(obj), std::min(*want, max_pos));
                                            if (max_pos >= *want) *want = -1;  // there: the user's from here on
                                        }),
                             nullptr);
        }
        *want = position;
    }
};

class tabs_impl : public widget_base<rap::tabs>
{
public:
    explicit tabs_impl(GtkWidget* notebook)
        : widget_base(notebook)
    {
        connect(
            w_, "switch-page",
            +[](GtkNotebook*, GtkWidget*, guint page, gpointer p)
            {
                auto* self = static_cast<tabs_impl*>(p);
                self->ensure_built(int(page));
                if (self->changed_) self->changed_(int(page));
            },
            this);
    }

    int current() const override { return gtk_notebook_get_current_page(GTK_NOTEBOOK(w_)); }
    void set_current(int i) override { gtk_notebook_set_current_page(GTK_NOTEBOOK(w_), i); }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }

    // Lazy pages: an empty box each, filled the first time it's shown.
    void set_lazy(std::vector<std::function<node()>> lazy, std::vector<GtkWidget*> pages,
                 std::function<GtkWidget*(const node&)> build)
    {
        lazy_ = std::move(lazy);
        pages_ = std::move(pages);
        build_ = std::move(build);
        ensure_built(current());
    }
    void ensure_built(int i)
    {
        if (i < 0 || size_t(i) >= lazy_.size() || !lazy_[size_t(i)]) return;
        const auto build = std::move(lazy_[size_t(i)]);
        lazy_[size_t(i)] = nullptr;
        GtkWidget* w = build_(build());
        gtk_widget_set_hexpand(w, TRUE);
        gtk_widget_set_vexpand(w, TRUE);
        gtk_box_append(GTK_BOX(pages_[size_t(i)]), w);
    }

private:
    std::function<void(int)> changed_;
    std::vector<std::function<node()>> lazy_;
    std::vector<GtkWidget*> pages_;
    std::function<GtkWidget*(const node&)> build_;
};

// Tabs as a list of page names down the left, the page beside it.
class side_tabs_impl : public widget_base<rap::tabs>
{
public:
    side_tabs_impl(GtkWidget* box, GtkWidget* list, GtkWidget* stack)
        : widget_base(box)
        , list_(list)
        , stack_(stack)
    {
        connect(
            list_, "row-selected",
            +[](GtkListBox*, GtkListBoxRow* row, gpointer p)
            {
                auto* self = static_cast<side_tabs_impl*>(p);
                if (!row) return;
                const int i = gtk_list_box_row_get_index(row);
                self->ensure_built(i);
                gtk_stack_set_visible_child_name(GTK_STACK(self->stack_), std::to_string(i).c_str());
                if (self->changed_) self->changed_(i);
            },
            this);
    }

    int current() const override
    {
        GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list_));
        return row ? gtk_list_box_row_get_index(row) : 0;
    }
    void set_current(int i) override { gtk_list_box_select_row(GTK_LIST_BOX(list_), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), i)); }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }

    // Lazy pages: an empty box each, filled the first time it's shown.
    void set_lazy(std::vector<std::function<node()>> lazy, std::vector<GtkWidget*> pages,
                 std::function<GtkWidget*(const node&)> build)
    {
        lazy_ = std::move(lazy);
        pages_ = std::move(pages);
        build_ = std::move(build);
        ensure_built(current());
    }
    void ensure_built(int i)
    {
        if (i < 0 || size_t(i) >= lazy_.size() || !lazy_[size_t(i)]) return;
        const auto build = std::move(lazy_[size_t(i)]);
        lazy_[size_t(i)] = nullptr;
        GtkWidget* w = build_(build());
        gtk_widget_set_hexpand(w, TRUE);
        gtk_widget_set_vexpand(w, TRUE);
        gtk_box_append(GTK_BOX(pages_[size_t(i)]), w);
    }

private:
    GtkWidget* list_;
    GtkWidget* stack_;
    std::function<void(int)> changed_;
    std::vector<std::function<node()>> lazy_;
    std::vector<GtkWidget*> pages_;
    std::function<GtkWidget*(const node&)> build_;
};

// -- list ---------------------------------------------------------------------

class list_impl : public widget_base<rap::list>, public list_observer
{
public:
    explicit list_impl(const spec::list& s)
        : widget_base(nullptr, gtk_scrolled_window_new())
        , checks_(s.checkboxes)
    {
        rows_ = RAPPORT_ROWS(g_object_new(rapport_rows_get_type(), nullptr));
        auto* rows = G_LIST_MODEL(rows_);  // ownership passes to the selection model
        switch (s.selection)
        {
        case selection_mode::none: sel_ = GTK_SELECTION_MODEL(gtk_no_selection_new(rows)); break;
        case selection_mode::single:
        {
            auto* single = gtk_single_selection_new(rows);
            gtk_single_selection_set_autoselect(single, false);
            gtk_single_selection_set_can_unselect(single, true);
            sel_ = GTK_SELECTION_MODEL(single);
            break;
        }
        case selection_mode::multi: sel_ = GTK_SELECTION_MODEL(gtk_multi_selection_new(rows)); break;
        }

        w_ = gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(sel_)));
        if (checks_)
        {
            // Space toggles the selected row's box (the column view has no
            // cursor to ask; the model applies it to the selection).
            GtkEventController* space = gtk_event_controller_key_new();
            gtk_event_controller_set_propagation_phase(space, GTK_PHASE_CAPTURE);
            g_signal_connect(
                space, "key-pressed",
                G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType mods,
                               gpointer p) -> gboolean
                           {
                               auto* self = static_cast<list_impl*>(p);
                               if (key != GDK_KEY_space ||
                                   (mods & gtk_accelerator_get_default_mod_mask()) || !self->model_)
                                   return false;
                               GtkBitset* sel = gtk_selection_model_get_selection(self->sel_);
                               if (!gtk_bitset_is_empty(sel))
                                   self->model_->toggle_checked(gtk_bitset_get_minimum(sel));
                               gtk_bitset_unref(sel);
                               return true;
                           }),
                this);
            gtk_widget_add_controller(w_, space);
        }
        auto* view = GTK_COLUMN_VIEW(w_);
        gtk_column_view_set_show_column_separators(view, true);
        if (s.compact)
        {
            // TODO(tortilla): untested without GTK 4 headers. One display-wide
            // rule, matching only lists marked compact.
            static bool installed = false;
            if (!installed)
            {
                std::string px = std::to_string(compact_font(w_).px) + "px";
                // Only the font: the row padding is the theme's (data-table
                // below), so rows aren't squeezed past what it allows.
                std::string rule =
                    "columnview.rapport-compact, textview.rapport-compact { font-size: " + px +
                    "; }";
                GtkCssProvider* css = gtk_css_provider_new();
                gtk_css_provider_load_from_string(css, rule.c_str());
                gtk_style_context_add_provider_for_display(gtk_widget_get_display(w_),
                                                           GTK_STYLE_PROVIDER(css),
                                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
                g_object_unref(css);
                installed = true;
            }
            gtk_widget_add_css_class(w_, "rapport-compact");
            // The theme's own dense rows for tables of data: a little
            // padding rather than a rich list's, and none taken away.
            gtk_widget_add_css_class(w_, "data-table");
        }
        if (s.soft_selection)
        {
            // The colours computed as gtk3 does, written into the rule:
            // @accent_bg_color isn't defined by every GTK 4 theme, and an
            // unresolved colour left selected rows with no background.
            soft_ = soft_selection_colors(w_);
            static bool installed = false;
            if (!installed)
            {
                auto rgb = [](color c)
                {
                    return "rgb(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," +
                           std::to_string(c.b) + ")";
                };
                // The row under the pointer gets a light tint, as on gtk3 and
                // win32; GTK 4 already gives rows :hover.
                color base = theme_color_of(w_, theme_color::base), accent = theme_color_of(w_, theme_color::accent);
                const bool dark = base.r * 30 + base.g * 59 + base.b * 11 < 12800;
                const color hover = mix_colors(base, accent, dark ? 0.16f : 0.09f);
                std::string rule = "columnview.rapport-soft row:hover { background-color: " + rgb(hover) +
                                   "; } columnview.rapport-soft row:selected, columnview.rapport-soft row:selected:focus, "
                                   "columnview.rapport-soft row:selected:hover { background-color: " +
                                   rgb(soft_->first) + "; color: " + rgb(soft_->second) +
                                   "; } columnview row.rapport-drop { box-shadow: inset 0 0 0 2px " +
                                   rgb(theme_color_of(w_, theme_color::highlight)) + "; }";
                GtkCssProvider* css = gtk_css_provider_new();
                gtk_css_provider_load_from_string(css, rule.c_str());
                gtk_style_context_add_provider_for_display(gtk_widget_get_display(w_),
                                                           GTK_STYLE_PROVIDER(css),
                                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
                g_object_unref(css);
                installed = true;
            }
            gtk_widget_add_css_class(w_, "rapport-soft");
        }
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer_), w_);
        gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(outer_), true);

        for (size_t i = 0; i < s.columns.size(); ++i) add_column(s.columns[i], i);

        ctx_menu_ = std::make_unique<menu_actions>(w_, "ctx");
        if (s.auto_scroll) enable_auto_scroll();

        connect(
            sel_, "selection-changed",
            +[](GtkSelectionModel*, guint, guint, gpointer p)
            {
                auto* self = static_cast<list_impl*>(p);
                // Painted cells draw with the selection's colours.
                for (auto [li, ctx] : self->bound_)
                    if (ctx->painted) gtk_widget_queue_draw(gtk_list_item_get_child(li));
                if (self->sel_changed_) self->sel_changed_();
            },
            this);
        connect(
            w_, "activate",
            +[](GtkColumnView*, guint row, gpointer p)
            {
                auto* self = static_cast<list_impl*>(p);
                if (self->activate_) self->activate_(row);
            },
            this);
        connect(
            gtk_column_view_get_sorter(view), "changed",
            +[](GtkSorter* sorter, GtkSorterChange, gpointer p)
            { static_cast<list_impl*>(p)->sort_changed(GTK_COLUMN_VIEW_SORTER(sorter)); },
            this);
    }

    ~list_impl() override
    {
        if (model_) model_->remove_observer(this);
        g_object_unref(sel_);
    }

    void set_model(list_model* m) override
    {
        if (model_) model_->remove_observer(this);
        model_ = m;
        if (model_) model_->add_observer(this);
        model_reset();
    }

    std::vector<size_t> selection() const override
    {
        std::vector<size_t> out;
        GtkBitset* set = gtk_selection_model_get_selection(sel_);
        GtkBitsetIter it;
        guint v;
        for (bool ok = gtk_bitset_iter_init_first(&it, set, &v); ok;
             ok = gtk_bitset_iter_next(&it, &v))
            out.push_back(v);
        gtk_bitset_unref(set);
        return out;
    }

    std::optional<size_t> current() const override
    {
        auto s = selection();
        if (s.empty()) return std::nullopt;
        return s.front();
    }

    void select(const std::vector<size_t>& rows) override
    {
        gtk_selection_model_unselect_all(sel_);
        for (size_t r : rows) gtk_selection_model_select_item(sel_, guint(r), false);
    }

    void set_current(size_t row) override
    {
        gtk_column_view_scroll_to(
            GTK_COLUMN_VIEW(w_), guint(row), nullptr,
            GtkListScrollFlags(GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT), nullptr);
    }

    void scroll_to(size_t row) override
    {
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(w_), guint(row), nullptr, GTK_LIST_SCROLL_NONE,
                                  nullptr);
    }

    void set_sort_indicator(std::optional<size_t> column, sort_order order) override
    {
        suppress_sort_ = true;
        GtkColumnViewColumn* col = column && *column < cols_.size() ? cols_[*column] : nullptr;
        gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(w_), col,
                                       order == sort_order::ascending ? GTK_SORT_ASCENDING
                                                                     : GTK_SORT_DESCENDING);
        suppress_sort_ = false;
    }

    std::vector<float> column_widths() const override
    {
        std::vector<float> out;
        for (auto* c : cols_) out.push_back(float(gtk_column_view_column_get_fixed_width(c)));
        return out;
    }

    std::vector<size_t> column_order() const override
    {
        std::vector<size_t> out;
        GListModel* shown = gtk_column_view_get_columns(GTK_COLUMN_VIEW(w_));
        for (guint i = 0; i < g_list_model_get_n_items(shown); ++i)
        {
            auto* c = static_cast<GtkColumnViewColumn*>(g_list_model_get_item(shown, i));
            if (auto it = std::ranges::find(cols_, c); it != cols_.end())
                out.push_back(size_t(it - cols_.begin()));
            g_object_unref(c);
        }
        return out;
    }
    void set_column_order(const std::vector<size_t>& order) override
    {
        if (!is_column_order(order, cols_.size())) return;
        // Inserting a column the view already has moves it.
        for (size_t pos = 0; pos < order.size(); ++pos)
            gtk_column_view_insert_column(GTK_COLUMN_VIEW(w_), guint(pos), cols_[order[pos]]);
    }

    void set_column_widths(const std::vector<float>& widths) override
    {
        for (size_t i = 0; i < widths.size() && i < cols_.size(); ++i)
            if (widths[i] > 0) gtk_column_view_column_set_fixed_width(cols_[i], int(widths[i]));
    }

    void on_selection_changed(std::function<void()> f) override { sel_changed_ = std::move(f); }
    void on_activate(std::function<void(size_t)> f) override { activate_ = std::move(f); }
    void on_sort_requested(std::function<void(size_t, sort_order)> f) override
    {
        sort_ = std::move(f);
    }
    void on_context_menu(std::function<menu(std::optional<size_t>)> f) override
    {
        context_menu_ = std::move(f);
    }
    // TODO(tortilla): header right-click menu (gesture on the header row).
    void on_header_menu(std::function<menu(size_t)> f) override { header_menu_ = std::move(f); }
    void on_cell_context_menu(std::function<menu(size_t, size_t, point_f)> f) override { cell_menu_ = std::move(f); }
    // TODO(tortilla): drag-reordering (GtkDragSource on the rows).
    void on_reorder(reorder_fn) override {}
    void set_reorderable(bool) override {}
    // Autoscroll (spec.auto_scroll): with the middle button held, a tick
    // callback scrolls by the pointer's offset from the anchor. (The anchor
    // isn't drawn on gtk4 yet: the cursor shows the mode.)
    void enable_auto_scroll()
    {
        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_MIDDLE);
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
        g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int, double, double y, gpointer p)
                                                       {
                                                           auto* self = static_cast<list_impl*>(p);
                                                           gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
                                                           self->start_auto_scroll(y);
                                                       }),
                         this);
        g_signal_connect(click, "released", G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer p)
                                                        { static_cast<list_impl*>(p)->stop_auto_scroll(); }),
                         this);
        g_signal_connect(click, "cancel", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer p)
                                                      { static_cast<list_impl*>(p)->stop_auto_scroll(); }),
                         this);
        gtk_widget_add_controller(w_, GTK_EVENT_CONTROLLER(click));
        GtkEventController* motion = gtk_event_controller_motion_new();
        g_signal_connect(motion, "motion", G_CALLBACK(+[](GtkEventControllerMotion*, double, double y, gpointer p)
                                                       { static_cast<list_impl*>(p)->auto_y = y; }),
                         this);
        gtk_widget_add_controller(w_, motion);
    }
    void start_auto_scroll(double y)
    {
        stop_auto_scroll();
        auto_y0 = auto_y = y;
        auto_last = 0;
        auto_tick = gtk_widget_add_tick_callback(
            w_,
            +[](GtkWidget*, GdkFrameClock* clock, gpointer p) -> gboolean
            {
                auto* self = static_cast<list_impl*>(p);
                const gint64 now = gdk_frame_clock_get_frame_time(clock);
                const double dt = self->auto_last ? (now - self->auto_last) / 1e6 : 0;
                self->auto_last = now;
                GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->outer_));
                const double v = gtk_adjustment_get_value(adj) + auto_scroll_speed(float(self->auto_y - self->auto_y0)) * dt;
                gtk_adjustment_set_value(adj, std::clamp(v, gtk_adjustment_get_lower(adj),
                                                         gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj)));
                return G_SOURCE_CONTINUE;
            },
            this, nullptr);
        gtk_widget_set_cursor_from_name(w_, "all-scroll");
    }
    void stop_auto_scroll()
    {
        if (!auto_tick) return;
        gtk_widget_remove_tick_callback(w_, auto_tick);
        auto_tick = 0;
        gtk_widget_set_cursor(w_, nullptr);
    }
    guint auto_tick = 0;
    double auto_y0 = 0, auto_y = 0;
    gint64 auto_last = 0;

    // cell_drag: a drag gesture on the column view. The cell under a point
    // is found by picking the widget there; the target row is framed with
    // a CSS class. (No floating label on gtk4 yet: the cursor and the frame
    // show the drag.)
    void on_cell_drag(cell_drag d) override
    {
        if (!cell_drag_.begin)
        {
            GtkGesture* drag = gtk_gesture_drag_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
            g_signal_connect(drag, "drag-begin", G_CALLBACK(+[](GtkGestureDrag* g, double x, double y, gpointer p)
                                                            { static_cast<list_impl*>(p)->cell_drag_begin(g, x, y); }),
                             this);
            g_signal_connect(drag, "drag-update", G_CALLBACK(+[](GtkGestureDrag* g, double dx, double dy, gpointer p)
                                                             { static_cast<list_impl*>(p)->cell_drag_update(g, dx, dy); }),
                             this);
            g_signal_connect(drag, "drag-end", G_CALLBACK(+[](GtkGestureDrag*, double, double, gpointer p)
                                                          { static_cast<list_impl*>(p)->cell_drag_end(true); }),
                             this);
            gtk_widget_add_controller(w_, GTK_EVENT_CONTROLLER(drag));
            GtkEventController* keys = gtk_event_controller_key_new();
            g_signal_connect(keys, "key-pressed",
                             G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType, gpointer p) -> gboolean
                                        {
                                            auto* self = static_cast<list_impl*>(p);
                                            if (key != GDK_KEY_Escape || !self->cell_.dragging) return false;
                                            self->cell_drag_end(false);
                                            return true;
                                        }),
                             this);
            gtk_widget_add_controller(w_, keys);
        }
        cell_drag_ = std::move(d);
    }

    bool column_visible(size_t column) const override
    {
        return column < cols_.size() && gtk_column_view_column_get_visible(cols_[column]);
    }
    void set_column_visible(size_t column, bool v) override
    {
        if (column < cols_.size()) gtk_column_view_column_set_visible(cols_[column], v);
    }

    // list_observer
    void model_reset() override
    {
        guint old = rows_->count;
        rows_->count = model_ ? guint(model_->row_count()) : 0;
        g_list_model_items_changed(G_LIST_MODEL(rows_), 0, old, rows_->count);
    }
    // Changed rows are refreshed in the cells showing them. Reporting them
    // to GTK as removed and re-added (items-changed) dropped their
    // selection and rebuilt their widgets, on every label or icon update.
    void rows_changed(size_t first, size_t n) override
    {
        for (auto [li, ctx] : bound_)
        {
            const size_t row = row_of(gtk_list_item_get_item(li));
            if (row >= first && row < first + n) bind_cell(li, *ctx);
        }
    }
    void rows_inserted(size_t first, size_t n) override
    {
        rows_->count += guint(n);
        g_list_model_items_changed(G_LIST_MODEL(rows_), guint(first), 0, guint(n));
    }
    void rows_removed(size_t first, size_t n) override
    {
        rows_->count -= guint(n);
        g_list_model_items_changed(G_LIST_MODEL(rows_), guint(first), guint(n), 0);
    }

protected:
    void on_disconnect() override
    {
        alive_ = false;
        if (popover_) gtk_widget_unparent(popover_);
        popover_ = nullptr;
    }

private:
    struct column_ctx
    {
        list_impl* list;
        size_t column;
        bool painted;
    };

    // The cell widget at a point of the column view, its row and column.
    struct cell_hit
    {
        GtkWidget* cell = nullptr;
        size_t row = 0, column = 0;
    };
    std::optional<cell_hit> cell_at(double x, double y) const
    {
        for (GtkWidget* w = gtk_widget_pick(w_, x, y, GTK_PICK_DEFAULT); w && w != w_; w = gtk_widget_get_parent(w))
        {
            const size_t tagged = row_of(w);
            auto* ctx = static_cast<column_ctx*>(g_object_get_data(G_OBJECT(w), "rapport-ctx"));
            if (tagged && ctx) return cell_hit{w, tagged - 1, ctx->column};
        }
        return std::nullopt;
    }
    static GtkWidget* row_widget(GtkWidget* cell)
    {
        for (GtkWidget* w = cell; w; w = gtk_widget_get_parent(w))
            if (std::string_view(gtk_widget_get_css_name(w)) == "row") return w;
        return nullptr;
    }

    struct cell_drag_state
    {
        bool pressed = false, dragging = false;
        double x0 = 0, y0 = 0;
        size_t from = 0;
        cell_drag_start start;
        std::optional<size_t> target;
        GtkWidget* target_row = nullptr;  // framed
    };
    void cell_drag_begin(GtkGestureDrag* g, double x, double y)
    {
        cell_ = {};
        auto hit = cell_at(x, y);
        std::optional<cell_drag_start> start;
        if (hit && model_)
        {
            graphene_point_t in = GRAPHENE_POINT_INIT(float(x), float(y)), out;
            if (!gtk_widget_compute_point(w_, hit->cell, &in, &out)) out = in;
            start = cell_drag_.begin(hit->row, hit->column, point_f{out.x, out.y});
        }
        if (!start)
        {
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);  // an ordinary click
            return;
        }
        cell_ = {true, false, x, y, hit->row, std::move(*start), std::nullopt, nullptr};
    }
    void cell_drag_update(GtkGestureDrag* g, double dx, double dy)
    {
        if (!cell_.pressed) return;
        if (!cell_.dragging)
        {
            int threshold = 8;
            g_object_get(gtk_widget_get_settings(w_), "gtk-dnd-drag-threshold", &threshold, nullptr);
            if (std::abs(dx) < threshold && std::abs(dy) < threshold) return;
            cell_.dragging = true;
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);  // no selection change on release
            gtk_widget_grab_focus(w_);  // Esc reaches the key controller
        }
        if (cell_.target_row) gtk_widget_remove_css_class(cell_.target_row, "rapport-drop");
        cell_.target.reset();
        cell_.target_row = nullptr;
        if (auto hit = cell_at(cell_.x0 + dx, cell_.y0 + dy);
            hit && cell_drag_.can_drop && cell_drag_.can_drop(cell_.from, hit->row, cell_.start.data))
        {
            cell_.target = hit->row;
            if ((cell_.target_row = row_widget(hit->cell))) gtk_widget_add_css_class(cell_.target_row, "rapport-drop");
        }
        gtk_widget_set_cursor_from_name(w_, cell_.target ? "grabbing" : "not-allowed");
    }
    void cell_drag_end(bool drop)
    {
        if (!cell_.pressed) return;
        cell_drag_state c = std::move(cell_);
        cell_ = {};
        if (c.target_row) gtk_widget_remove_css_class(c.target_row, "rapport-drop");
        gtk_widget_set_cursor(w_, nullptr);
        if (drop && c.dragging && c.target && cell_drag_.drop) cell_drag_.drop(c.from, *c.target, c.start.data);
    }
    cell_drag cell_drag_;
    cell_drag_state cell_;

    static size_t row_of(gpointer obj)
    {
        return GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(obj), "rapport-row"));
    }

    void add_column(const column& c, size_t index)
    {
        auto& ctx = ctxs_.emplace_back(new column_ctx{this, index, c.painted});
        GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
        connect(
            factory, "setup",
            +[](GtkSignalListItemFactory*, GtkListItem* li, gpointer p)
            { static_cast<column_ctx*>(p)->list->setup_cell(li, *static_cast<column_ctx*>(p)); },
            ctx.get());
        connect(
            factory, "bind",
            +[](GtkSignalListItemFactory*, GtkListItem* li, gpointer p)
            { static_cast<column_ctx*>(p)->list->bind_cell(li, *static_cast<column_ctx*>(p)); },
            ctx.get());
        connect(
            factory, "unbind",
            +[](GtkSignalListItemFactory*, GtkListItem* li, gpointer p)
            { static_cast<column_ctx*>(p)->list->bound_.erase(li); },
            ctx.get());

        GtkColumnViewColumn* col = gtk_column_view_column_new(c.title.c_str(), factory);
        gtk_column_view_column_set_resizable(col, true);
        gtk_column_view_column_set_expand(col, c.stretch);
        int width = px(c.width);
        if (width < 0 && c.painted) width = int(8 * g_metrics.char_width);
        if (width > 0) gtk_column_view_column_set_fixed_width(col, width);
        if (c.sortable)
        {
            // Never compares anything: it only makes the header clickable.
            // Sorting happens in the app's model.
            GtkCustomSorter* sorter = gtk_custom_sorter_new(nullptr, nullptr, nullptr);
            gtk_column_view_column_set_sorter(col, GTK_SORTER(sorter));
            g_object_unref(sorter);
        }
        gtk_column_view_column_set_visible(col, c.visible);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(w_), col);
        gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(w_), true);  // column_order()
        cols_.push_back(col);
        g_object_unref(col);
    }

    void setup_cell(GtkListItem* li, column_ctx& ctx)
    {
        GtkWidget* child;
        if (ctx.painted)
        {
            child = gtk_drawing_area_new();
            gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(child),
                                                int(g_metrics.line_height + 0.5f));
            gtk_widget_set_hexpand(child, true);
            gtk_drawing_area_set_draw_func(
                GTK_DRAWING_AREA(child),
                +[](GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer p)
                {
                    static_cast<column_ctx*>(p)->list->draw_cell(area, cr, w, h,
                                                               *static_cast<column_ctx*>(p));
                },
                &ctx, nullptr);
        }
        else
        {
            // An icon (a file's type, when the model gives one) and the text.
            GtkWidget* label = gtk_label_new(nullptr);
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_label_set_single_line_mode(GTK_LABEL(label), true);
            gtk_widget_set_hexpand(label, true);
            GtkWidget* image = gtk_image_new();
            gtk_widget_set_visible(image, false);
            child = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            if (checks_ && ctx.column == 0)
            {
                // The row's checkbox. bind_cell sets it from the model with
                // "rapport-binding" set, so only the user's clicks reach the model.
                GtkWidget* check = gtk_check_button_new();
                gtk_widget_set_focusable(check, false);
                gtk_box_append(GTK_BOX(child), check);
                g_object_set_data(G_OBJECT(child), "rapport-check", check);
                g_signal_connect(
                    check, "toggled",
                    G_CALLBACK(+[](GtkCheckButton* b, gpointer p)
                               {
                                   if (g_object_get_data(G_OBJECT(b), "rapport-binding")) return;
                                   auto* self = static_cast<column_ctx*>(p)->list;
                                   GtkWidget* cell = gtk_widget_get_parent(GTK_WIDGET(b));
                                   const size_t tagged = row_of(cell);
                                   if (self->model_ && tagged > 0)
                                       self->model_->toggle_checked(tagged - 1);
                               }),
                    &ctx);
            }
            gtk_box_append(GTK_BOX(child), image);
            gtk_box_append(GTK_BOX(child), label);
            g_object_set_data(G_OBJECT(child), "rapport-label", label);
            g_object_set_data(G_OBJECT(child), "rapport-image", image);
        }

        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
        g_signal_connect(click, "pressed",
                         G_CALLBACK(+[](GtkGestureClick* g, int, double x, double y, gpointer p)
                                    {
                                        auto* cell = gtk_event_controller_get_widget(
                                            GTK_EVENT_CONTROLLER(g));
                                        static_cast<column_ctx*>(p)->list->context_menu_at(cell, x, y);
                                    }),
                         &ctx);
        gtk_widget_add_controller(child, GTK_EVENT_CONTROLLER(click));

        g_object_set_data(G_OBJECT(child), "rapport-ctx", &ctx);
        gtk_list_item_set_child(li, child);
    }

    void bind_cell(GtkListItem* li, column_ctx& ctx)
    {
        if (!model_) return;
        bound_[li] = &ctx;
        size_t row = row_of(gtk_list_item_get_item(li));
        GtkWidget* child = gtk_list_item_get_child(li);
        // Stored +1 so 0 means "not bound".
        g_object_set_data(G_OBJECT(child), "rapport-row", GSIZE_TO_POINTER(row + 1));
        if (ctx.painted)
        {
            gtk_widget_queue_draw(child);
            return;
        }
        auto* label = GTK_LABEL(g_object_get_data(G_OBJECT(child), "rapport-label"));
        auto* image = GTK_IMAGE(g_object_get_data(G_OBJECT(child), "rapport-image"));
        if (auto* check =
                static_cast<GtkCheckButton*>(g_object_get_data(G_OBJECT(child), "rapport-check")))
        {
            const check_state st = model_->checked(row);
            g_object_set_data(G_OBJECT(check), "rapport-binding", GINT_TO_POINTER(1));
            gtk_check_button_set_inconsistent(check, st == check_state::indeterminate);
            gtk_check_button_set_active(check, st == check_state::checked);
            g_object_set_data(G_OBJECT(check), "rapport-binding", nullptr);
        }
        const icon icon = model_->icon(row, ctx.column);
        GIcon* gicon = !icon.file.empty() ? file_type_icon(icon.file) : !icon.name.empty() ? named_g_icon(icon.name) : nullptr;
        gtk_image_set_from_gicon(image, gicon);
        gtk_widget_set_visible(GTK_WIDGET(image), gicon != nullptr);
        gtk_label_set_text(label, model_->text(row, ctx.column).c_str());
        PangoAttrList* attrs = pango_attrs(GTK_WIDGET(label), model_->style(row, ctx.column));
        gtk_label_set_attributes(label, attrs);
        if (attrs) pango_attr_list_unref(attrs);
    }

    void draw_cell(GtkDrawingArea* area, cairo_t* cr, int w, int h, column_ctx& ctx)
    {
        size_t tagged = row_of(area);
        if (!alive_ || !model_ || tagged == 0) return;
        size_t row = tagged - 1;
        if (row >= model_->row_count()) return;
        cairo_painter painter(cr, GTK_WIDGET(area));
        cell_info info{{float(w), float(h)},
                      bool(gtk_selection_model_is_selected(sel_, guint(row))),
                      bool(gtk_widget_has_focus(GTK_WIDGET(area)))};
        model_->paint(row, ctx.column, painter, info);
    }

    void sort_changed(GtkColumnViewSorter* sorter)
    {
        if (suppress_sort_ || !sort_) return;
        GtkColumnViewColumn* col = gtk_column_view_sorter_get_primary_sort_column(sorter);
        auto it = std::ranges::find(cols_, col);
        if (it == cols_.end()) return;
        bool asc = gtk_column_view_sorter_get_primary_sort_order(sorter) == GTK_SORT_ASCENDING;
        sort_(size_t(it - cols_.begin()), asc ? sort_order::ascending : sort_order::descending);
    }

    void context_menu_at(GtkWidget* cell, double x, double y)
    {
        if (!alive_) return;
        size_t tagged = row_of(cell);
        std::optional<size_t> row;
        if (tagged) row = tagged - 1;
        if (row && !gtk_selection_model_is_selected(sel_, guint(*row)))
            gtk_selection_model_select_item(sel_, guint(*row), true);
        auto* ctx = static_cast<column_ctx*>(g_object_get_data(G_OBJECT(cell), "rapport-ctx"));
        const bool at_cell = cell_menu_ && row && ctx;
        if (!context_menu_ && !at_cell) return;

        menu menu = at_cell ? cell_menu_(*row, ctx->column, point_f{float(x), float(y)}) : context_menu_(row);
        if (menu.empty()) return;

        GMenu* model = ctx_menu_->build(std::move(menu));

        graphene_point_t in = GRAPHENE_POINT_INIT(float(x), float(y)), out;
        if (!gtk_widget_compute_point(cell, w_, &in, &out)) out = in;

        if (popover_) gtk_widget_unparent(popover_);
        popover_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(model));
        g_object_unref(model);
        gtk_widget_set_parent(popover_, w_);
        GdkRectangle at{int(out.x), int(out.y), 1, 1};
        gtk_popover_set_pointing_to(GTK_POPOVER(popover_), &at);
        gtk_popover_set_has_arrow(GTK_POPOVER(popover_), false);
        gtk_popover_set_position(GTK_POPOVER(popover_), GTK_POS_BOTTOM);
        gtk_widget_set_halign(popover_, GTK_ALIGN_START);
        gtk_popover_popup(GTK_POPOVER(popover_));
    }

    RapportRows* rows_ = nullptr;  // owned by sel_
    GtkSelectionModel* sel_ = nullptr;
    std::unordered_map<GtkListItem*, column_ctx*> bound_;  // cells showing a row now
    std::optional<std::pair<color, color>> soft_;  // selection background, text
    std::unique_ptr<menu_actions> ctx_menu_;
    GtkWidget* popover_ = nullptr;
    list_model* model_ = nullptr;
    std::vector<GtkColumnViewColumn*> cols_;
    std::vector<std::unique_ptr<column_ctx>> ctxs_;
    bool suppress_sort_ = false;
    bool alive_ = true;
    bool checks_ = false;  // a check button at the start of column 0

    std::function<void()> sel_changed_;
    std::function<void(size_t)> activate_;
    std::function<void(size_t, sort_order)> sort_;
    std::function<menu(std::optional<size_t>)> context_menu_;
    std::function<menu(size_t)> header_menu_;
    std::function<menu(size_t, size_t, point_f)> cell_menu_;
};

// -- window -------------------------------------------------------------------

// -- canvas -------------------------------------------------------------------

// Paint, resize, cursor and frame callbacks. Written without a GTK 4 build
// to check it against.
unsigned modifiers(GdkModifierType state)
{
    unsigned m = 0;
    if (state & GDK_SHIFT_MASK) m |= mod_shift;
    if (state & GDK_CONTROL_MASK) m |= mod_ctrl;
    if (state & GDK_ALT_MASK) m |= mod_alt;
    if (state & (GDK_SUPER_MASK | GDK_META_MASK)) m |= mod_meta;
    return m;
}

// Input mirrors the gtk3 WCanvas: presses, releases and the wheel from a
// legacy controller (raw events, so a release after a drag still arrives),
// motion and crossing from a motion controller, keys from a key controller.
class canvas_impl : public widget_base<rap::canvas>
{
public:
    explicit canvas_impl(const spec::canvas& s)
        : widget_base(gtk_drawing_area_new())
    {
        gtk_widget_set_focusable(w_, s.focusable);
        gtk_drawing_area_set_draw_func(
            GTK_DRAWING_AREA(w_),
            +[](GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer p)
            {
                auto* self = static_cast<canvas_impl*>(p);
                if (!self->paint_) return;
                cairo_painter painter(cr, GTK_WIDGET(area));
                self->paint_(painter, {float(w), float(h)});
            },
            this, nullptr);
        connect(
            w_, "resize",
            +[](GtkDrawingArea*, int w, int h, gpointer p)
            {
                auto* self = static_cast<canvas_impl*>(p);
                if (self->resize_) self->resize_({float(w), float(h)});
            },
            this);
        focusable_ = s.focusable;

        GtkEventController* legacy = gtk_event_controller_legacy_new();
        connect(legacy, "event", +[](GtkEventControllerLegacy*, GdkEvent* e, gpointer p) -> gboolean
                { return static_cast<canvas_impl*>(p)->raw_event(e); }, this);
        gtk_widget_add_controller(w_, legacy);

        GtkEventController* motion = gtk_event_controller_motion_new();
        connect(motion, "motion", +[](GtkEventControllerMotion* c, double x, double y, gpointer p)
                { static_cast<canvas_impl*>(p)->crossing(GTK_EVENT_CONTROLLER(c), pointer_event_type::move, x, y); }, this);
        connect(motion, "enter", +[](GtkEventControllerMotion* c, double x, double y, gpointer p)
                { static_cast<canvas_impl*>(p)->crossing(GTK_EVENT_CONTROLLER(c), pointer_event_type::enter, x, y); }, this);
        connect(motion, "leave", +[](GtkEventControllerMotion* c, gpointer p)
                { static_cast<canvas_impl*>(p)->crossing(GTK_EVENT_CONTROLLER(c), pointer_event_type::leave, -1, -1); }, this);
        gtk_widget_add_controller(w_, motion);

        GtkEventController* keys = gtk_event_controller_key_new();
        connect(keys, "key-pressed", +[](GtkEventControllerKey*, guint keyval, guint code, GdkModifierType state, gpointer p) -> gboolean
                { return static_cast<canvas_impl*>(p)->key(true, keyval, code, state); }, this);
        connect(keys, "key-released", +[](GtkEventControllerKey*, guint keyval, guint code, GdkModifierType state, gpointer p)
                { static_cast<canvas_impl*>(p)->key(false, keyval, code, state); }, this);
        gtk_widget_add_controller(w_, keys);
    }

    ~canvas_impl() override { on_disconnect(); }

    void invalidate() override { gtk_widget_queue_draw(w_); }
    size_f size() const override
    {
        return {float(gtk_widget_get_width(w_)), float(gtk_widget_get_height(w_))};
    }
    float device_scale() const override { return device_scale_of(w_); }
    void set_cursor(cursor c) override
    {
        static const char* names[] = {"default",   "pointer",   "text",        "crosshair",  "grab",
                                      "grabbing",  "zoom-in",   "zoom-out",    "move",       "wait",
                                      "none",      "ew-resize", "ns-resize",   "nwse-resize", "nesw-resize"};
        gtk_widget_set_cursor_from_name(w_, c == cursor::default_ ? nullptr : names[int(c)]);
    }

    void on_paint(std::function<void(rap::painter&, rap::size_f)> f) override { paint_ = std::move(f); }
    void on_resize(std::function<void(size_f)> f) override { resize_ = std::move(f); }
    void on_pointer(std::function<bool(const pointer_event&)> f) override { pointer_ = std::move(f); }
    void on_scroll(std::function<bool(const scroll_event&)> f) override { scroll_ = std::move(f); }
    void on_key(std::function<bool(const key_event&)> f) override { key_ = std::move(f); }

    subscription on_frame(std::function<void(double)> f) override
    {
        if (!*alive_) return {};
        guint id = gtk_widget_add_tick_callback(
            w_,
            +[](GtkWidget*, GdkFrameClock* clock, gpointer p) -> gboolean
            {
                (*static_cast<std::function<void(double)>*>(p))(
                    double(gdk_frame_clock_get_frame_time(clock)) / 1e6);
                return G_SOURCE_CONTINUE;
            },
            new std::function<void(double)>(std::move(f)),
            +[](gpointer p) { delete static_cast<std::function<void(double)>*>(p); });
        ticks_.push_back(id);
        return subscription(
            [this, alive = alive_, id]
            {
                if (!*alive) return;
                gtk_widget_remove_tick_callback(w_, id);
                std::erase(ticks_, id);
            });
    }

protected:
    void on_disconnect() override
    {
        if (!*alive_) return;
        *alive_ = false;
        for (guint id : ticks_) gtk_widget_remove_tick_callback(w_, id);
        ticks_.clear();
    }

private:
    // A raw event's position, from its surface to this widget.
    point_f widget_pos(GdkEvent* e) const
    {
        double x = 0, y = 0;
        gdk_event_get_position(e, &x, &y);
        GtkNative* native = gtk_widget_get_native(w_);
        double sx = 0, sy = 0;
        if (native) gtk_native_get_surface_transform(native, &sx, &sy);
        graphene_point_t in = GRAPHENE_POINT_INIT(float(x - sx), float(y - sy)), out = in;
        if (native && !gtk_widget_compute_point(GTK_WIDGET(native), w_, &in, &out)) out = in;
        return {out.x, out.y};
    }

    bool raw_event(GdkEvent* e)
    {
        const GdkEventType type = gdk_event_get_event_type(e);
        if (type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE)
        {
            pointer_event ev{type == GDK_BUTTON_PRESS ? pointer_event_type::press : pointer_event_type::release, widget_pos(e),
                            int(gdk_button_event_get_button(e)), 1, modifiers(gdk_event_get_modifier_state(e))};
            if (ev.type == pointer_event_type::press)
            {
                if (focusable_ && !gtk_widget_has_focus(w_)) gtk_widget_grab_focus(w_);
                // Count clicks ourselves, as gtk3 does, so each press arrives once.
                int time = 400, distance = 5;
                g_object_get(gtk_widget_get_settings(w_), "gtk-double-click-time", &time, "gtk-double-click-distance", &distance,
                             nullptr);
                const guint32 t = gdk_event_get_time(e);
                const bool again = ev.button == last_button_ && t - last_time_ <= guint32(time) &&
                                   std::abs(ev.pos.x - last_pos_.x) <= distance && std::abs(ev.pos.y - last_pos_.y) <= distance;
                clicks_ = again ? clicks_ + 1 : 1;
                ev.clicks = clicks_;
                last_button_ = ev.button;
                last_time_ = t;
                last_pos_ = ev.pos;
            }
            return pointer_ && pointer_(ev);
        }
        if (type == GDK_SCROLL && scroll_)
        {
            scroll_event ev{widget_pos(e), 0, 0, false, modifiers(gdk_event_get_modifier_state(e))};
            switch (gdk_scroll_event_get_direction(e))
            {
            case GDK_SCROLL_UP: ev.dy = -1; break;
            case GDK_SCROLL_DOWN: ev.dy = 1; break;
            case GDK_SCROLL_LEFT: ev.dx = -1; break;
            case GDK_SCROLL_RIGHT: ev.dx = 1; break;
            case GDK_SCROLL_SMOOTH:
            {
                double dx = 0, dy = 0;
                gdk_scroll_event_get_deltas(e, &dx, &dy);
                ev.dx = float(dx);
                ev.dy = float(dy);
                // Wheels report whole notches; touchpads, pixels.
                ev.precise = gdk_scroll_event_get_unit(e) == GDK_SCROLL_UNIT_SURFACE;
                break;
            }
            }
            if (ev.dx == 0 && ev.dy == 0) return false;
            return scroll_(ev);
        }
        return false;
    }

    void crossing(GtkEventController* c, pointer_event_type type, double x, double y)
    {
        if (!pointer_) return;
        point_f pos{float(x), float(y)};
        if (type == pointer_event_type::leave) pos = last_move_;
        pointer_({type, pos, 0, 1, modifiers(gtk_event_controller_get_current_event_state(c))});
        if (type != pointer_event_type::leave) last_move_ = pos;
    }

    bool key(bool press, guint keyval, guint code, GdkModifierType state)
    {
        if (!key_) return false;
        key_event ev;
        ev.press = press;
        ev.key = key_for(keyval);
        if (ev.key == key::character) ev.codepoint = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
        ev.modifiers = modifiers(state);
        if (press)
        {
            ev.repeat = std::ranges::find(held_, code) != held_.end();
            if (!ev.repeat) held_.push_back(code);
        }
        else
            std::erase(held_, code);
        return key_(ev);
    }

    std::function<void(rap::painter&, rap::size_f)> paint_;
    std::function<void(size_f)> resize_;
    std::function<bool(const pointer_event&)> pointer_;
    std::function<bool(const scroll_event&)> scroll_;
    std::function<bool(const key_event&)> key_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::vector<guint> ticks_;
    std::vector<guint> held_;
    bool focusable_ = false;
    int clicks_ = 0, last_button_ = 0;
    guint32 last_time_ = 0;
    point_f last_pos_{}, last_move_{};
};

class window_impl : public rap::window, public part
{
public:
    window_impl(const window_spec& spec, window_impl* parent)
        : win_(gtk_window_new())
        , dialog_(spec.dialog)
    {
        auto* win = GTK_WINDOW(win_);
        gtk_window_set_title(win, spec.title.c_str());
        gtk_window_set_default_size(win, px(spec.width), px(spec.height));
        if (parent) gtk_window_set_transient_for(win, GTK_WINDOW(parent->win_));
        gtk_window_set_resizable(win, spec.resizable);
        // Window keys come first (capture phase), except while a text field
        // is being typed in: then only Escape and the F-keys, as on gtk3.
        GtkEventController* keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        connect(keys, "key-pressed", +[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer p) -> gboolean
                { return static_cast<window_impl*>(p)->window_key(true, keyval, state); }, this);
        connect(keys, "key-released", +[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer p)
                { static_cast<window_impl*>(p)->window_key(false, keyval, state); }, this);
        gtk_widget_add_controller(win_, keys);
        connect(win_, "notify::fullscreened",
                +[](GObject*, GParamSpec*, gpointer p)
                {
                    auto* self = static_cast<window_impl*>(p);
                    const bool fs = gtk_window_is_fullscreen(GTK_WINDOW(self->win_));
                    if (fs == self->fullscreen_) return;
                    self->fullscreen_ = fs;
                    if (self->menu_bar) self->set_menu_bar_visible(self->menu_bar_wanted);  // hidden in fullscreen
                    if (self->on_fullscreen) self->on_fullscreen(fs);
                },
                this);
        if (dialog_)
        {
            gtk_window_set_modal(win, parent != nullptr && spec.modal);
            GtkEventController* keys = gtk_shortcut_controller_new();
            gtk_shortcut_controller_add_shortcut(
                GTK_SHORTCUT_CONTROLLER(keys),
                gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, GdkModifierType(0)),
                                 gtk_named_action_new("window.close")));
            gtk_widget_add_controller(win_, keys);
        }
        connect(
            win_, "close-request",
            +[](GtkWindow*, gpointer p) -> gboolean
            {
                static_cast<window_impl*>(p)->close_requested();
                return true;  // we own the window; never let GTK destroy it
            },
            this);
    }

    ~window_impl() override
    {
        if (last_shown() == win_) last_shown() = nullptr;
        for (auto& part : parts) part->disconnect();
        disconnect();
        gtk_window_destroy(GTK_WINDOW(win_));
    }

    void set_title(const std::string& t) override
    {
        gtk_window_set_title(GTK_WINDOW(win_), t.c_str());
    }
    void show() override
    {
        finished_ = false;
        gtk_window_present(GTK_WINDOW(win_));
        snapshot_if_asked();
    }

    // Development aid, as in gtk3: RAPPORT_SNAPSHOT=file.png saves the window
    // shown last RAPPORT_SNAPSHOT_DELAY ms (default 500) after it appears,
    // then quits the main loop.
    static GtkWidget*& last_shown()
    {
        static GtkWidget* w = nullptr;
        return w;
    }
    static GMainLoop*& main_loop()
    {
        static GMainLoop* loop = nullptr;
        return loop;
    }
    // The window shown last, as GTK last drew it, rendered to a PNG.
    static bool save_last_shown(const std::string& path)
    {
        GtkWidget* win = last_shown();
        if (!win) return false;
        bool ok = false;
        GdkPaintable* paintable = gtk_widget_paintable_new(win);
        GtkSnapshot* snap = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, snap, gtk_widget_get_width(win), gtk_widget_get_height(win));
        if (GskRenderNode* node = gtk_snapshot_free_to_node(snap))
        {
            GskRenderer* renderer = gtk_native_get_renderer(GTK_NATIVE(win));
            GdkTexture* tex = gsk_renderer_render_texture(renderer, node, nullptr);
            ok = gdk_texture_save_to_png(tex, path.c_str());
            g_object_unref(tex);
            gsk_render_node_unref(node);
        }
        g_object_unref(paintable);
        return ok;
    }
    void snapshot_if_asked()
    {
        last_shown() = win_;
        static bool taken = false;
        const char* path = std::getenv("RAPPORT_SNAPSHOT");
        if (taken || !path || !*path) return;
        taken = true;
        const char* delay = std::getenv("RAPPORT_SNAPSHOT_DELAY");
        g_timeout_add(
            delay ? guint(std::atoi(delay)) : 500u,
            +[](gpointer) -> gboolean
            {
                save_last_shown(std::getenv("RAPPORT_SNAPSHOT"));
                if (main_loop()) g_main_loop_quit(main_loop());
                return G_SOURCE_REMOVE;
            },
            nullptr);
    }
    void close() override { gtk_widget_set_visible(win_, false); }
    void on_finished(std::function<void(dialog_role)> f) override { on_finished_ = std::move(f); }
    void on_close_request(std::function<bool()> f) override { on_close_request_ = std::move(f); }
    void on_files_dropped(std::function<void(const std::vector<std::string>&)> f) override
    {
        const bool first = !on_files_dropped_;
        on_files_dropped_ = std::move(f);
        if (!first || !on_files_dropped_) return;
        GtkDropTarget* target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
        connect(
            target, "drop",
            +[](GtkDropTarget*, const GValue* value, double, double, gpointer p) -> gboolean
            {
                auto* self = static_cast<window_impl*>(p);
                if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return false;
                std::vector<std::string> paths;
                GSList* files =
                    gdk_file_list_get_files(static_cast<GdkFileList*>(g_value_get_boxed(value)));
                for (GSList* f = files; f; f = f->next)
                    if (char* path = g_file_get_path(G_FILE(f->data)))
                    {
                        paths.emplace_back(path);
                        g_free(path);
                    }
                g_slist_free(files);
                if (paths.empty() || !self->on_files_dropped_) return false;
                self->on_files_dropped_(paths);
                return true;
            },
            this);
        gtk_widget_add_controller(win_, GTK_EVENT_CONTROLLER(target));
    }

    // TODO(tortilla): drawn window icons.
    size_f size() const override
    {
        return {float(gtk_widget_get_width(win_)), float(gtk_widget_get_height(win_))};
    }
    void resize(size_f s) override
    {
        // GTK 4 can't place windows, but at least the frame fits the work
        // area (its shadows are what the surface has beyond the window).
        int w = int(s.w), h = int(s.h);
        const rect_f wa = work_area();
        if (GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(win_)); surface && wa.w > 0 && wa.h > 0)
        {
            const int extra_w = std::max(0, gdk_surface_get_width(surface) - gtk_widget_get_width(win_));
            const int extra_h = std::max(0, gdk_surface_get_height(surface) - gtk_widget_get_height(win_));
            w = std::min(w, int(wa.w) - extra_w);
            h = std::min(h, int(wa.h) - extra_h);
        }
        gtk_window_set_default_size(GTK_WINDOW(win_), std::max(1, w), std::max(1, h));
    }
    void set_min_size(size_f s) override { gtk_widget_set_size_request(win_, int(s.w), int(s.h)); }
    std::optional<point_f> position() const override { return std::nullopt; }  // GTK 4: never
    void move(point_f) override {}
    rect_f work_area() const override
    {
        // GTK 4 has no work area: the monitor's geometry, less what WSLg
        // can't see (the Windows taskbar).
        GdkDisplay* d = gtk_widget_get_display(win_);
        GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(win_));
        GdkMonitor* m = surface ? gdk_display_get_monitor_at_surface(d, surface) : nullptr;
        GdkRectangle r{};
        if (m)
            gdk_monitor_get_geometry(m, &r);
        else
        {
            GListModel* monitors = gdk_display_get_monitors(d);
            auto* first = GDK_MONITOR(g_list_model_get_item(monitors, 0));
            if (!first) return {};
            gdk_monitor_get_geometry(first, &r);
            g_object_unref(first);
        }
        return without_windows_taskbar({float(r.x), float(r.y), float(r.width), float(r.height)});
    }
    bool fullscreen() const override { return gtk_window_is_fullscreen(GTK_WINDOW(win_)); }
    bool maximized() const override
    {
        if (gtk_window_is_maximized(GTK_WINDOW(win_))) return true;
        GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(win_));
        return surface && GDK_IS_TOPLEVEL(surface) &&
               (gdk_toplevel_get_state(GDK_TOPLEVEL(surface)) & GDK_TOPLEVEL_STATE_TILED);
    }
    void set_fullscreen(bool on) override
    {
        if (on)
            gtk_window_fullscreen(GTK_WINDOW(win_));
        else
            gtk_window_unfullscreen(GTK_WINDOW(win_));
    }
    void on_fullscreen_changed(std::function<void(bool)> f) override { on_fullscreen = std::move(f); }
    void set_icon(const icon& icon) override
    {
        if (!icon.name.empty()) gtk_window_set_icon_name(GTK_WINDOW(win_), icon.name.c_str());
    }
    void on_key(std::function<bool(const key_event&)> f) override { on_key_ = std::move(f); }
    void set_menu_bar_visible(bool on) override
    {
        menu_bar_wanted = on;
        if (menu_bar) gtk_widget_set_visible(menu_bar, on && !gtk_window_is_fullscreen(GTK_WINDOW(win_)));
    }

    // The menu bar: a row of flat menu buttons, each building its menu when
    // it opens. (GtkPopoverMenuBar wants its whole model up front.)
    GtkWidget* build_menu_bar(const std::vector<menu_bar_item>& menus)
    {
        menu_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(menu_bar, "menubar");
        for (const menu_bar_item& m : menus)
        {
            auto bar = std::make_unique<bar_menu>();
            bar->fn = m.menu;
            GtkWidget* button = gtk_menu_button_new();
            gtk_menu_button_set_label(GTK_MENU_BUTTON(button), m.title.c_str());
            gtk_menu_button_set_use_underline(GTK_MENU_BUTTON(button), true);
            gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(button), false);
            gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(button), false);
            bar->actions = std::make_unique<menu_actions>(button, "bar");
            attach_menu(GTK_MENU_BUTTON(button), bar->actions.get(), &bar->fn);
            gtk_box_append(GTK_BOX(menu_bar), button);
            bars.push_back(std::move(bar));
        }
        return menu_bar;
    }

    void set_content(GtkWidget* content, bool margins, const std::vector<menu_bar_item>& menus)
    {
        int margin = !margins ? 0 : dialog_ ? 12 : 6;
        gtk_widget_set_margin_start(content, margin);
        gtk_widget_set_margin_end(content, margin);
        gtk_widget_set_margin_top(content, margin);
        gtk_widget_set_margin_bottom(content, margin);
        if (menus.empty()) return gtk_window_set_child(GTK_WINDOW(win_), content);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_append(GTK_BOX(box), build_menu_bar(menus));
        gtk_widget_set_vexpand(content, true);
        gtk_box_append(GTK_BOX(box), content);
        gtk_window_set_child(GTK_WINDOW(win_), box);
    }

    bool window_key(bool press, guint keyval, GdkModifierType state)
    {
        if (!on_key_) return false;
        GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(win_));
        const bool typing = focus && (GTK_IS_EDITABLE(focus) || (GTK_IS_TEXT_VIEW(focus) && gtk_text_view_get_editable(GTK_TEXT_VIEW(focus))));
        const bool not_text = keyval == GDK_KEY_Escape || (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12);
        if (typing && !not_text) return false;
        key_event ev;
        ev.press = press;
        ev.key = key_for(keyval);
        if (ev.key == key::character) ev.codepoint = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
        ev.modifiers = modifiers(state);
        return on_key_(ev);
    }
    std::function<bool(const key_event&)> on_key_;
    std::function<void(bool)> on_fullscreen;
    bool fullscreen_ = false;

    struct bar_menu
    {
        std::function<menu()> fn;
        std::unique_ptr<menu_actions> actions;
    };
    GtkWidget* menu_bar = nullptr;
    std::vector<std::unique_ptr<bar_menu>> bars;
    bool menu_bar_wanted = true;

    void add_role_button(GtkWidget* button, dialog_role role)
    {
        g_object_set_data(G_OBJECT(button), "rapport-role", GINT_TO_POINTER(int(role)));
        connect(
            button, "clicked",
            +[](GtkButton* b, gpointer p)
            {
                auto role =
                    dialog_role(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "rapport-role")));
                static_cast<window_impl*>(p)->finish(role);
            },
            this);
        if (role == dialog_role::ok || role == dialog_role::yes)
        {
            gtk_widget_add_css_class(button, "suggested-action");
            gtk_window_set_default_widget(GTK_WINDOW(win_), button);
        }
    }

    GtkWidget* gtk_window() const { return win_; }

    std::vector<std::unique_ptr<part>> parts;

private:
    void close_requested()
    {
        if (on_close_request_ && !on_close_request_()) return;
        if (dialog_)
            finish(dialog_role::cancel);
        else
            close();
    }

    // The callback may delete this window, so it runs last.
    void finish(dialog_role role)
    {
        close();
        if (finished_) return;
        finished_ = true;
        if (auto f = on_finished_) f(role);
    }

    GtkWidget* win_;
    bool dialog_;
    bool finished_ = false;
    std::function<void(dialog_role)> on_finished_;
    std::function<bool()> on_close_request_;
    std::function<void(const std::vector<std::string>&)> on_files_dropped_;
};

// -- builder ------------------------------------------------------------------

class builder
{
public:
    builder(window_impl& win, build_sink& sink)
        : win_(win)
        , sink_(sink)
    {
    }

    GtkWidget* build(const node& n, orientation parent_dir)
    {
        GtkWidget* w = std::visit([&](const auto& s) { return make(n, s, parent_dir); }, n.spec);
        // Containers have no interface; one with a handle gets a plain Widget.
        const bool container = std::holds_alternative<spec::box>(n.spec) ||
                               std::holds_alternative<spec::form>(n.spec) ||
                               std::holds_alternative<spec::group>(n.spec) ||
                               std::holds_alternative<spec::scroll>(n.spec) ||
                               std::holds_alternative<spec::toolbar>(n.spec);
        // A widget for a container with a handle, or one created hidden.
        if (container && (n.common.handle || !n.common.visible)) add<plain_impl>(n, w);
        int width = px(n.common.width), height = px(n.common.height);
        if (width > 0 || height > 0) gtk_widget_set_size_request(w, width, height);
        if (n.common.stretch > 0)
        {
            if (parent_dir == orientation::vertical)
                gtk_widget_set_vexpand(w, true);
            else
                gtk_widget_set_hexpand(w, true);
        }
        return w;
    }

private:
    static bool fills_form_row(const rap::node_spec& s)
    {
        return std::holds_alternative<spec::entry>(s) || std::holds_alternative<spec::combo>(s) ||
               std::holds_alternative<spec::text_view>(s) || std::holds_alternative<spec::list>(s) ||
               std::holds_alternative<spec::canvas>(s) || std::holds_alternative<spec::box>(s) ||
               std::holds_alternative<spec::form>(s) || std::holds_alternative<spec::splitter>(s) ||
               std::holds_alternative<spec::group>(s) || std::holds_alternative<spec::scroll>(s) ||
               std::holds_alternative<spec::tabs>(s);
    }

    template <class T, class... Args>
    T& add(const node& n, Args&&... args)
    {
        auto* w = new T(std::forward<Args>(args)...);
        win_.parts.emplace_back(w);
        sink_.created(n, *w);
        return *w;
    }

    GtkWidget* make(const node& n, const spec::box& s, orientation o)
    {
        // Controls here are cheap to make: a deferred box is built now.
        if (s.deferred) return make(n, spec::box{s.dir, {s.deferred()}, s.margins, {}}, o);
        GtkWidget* box = gtk_box_new(gtk_orientation(s.dir), 6);
        for (auto& c : s.items) gtk_box_append(GTK_BOX(box), build(c, s.dir));
        if (s.margins)
        {
            // A bar along a window edge: the full margin at its ends, half along its sides.
            const bool row = s.dir == orientation::horizontal;
            gtk_widget_set_margin_start(box, row ? 6 : 3);
            gtk_widget_set_margin_end(box, row ? 6 : 3);
            gtk_widget_set_margin_top(box, row ? 3 : 6);
            gtk_widget_set_margin_bottom(box, row ? 3 : 6);
        }
        return box;
    }

    GtkWidget* make(const node&, const spec::form& s, orientation)
    {
        GtkWidget* grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
        for (size_t i = 0; i < s.fields.size(); ++i)
        {
            GtkWidget* field = build(s.fields[i], orientation::horizontal);
            // Text fields and containers fill the row; spins, selects, colour
            // buttons and the like keep their natural width (as in Windows
            // dialogs) unless the node asks to stretch.
            if (fills_form_row(s.fields[i].spec))
                gtk_widget_set_hexpand(field, true);
            else if (!gtk_widget_get_hexpand(field))
                gtk_widget_set_halign(field, GTK_ALIGN_START);
            if (s.labels[i].empty())
            {
                gtk_grid_attach(GTK_GRID(grid), field, 0, int(i), 2, 1);
                continue;
            }
            GtkWidget* label = gtk_label_new(s.labels[i].c_str());
            gtk_label_set_xalign(GTK_LABEL(label), 1);
            gtk_widget_add_css_class(label, "dim-label");
            // set_visible on the field shows and hides its label too.
            g_object_set_data(G_OBJECT(field), "rapport-form-label", label);
            if (!s.fields[i].common.visible) gtk_widget_set_visible(label, false);
            gtk_grid_attach(GTK_GRID(grid), label, 0, int(i), 1, 1);
            gtk_grid_attach(GTK_GRID(grid), field, 1, int(i), 1, 1);
        }
        return grid;
    }

    GtkWidget* make(const node& n, const spec::splitter& s, orientation)
    {
        if (s.panes.empty()) return gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget* paned = panes(s, 0);
        if (GTK_IS_PANED(paned)) add<splitter_impl>(n, paned);
        return paned;
    }

    // GtkPaned holds two children; more panes nest to the right/bottom.
    GtkWidget* panes(const spec::splitter& s, size_t from)
    {
        GtkWidget* first = build(s.panes[from], s.dir);
        if (from + 1 == s.panes.size()) return first;

        bool any_stretch =
            std::ranges::any_of(s.panes, [](auto& p) { return p.common.stretch > 0; });
        // Spare space goes to stretching panes only (as on win32): the rest
        // keep their size when the window grows.
        const bool rest_stretch = std::any_of(s.panes.begin() + std::ptrdiff_t(from + 1), s.panes.end(),
                                             [](auto& p) { return p.common.stretch > 0; });
        GtkWidget* paned = gtk_paned_new(gtk_orientation(s.dir));
        auto* p = GTK_PANED(paned);
        gtk_paned_set_wide_handle(p, true);
        g_object_set_data(G_OBJECT(paned), "rapport-split-chain", GINT_TO_POINTER(1));
        gtk_paned_set_start_child(p, first);
        gtk_paned_set_end_child(p, panes(s, from + 1));
        gtk_paned_set_resize_start_child(p, !any_stretch || s.panes[from].common.stretch > 0);
        gtk_paned_set_resize_end_child(p, !any_stretch || rest_stretch);
        gtk_paned_set_shrink_start_child(p, false);
        gtk_paned_set_shrink_end_child(p, false);
        return paned;
    }

    GtkWidget* make(const node&, const spec::scroll& s, orientation)
    {
        GtkWidget* scroller = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                       s.horizontal ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller), true);
        gtk_widget_set_vexpand(scroller, true);
        if (!s.content.empty())
            gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller),
                                          build(s.content.front(), orientation::vertical));
        return scroller;
    }

    GtkWidget* make(const node&, const spec::group& s, orientation)
    {
        GtkWidget* frame = gtk_frame_new(s.title.c_str());
        if (!s.content.empty())
        {
            GtkWidget* child = build(s.content.front(), orientation::vertical);
            gtk_widget_set_margin_start(child, 6);
            gtk_widget_set_margin_end(child, 6);
            gtk_widget_set_margin_top(child, 6);
            gtk_widget_set_margin_bottom(child, 6);
            gtk_frame_set_child(GTK_FRAME(frame), child);
        }
        return frame;
    }

    GtkWidget* make(const node& n, const spec::tabs& s, orientation)
    {
        if (s.sidebar) return make_sidebar(n, s);
        GtkWidget* nb = gtk_notebook_new();
        std::vector<GtkWidget*> pages;
        for (size_t i = 0; i < s.pages.size(); ++i)
        {
            const bool lazy = i < s.lazy.size() && s.lazy[i];
            GtkWidget* page = lazy ? gtk_box_new(GTK_ORIENTATION_VERTICAL, 0) : build(s.pages[i], orientation::vertical);
            pages.push_back(page);
            gtk_widget_set_margin_start(page, 6);
            gtk_widget_set_margin_end(page, 6);
            gtk_widget_set_margin_top(page, 6);
            gtk_widget_set_margin_bottom(page, 6);
            gtk_notebook_append_page(GTK_NOTEBOOK(nb), page,
                                     gtk_label_new_with_mnemonic(s.titles[i].c_str()));
        }
        auto& tabs = add<tabs_impl>(n, nb);
        if (!s.lazy.empty())
            tabs.set_lazy(s.lazy, std::move(pages),
                         [&win = win_, &sink = sink_](const node& page) { return builder(win, sink).build(page, orientation::vertical); });
        return nb;
    }

    // Tabs as a list of page names down the left, the page in a stack beside it.
    GtkWidget* make_sidebar(const node& n, const spec::tabs& s)
    {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget* list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_BROWSE);
        GtkWidget* stack = gtk_stack_new();
        std::vector<GtkWidget*> pages;
        for (size_t i = 0; i < s.pages.size(); ++i)
        {
            GtkWidget* label = gtk_label_new_with_mnemonic(s.titles[i].c_str());
            gtk_widget_set_halign(label, GTK_ALIGN_START);
            gtk_widget_set_margin_start(label, 12);
            gtk_widget_set_margin_end(label, 12);
            gtk_widget_set_margin_top(label, 8);
            gtk_widget_set_margin_bottom(label, 8);
            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
            gtk_list_box_append(GTK_LIST_BOX(list), row);
            const bool lazy = i < s.lazy.size() && s.lazy[i];
            GtkWidget* page = lazy ? gtk_box_new(GTK_ORIENTATION_VERTICAL, 0) : build(s.pages[i], orientation::vertical);
            gtk_widget_set_margin_start(page, 6);
            gtk_widget_set_margin_end(page, 6);
            gtk_widget_set_margin_top(page, 6);
            gtk_widget_set_margin_bottom(page, 6);
            gtk_stack_add_named(GTK_STACK(stack), page, std::to_string(i).c_str());
            pages.push_back(page);
        }
        gtk_box_append(GTK_BOX(box), list);
        gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
        gtk_widget_set_hexpand(stack, TRUE);
        gtk_box_append(GTK_BOX(box), stack);
        auto& tabs = add<side_tabs_impl>(n, box, list, stack);
        tabs.set_current(0);
        if (!s.lazy.empty())
            tabs.set_lazy(s.lazy, std::move(pages),
                         [&win = win_, &sink = sink_](const node& page) { return builder(win, sink).build(page, orientation::vertical); });
        return box;
    }

    GtkWidget* make(const node&, const spec::spacer&, orientation)
    {
        return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    }

    GtkWidget* make(const node&, const spec::separator&, orientation parent_dir)
    {
        return gtk_separator_new(parent_dir == orientation::vertical ? GTK_ORIENTATION_HORIZONTAL
                                                                    : GTK_ORIENTATION_VERTICAL);
    }

    GtkWidget* make(const node& n, const spec::label& s, orientation)
    {
        return add<label_impl>(n, s).outer();
    }

    GtkWidget* make(const node& n, const spec::button& s, orientation)
    {
        auto& w = add<button_impl>(n, s.icon_only ? std::string() : s.text);
        if (s.icon) w.set_icon(s.icon);
        if (s.icon_only && n.common.tooltip.empty()) w.set_tooltip(s.text);
        GtkWidget* b = w.outer();
        if (s.flat) gtk_button_set_has_frame(GTK_BUTTON(b), false);
        return b;
    }

    GtkWidget* make(const node& n, const spec::tool_button& s, orientation)
    {
        return add<tool_button_impl>(n, s).outer();
    }

    // No GtkToolbar in GTK 4: a box styled as a toolbar.
    GtkWidget* make(const node&, const spec::toolbar& s, orientation)
    {
        GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(bar, "toolbar");
        for (const node& item : s.items)
        {
            GtkWidget* w = build(item, orientation::horizontal);
            if (std::holds_alternative<spec::spacer>(item.spec)) gtk_widget_set_hexpand(w, true);
            gtk_box_append(GTK_BOX(bar), w);
        }
        return bar;
    }

    GtkWidget* make(const node& n, const spec::slider& s, orientation)
    {
        GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, s.min, s.max,
                                                    s.step > 0 ? s.step : 0.01);
        gtk_scale_set_digits(GTK_SCALE(scale), s.digits);
        gtk_scale_set_draw_value(GTK_SCALE(scale), s.show_value);
        gtk_range_set_value(GTK_RANGE(scale), s.value);
        for (auto& [at, label] : s.marks)
            gtk_scale_add_mark(GTK_SCALE(scale), at, GTK_POS_BOTTOM,
                               label.empty() ? nullptr : label.c_str());
        return add<value_impl<rap::slider>>(n, scale).outer();
    }

    GtkWidget* make(const node& n, const spec::spin& s, orientation)
    {
        GtkWidget* spin = gtk_spin_button_new_with_range(s.min, s.max, s.step > 0 ? s.step : 1);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), guint(std::max(s.digits, 0)));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), s.value);
        return add<value_impl<rap::spin>>(n, spin).outer();
    }

    GtkWidget* make(const node& n, const spec::color_button& s, orientation)
    {
        return add<color_button_impl>(n, s).outer();
    }

    GtkWidget* make(const node& n, const spec::menu_button& s, orientation)
    {
        return add<menu_button_impl>(n, s).outer();
    }

    GtkWidget* make(const node& n, const spec::progress_bar&, orientation)
    {
        return add<progress_bar_impl>(n).outer();
    }

    GtkWidget* make(const node& n, const spec::date_picker& s, orientation)
    {
        return add<date_picker_impl>(n, s).outer();
    }

    GtkWidget* make(const node& n, const spec::check& s, orientation)
    {
        return add<check_impl>(n, s).outer();
    }

    GtkWidget* make(const node& n, const spec::entry& s, orientation)
    {
        return add<entry_impl>(n, s).outer();
    }

    GtkWidget* make(const node& n, const spec::drop_down& s, orientation)
    {
        return add<drop_down_impl>(n, s).outer();
    }

    GtkWidget* make(const node& n, const spec::combo& s, orientation)
    {
        return add<combo_impl>(n, s).outer();
    }

    GtkWidget* make(const node& n, const spec::text_view& s, orientation)
    {
        return add<text_view_impl>(n, s).outer();
    }

    GtkWidget* make(const node& n, const spec::list& s, orientation)
    {
        return add<list_impl>(n, s).outer();
    }

    GtkWidget* make(const node& n, const spec::canvas& s, orientation)
    {
        return add<canvas_impl>(n, s).outer();
    }

    GtkWidget* make(const node&, const spec::buttons& s, orientation)
    {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_top(box, 6);
        auto ordered = s.buttons;
        std::ranges::stable_sort(ordered, {}, [](auto& b) { return gnome_order(b.role); });
        bool spaced = false;
        for (auto& b : ordered)
        {
            // Help sits at the far start, the rest at the end.
            if (!spaced && b.role != dialog_role::help)
            {
                GtkWidget* gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
                gtk_widget_set_hexpand(gap, true);
                gtk_box_append(GTK_BOX(box), gap);
                spaced = true;
            }
            auto* w = new button_impl(b.text.empty() ? stock_label(b.role) : b.text);
            win_.parts.emplace_back(w);
            if (!b.on_click) win_.add_role_button(w->outer(), b.role);
            sink_.created(b, *w);
            gtk_box_append(GTK_BOX(box), w->outer());
        }
        return box;
    }

    window_impl& win_;
    build_sink& sink_;
};

// -- backend ------------------------------------------------------------------

class gtk4_backend : public backend
{
public:
    static backend* create(int*, char**)
    {
        // Under WSLg, Wayland, as for GTK 3: on its X server the pointer can
        // vanish over the window. (Its X11 does draw faster: 23 against 20 fps
        // for a panning canvas; gtk3 39 against 29.)
        if (!gtk_init_check()) return nullptr;
        // Dark when the desktop says so, as the user's theme draws it.
        follow_color_scheme(
            +[](int scheme)
            {
                GtkSettings* settings = gtk_settings_get_default();
                // 4.20 and later: the interface colour scheme, which also
                // picks the theme's dark variant (the older property now
                // warns at run time); before, that property.
                if (g_object_class_find_property(G_OBJECT_GET_CLASS(settings),
                                                 "gtk-interface-color-scheme"))
                    g_object_set(settings, "gtk-interface-color-scheme",
                                 scheme == 1   ? 2   // GTK_INTERFACE_COLOR_SCHEME_DARK
                                 : scheme == 2 ? 3   // _LIGHT
                                               : 1,  // _DEFAULT
                                 nullptr);
                else
                    g_object_set(settings, "gtk-application-prefer-dark-theme", scheme == 1,
                                 nullptr);
            });
        init_metrics();
        return new gtk4_backend;
    }

    std::string_view name() const override { return "gtk4"; }

    std::unique_ptr<rap::window> create_window(const window_spec& spec, rap::window* parent,
                                                build_sink& sink) override
    {
        auto win = std::make_unique<window_impl>(spec, static_cast<window_impl*>(parent));
        win->set_content(builder(*win, sink).build(spec.content, orientation::vertical),
                        spec.margins, spec.menu_bar);
        return win;
    }

    int run() override
    {
        if (quit_) return code_;
        loop_ = g_main_loop_new(nullptr, false);
        window_impl::main_loop() = loop_;
        g_main_loop_run(loop_);
        window_impl::main_loop() = nullptr;
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        return code_;
    }

    void quit(int code) override
    {
        quit_ = true;
        code_ = code;
        if (loop_) g_main_loop_quit(loop_);
    }

    void post(std::function<void()> f) override
    {
        g_idle_add_full(
            G_PRIORITY_DEFAULT,
            +[](gpointer p) -> gboolean
            {
                (*static_cast<std::function<void()>*>(p))();
                return G_SOURCE_REMOVE;
            },
            new std::function<void()>(std::move(f)),
            +[](gpointer p) { delete static_cast<std::function<void()>*>(p); });
    }

    void message(rap::window* parent, const message_spec& m,
                 std::function<void(dialog_role)> done) override
    {
        GtkAlertDialog* dlg = gtk_alert_dialog_new("%s", m.text.c_str());
        if (!m.detail.empty()) gtk_alert_dialog_set_detail(dlg, m.detail.c_str());

        struct context
        {
            std::vector<dialog_role> roles;
            std::function<void(dialog_role)> done;
        };
        auto* ctx = new context{m.buttons, std::move(done)};
        if (ctx->roles.empty()) ctx->roles = {dialog_role::ok};

        std::vector<std::string> labels;
        for (auto r : ctx->roles)
        {
            std::string l = stock_label(r);
            std::erase(l, '_');
            labels.push_back(l);
        }
        std::vector<const char*> ptrs;
        for (auto& l : labels) ptrs.push_back(l.c_str());
        ptrs.push_back(nullptr);
        gtk_alert_dialog_set_buttons(dlg, ptrs.data());
        for (size_t i = 0; i < ctx->roles.size(); ++i)
            if (ctx->roles[i] == dialog_role::cancel || ctx->roles[i] == dialog_role::no)
                gtk_alert_dialog_set_cancel_button(dlg, int(i));

        gtk_alert_dialog_choose(
            dlg, parent_window(parent), nullptr,
            +[](GObject* src, GAsyncResult* res, gpointer p)
            {
                std::unique_ptr<context> ctx(static_cast<context*>(p));
                int i = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
                dialog_role role = i >= 0 && size_t(i) < ctx->roles.size() ? ctx->roles[size_t(i)]
                                                                          : dialog_role::cancel;
                if (ctx->done) ctx->done(role);
            },
            ctx);
        g_object_unref(dlg);
    }

    // A save in progress. When the backend added the extension and that
    // file exists, GTK's own overwrite check saw only the name as typed, so
    // replacing is asked here; Cancel reopens the dialog on the typed name.
    struct save_state
    {
        GtkFileDialog* dlg;  // owned
        GtkWindow* parent;
        std::string ext;
        std::function<void(std::vector<std::string>)> done;

        void run()
        {
            gtk_file_dialog_save(dlg, parent, nullptr,
                                 +[](GObject* s, GAsyncResult* r, gpointer p)
                                 {
                                     static_cast<save_state*>(p)->saved(
                                         gtk_file_dialog_save_finish(GTK_FILE_DIALOG(s), r, nullptr));
                                 },
                                 this);
        }
        void saved(GFile* file)
        {
            std::string typed;
            if (file)
            {
                if (char* path = g_file_get_path(file))
                {
                    typed = path;
                    g_free(path);
                }
                g_object_unref(file);
            }
            if (typed.empty()) return finish({});
            const std::string path = with_extension(typed, ext);
            if (path == typed || !g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) return finish({path});
            gchar* name = g_path_get_basename(path.c_str());
            GtkAlertDialog* alert =
                gtk_alert_dialog_new("%s", rap::i18n::format(rap::i18n::tr("rapport", "A file named \u201c{0}\u201d "
                                                                                     "already exists. Replace it?"),
                                                             name)
                                               .c_str());
            g_free(name);
            const std::string cancel = stock_label(dialog_role::cancel);
            const char* buttons[] = {cancel.c_str(), rap::i18n::tr_ctx("rapport", "button", "_Replace"), nullptr};
            gtk_alert_dialog_set_buttons(alert, buttons);
            gtk_alert_dialog_set_cancel_button(alert, 0);
            gtk_alert_dialog_set_default_button(alert, 0);
            pending = {typed, path};
            gtk_alert_dialog_choose(alert, parent, nullptr,
                                    +[](GObject* a, GAsyncResult* r, gpointer p)
                                    {
                                        auto* self = static_cast<save_state*>(p);
                                        const int b = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(a), r, nullptr);
                                        if (b == 1) return self->finish({self->pending.second});
                                        GFile* again = g_file_new_for_path(self->pending.first.c_str());
                                        gtk_file_dialog_set_initial_file(self->dlg, again);
                                        g_object_unref(again);
                                        self->run();
                                    },
                                    this);
            g_object_unref(alert);
        }
        void finish(std::vector<std::string> out)
        {
            auto done = std::move(this->done);
            g_object_unref(dlg);
            delete this;
            if (done) done(std::move(out));
        }
        std::pair<std::string, std::string> pending;  // typed, with extension
    };

    void choose_files(rap::window* parent, const file_dialog_spec& f,
                     std::function<void(std::vector<std::string>)> done) override
    {
        GtkFileDialog* dlg = gtk_file_dialog_new();
        if (!f.title.empty()) gtk_file_dialog_set_title(dlg, f.title.c_str());
        if (!f.filters.empty())
        {
            GListStore* store = g_list_store_new(GTK_TYPE_FILE_FILTER);
            for (auto& flt : f.filters)
            {
                GtkFileFilter* ff = gtk_file_filter_new();
                gtk_file_filter_set_name(ff, flt.name.c_str());
                for (size_t at = 0; at <= flt.pattern.size();)  // "*.patch;*.diff"
                {
                    size_t end = std::min(flt.pattern.find(';', at), flt.pattern.size());
                    if (end > at) gtk_file_filter_add_pattern(ff, flt.pattern.substr(at, end - at).c_str());
                    at = end + 1;
                }
                g_list_store_append(store, ff);
                g_object_unref(ff);
            }
            gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(store));
            g_object_unref(store);
        }
        if (!f.initial_path.empty())
        {
            GFile* file = g_file_new_for_path(f.initial_path.c_str());
            if (g_file_test(f.initial_path.c_str(), G_FILE_TEST_IS_DIR))
                gtk_file_dialog_set_initial_folder(dlg, file);
            else
                gtk_file_dialog_set_initial_file(dlg, file);
            g_object_unref(file);
        }

        using done_fn = std::function<void(std::vector<std::string>)>;
        GtkWindow* pw = parent_window(parent);
        if (f.mode == file_dialog_spec::mode::save)
        {
            // GTK 4 doesn't say which filter the user picked: the first's
            // extension goes on a name typed without one.
            auto* save = new save_state{dlg, pw, f.filters.empty() ? "" : default_extension(f.filters.front().pattern),
                                       std::move(done)};
            g_object_ref(dlg);
            save->run();
            g_object_unref(dlg);
            return;
        }
        auto* ctx = new done_fn(std::move(done));
        auto path_of = [](GFile* file)
        {
            std::string out;
            if (char* p = g_file_get_path(file))
            {
                out = p;
                g_free(p);
            }
            return out;
        };
        static auto single = +[](GFile* file, gpointer p)
        {
            std::unique_ptr<done_fn> done(static_cast<done_fn*>(p));
            std::vector<std::string> out;
            if (file)
            {
                if (char* path = g_file_get_path(file))
                {
                    out.push_back(path);
                    g_free(path);
                }
                g_object_unref(file);
            }
            if (*done) (*done)(std::move(out));
        };
        (void)path_of;

        switch (f.mode)
        {
        case file_dialog_spec::mode::open:
            gtk_file_dialog_open(
                dlg, pw, nullptr,
                +[](GObject* s, GAsyncResult* r, gpointer p)
                { single(gtk_file_dialog_open_finish(GTK_FILE_DIALOG(s), r, nullptr), p); },
                ctx);
            break;
        case file_dialog_spec::mode::save: break;  // above
        case file_dialog_spec::mode::folder:
            gtk_file_dialog_select_folder(
                dlg, pw, nullptr,
                +[](GObject* s, GAsyncResult* r, gpointer p)
                {
                    single(gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(s), r, nullptr), p);
                },
                ctx);
            break;
        case file_dialog_spec::mode::open_many:
            gtk_file_dialog_open_multiple(
                dlg, pw, nullptr,
                +[](GObject* s, GAsyncResult* r, gpointer p)
                {
                    std::unique_ptr<done_fn> done(static_cast<done_fn*>(p));
                    std::vector<std::string> out;
                    if (GListModel* files =
                            gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(s), r, nullptr))
                    {
                        for (guint i = 0; i < g_list_model_get_n_items(files); ++i)
                        {
                            auto* file = G_FILE(g_list_model_get_item(files, i));
                            if (char* path = g_file_get_path(file))
                            {
                                out.push_back(path);
                                g_free(path);
                            }
                            g_object_unref(file);
                        }
                        g_object_unref(files);
                    }
                    if (*done) (*done)(std::move(out));
                },
                ctx);
            break;
        }
        g_object_unref(dlg);
    }

    // TODO(tortilla): a GdkTexture on the clipboard.
    bool save_snapshot(const std::string& path) override { return window_impl::save_last_shown(path); }

    void set_clipboard_image(int w, int h, pixel_format format, const uint8_t* pixels,
                             size_t stride) override
    {
        GdkDisplay* d = gdk_display_get_default();
        if (!d || w <= 0 || h <= 0) return;
        // The pixels are the caller's: copied now.
        GBytes* bytes = g_bytes_new(pixels, stride * size_t(h));
        GdkTexture* texture = gdk_memory_texture_new(
            w, h,
            format == pixel_format::rgba8 ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
            bytes, stride);
        gdk_clipboard_set_texture(gdk_display_get_clipboard(d), texture);
        g_object_unref(texture);
        g_bytes_unref(bytes);
    }

    // GTK's print operation (gtk-common), once the caller has returned.
    void print(rap::window* parent, print_job job) override
    {
        GtkWindow* over =
            parent ? GTK_WINDOW(static_cast<window_impl*>(parent)->gtk_window()) : nullptr;
        post([over, job = std::move(job)]() mutable { run_print_operation(over, std::move(job)); });
    }
    void set_clipboard(const std::string& s) override
    {
        if (GdkDisplay* d = gdk_display_get_default())
            gdk_clipboard_set_text(gdk_display_get_clipboard(d), s.c_str());
    }

    // TODO(tortilla): a GdkTexture-backed Image would avoid cairo for painting.
    std::unique_ptr<rap::image> create_image(int w, int h, pixel_format format,
                                              const uint8_t* pixels, size_t stride) override
    {
        return std::make_unique<cairo_image>(w, h, format, pixels, stride);
    }

    subscription timer(int ms, bool repeat, std::function<void()> fn) override
    {
        struct timer_state
        {
            std::function<void()> fn;
            bool repeat;
            guint id = 0;
        };
        auto state = std::make_shared<timer_state>(timer_state{std::move(fn), repeat});
        state->id = g_timeout_add_full(
            G_PRIORITY_DEFAULT, guint(std::max(ms, 0)),
            +[](gpointer p) -> gboolean
            {
                auto state = *static_cast<std::shared_ptr<timer_state>*>(p);
                const bool again = state->repeat;
                if (!again) state->id = 0;
                state->fn();
                return again && state->id ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
            },
            new std::shared_ptr<timer_state>(state),
            +[](gpointer p) { delete static_cast<std::shared_ptr<timer_state>*>(p); });
        return subscription(
            [state]
            {
                if (state->id) g_source_remove(std::exchange(state->id, 0));
            });
    }

private:
    static GtkWindow* parent_window(rap::window* w)
    {
        return w ? GTK_WINDOW(static_cast<window_impl*>(w)->gtk_window()) : nullptr;
    }

    GMainLoop* loop_ = nullptr;
    bool quit_ = false;
    int code_ = 0;
};

}  // namespace
}  // namespace rap::gtk4

RAPPORT_BACKEND_ENTRY(gtk4, rap::gtk4::gtk4_backend)
