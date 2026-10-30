// GTK 4 backend (package rapport-gtk4).
//
// Lists use GtkColumnView over a lazy GListModel (RapportRows) whose items
// are bare GObjects tagged with their row index; cells pull text from the
// rap::list_model at bind time. Painted columns are GtkDrawingAreas.
#include <gtk/gtk.h>

#include "builtin_icons.h"
#include "gtk_common.h"
#include "frame_layer.h"
#include "gpu_readback.h"

#include <memory>

// -- RapportRows --------------------------------------------------------------

G_BEGIN_DECLS
G_DECLARE_FINAL_TYPE(RapportRows, rapport_rows, RAPPORT, ROWS, GObject)
G_END_DECLS

// Each row's item carries its index. GTK keeps the items (and their cells)
// it already has across an insert or removal and moves them, so the ones
// still alive are kept here to renumber.
struct _RapportRows
{
    GObject parent_instance;
    guint count;
    GHashTable* live;  // the items handed out and not yet freed
};

static void rapport_rows_item_freed(gpointer self, GObject* item)
{
    g_hash_table_remove(RAPPORT_ROWS(self)->live, item);
}

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
    g_hash_table_add(RAPPORT_ROWS(m)->live, item);
    g_object_weak_ref(item, rapport_rows_item_freed, m);
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

static void rapport_rows_finalize(GObject* o)
{
    auto* self = RAPPORT_ROWS(o);
    GHashTableIter it;
    gpointer item;
    g_hash_table_iter_init(&it, self->live);
    while (g_hash_table_iter_next(&it, &item, nullptr))
        g_object_weak_unref(G_OBJECT(item), rapport_rows_item_freed, self);
    g_hash_table_unref(self->live);
    G_OBJECT_CLASS(rapport_rows_parent_class)->finalize(o);
}
static void rapport_rows_class_init(RapportRowsClass* c)
{
    G_OBJECT_CLASS(c)->finalize = rapport_rows_finalize;
}
static void rapport_rows_init(RapportRows* self)
{
    self->count = 0;
    self->live = g_hash_table_new(g_direct_hash, g_direct_equal);
}

// Items at `from` and after move by `by` rows.
static void rapport_rows_renumber(RapportRows* self, guint from, gint by)
{
    GHashTableIter it;
    gpointer item;
    g_hash_table_iter_init(&it, self->live);
    while (g_hash_table_iter_next(&it, &item, nullptr))
    {
        const guint row = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item), "rapport-row"));
        if (row >= from)
            g_object_set_data(G_OBJECT(item), "rapport-row",
                              GUINT_TO_POINTER(guint(gint(row) + by)));
    }
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
        if (s.wrap)
        {
            // About 60 characters across at most; GTK fits the height.
            gtk_label_set_wrap(GTK_LABEL(w_), true);
            gtk_label_set_wrap_mode(GTK_LABEL(w_), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_max_width_chars(GTK_LABEL(w_), 60);
        }
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

// Where a widget falls on its surface's device pixels. GTK 4 lays widgets
// out in logical pixels, so at a fractional scale one can start part way
// into a device pixel (y 57 at 1.25 is 71.25), and drawing there straddles
// pixels: soft, with a 1:1 frame's edge rows half blended. Painting moved
// by `shift` starts on the next whole pixel, and `size` is the whole pixels
// inside the widget. Whole at a whole scale, and wherever the origin
// happens to land on a pixel.
struct device_span
{
    point_f shift;  // logical, each under one device pixel
    size_f size;    // logical
    int width = 0, height = 0;  // `size` in device pixels

    bool operator==(const device_span& o) const
    {
        return width == o.width && height == o.height && shift.x == o.shift.x && shift.y == o.shift.y;
    }
};

device_span device_span_of(GtkWidget* w, float scale)
{
    const float lw = float(gtk_widget_get_width(w)), lh = float(gtk_widget_get_height(w));
    device_span span{{}, {lw, lh}, int(std::lround(lw * scale)), int(std::lround(lh * scale))};
    GtkNative* native = gtk_widget_get_native(w);
    if (!native || scale <= 0) return span;
    double sx = 0, sy = 0;
    gtk_native_get_surface_transform(native, &sx, &sy);
    double x = 0, y = 0;
#if GTK_CHECK_VERSION(4, 12, 0)
    graphene_point_t in = GRAPHENE_POINT_INIT(0, 0), out;
    if (!gtk_widget_compute_point(w, GTK_WIDGET(native), &in, &out)) return span;
    x = out.x;
    y = out.y;
#else
    if (!gtk_widget_translate_coordinates(w, GTK_WIDGET(native), 0, 0, &x, &y)) return span;
#endif
    // Float noise mustn't cost a pixel: 71.0001 is on the grid.
    constexpr double eps = 1e-3;
    const double ox = (x + sx) * scale, oy = (y + sy) * scale;
    const double x0 = std::ceil(ox - eps), y0 = std::ceil(oy - eps);
    const double x1 = std::floor(ox + lw * scale + eps), y1 = std::floor(oy + lh * scale + eps);
    span.width = int(std::max(0.0, x1 - x0));
    span.height = int(std::max(0.0, y1 - y0));
    span.shift = {float(std::max(0.0, x0 - ox) / scale), float(std::max(0.0, y0 - oy) / scale)};
    span.size = {float(span.width / scale), float(span.height / scale)};
    return span;
}

// Moves a drawing area's painting onto whole device pixels, for crisp
// edges and pixel_grid snapping. Its size stays the area's: a list cell's
// painting runs to the row's edge, where the next row's carries on.
void snap_to_device(cairo_t* cr, GtkWidget* w)
{
    const device_span span = device_span_of(w, device_scale_of(w));
    cairo_translate(cr, span.shift.x, span.shift.y);
}

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
            snap_to_device(cr, GTK_WIDGET(a));
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
// Pops up a popover menu once GTK has finished building it. A menu's
// section separators are shown in an idle after it's made, so popped up at
// once it's sized without them and cut short (its last items only reached
// by scrolling).
void popup_when_built(GtkWidget* popover)
{
    g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        +[](gpointer p) -> gboolean
        {
            auto* w = static_cast<GtkWidget*>(p);
            if (gtk_widget_get_parent(w)) gtk_popover_popup(GTK_POPOVER(w));
            return G_SOURCE_REMOVE;
        },
        g_object_ref(popover), g_object_unref);
}

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
// -- tree ---------------------------------------------------------------------

// A GtkListView over a GtkTreeListModel, rows a GtkTreeExpander and label.
// The models hold GtkStringObjects naming the app's ids; labels and children
// come from nodes_.
class tree_impl : public widget_base<rap::tree>
{
public:
    explicit tree_impl(const spec::tree& s)
        : widget_base(nullptr, gtk_scrolled_window_new())
    {
        root_ = g_list_store_new(GTK_TYPE_STRING_OBJECT);
        model_ = gtk_tree_list_model_new(G_LIST_MODEL(g_object_ref(root_)), false, false,
                                         +[](gpointer item, gpointer p) -> GListModel*
                                         { return static_cast<tree_impl*>(p)->children_of(item); },
                                         this, nullptr);
        sel_ = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(model_)));
        gtk_single_selection_set_autoselect(sel_, false);
        gtk_single_selection_set_can_unselect(sel_, true);
        GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
        connect(factory, "setup",
                +[](GtkSignalListItemFactory*, GObject* li, gpointer p)
                { static_cast<tree_impl*>(p)->setup(GTK_LIST_ITEM(li)); },
                this);
        connect(factory, "bind",
                +[](GtkSignalListItemFactory*, GObject* li, gpointer p)
                { static_cast<tree_impl*>(p)->bind_row(GTK_LIST_ITEM(li)); },
                this);
        w_ = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(sel_)), factory);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer_), w_);
        gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(outer_), true);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outer_), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        ctx_menu_ = std::make_unique<menu_actions>(w_, "ctx");
        connect(sel_, "selection-changed",
                +[](GtkSelectionModel*, guint, guint, gpointer p)
                {
                    auto* self = static_cast<tree_impl*>(p);
                    if (!self->quiet_ && self->changed_) self->changed_();
                },
                this);
        connect(w_, "activate",
                +[](GtkListView*, guint pos, gpointer p)
                {
                    auto* self = static_cast<tree_impl*>(p);
                    if (self->activate_) self->activate_(self->id_at(pos));
                },
                this);
        // Right-click on blank space (rows claim their own presses).
        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
        connect(click, "pressed",
                +[](GtkGestureClick*, int, double x, double y, gpointer p)
                { static_cast<tree_impl*>(p)->popup(std::nullopt, x, y); },
                this);
        gtk_widget_add_controller(w_, GTK_EVENT_CONTROLLER(click));
        // The menu key, and Shift+F10: under the selected row's start.
        GtkEventController* keys = gtk_event_controller_key_new();
        connect(keys, "key-pressed",
                +[](GtkEventControllerKey*, guint key, guint, GdkModifierType state, gpointer p) -> gboolean
                {
                    auto* self = static_cast<tree_impl*>(p);
                    if (key != GDK_KEY_Menu && !(key == GDK_KEY_F10 && (state & GDK_SHIFT_MASK))) return false;
                    self->popup(self->selected(), 16, 16);
                    return true;
                },
                this);
        gtk_widget_add_controller(w_, keys);
        set_items(s.items);
    }
    ~tree_impl() override
    {
        g_object_unref(sel_);
        g_object_unref(model_);
        g_object_unref(root_);
    }

    void set_items(std::vector<tree_item> items) override
    {
        const std::optional<std::string> keep = selected();
        std::unordered_map<std::string, bool> open;
        for (guint i = 0, n = rows(); i < n; ++i)
            if (GtkTreeListRow* row = gtk_tree_list_model_get_row(model_, i))
            {
                open[id_of(row)] = gtk_tree_list_row_get_expanded(row);
                g_object_unref(row);
            }
        quiet q(*this);
        nodes_.clear();
        parent_.clear();
        std::vector<std::string> top;
        for (const tree_item& t : items)
        {
            add(t, "");
            top.push_back(t.id);
        }
        for (auto& [id, node] : nodes_)
            if (auto was = open.find(id); was != open.end()) node.expanded = was->second;
        g_list_store_remove_all(root_);
        for (const std::string& id : top)
        {
            GtkStringObject* o = gtk_string_object_new(id.c_str());
            g_list_store_append(root_, o);
            g_object_unref(o);
        }
        // Open what should be, top down: children follow their parent's row.
        for (guint i = 0; i < rows(); ++i)
            if (GtkTreeListRow* row = gtk_tree_list_model_get_row(model_, i))
            {
                auto it = nodes_.find(id_of(row));
                if (it != nodes_.end() && it->second.expanded && !it->second.children.empty())
                    gtk_tree_list_row_set_expanded(row, true);
                g_object_unref(row);
            }
        gtk_single_selection_set_selected(sel_, GTK_INVALID_LIST_POSITION);
        if (keep) select_quietly(*keep);
    }
    std::optional<std::string> selected() const override
    {
        const guint pos = gtk_single_selection_get_selected(sel_);
        if (pos == GTK_INVALID_LIST_POSITION) return std::nullopt;
        return id_at(pos);
    }
    void select(const std::string& id) override
    {
        quiet q(*this);
        if (!select_quietly(id)) gtk_single_selection_set_selected(sel_, GTK_INVALID_LIST_POSITION);
    }
    void set_expanded(const std::string& id, bool open) override
    {
        if (auto pos = position_of(id))
            if (GtkTreeListRow* row = gtk_tree_list_model_get_row(model_, *pos))
            {
                gtk_tree_list_row_set_expanded(row, open);
                g_object_unref(row);
            }
    }
    void on_selection_changed(std::function<void()> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void(const std::string&)> f) override { activate_ = std::move(f); }
    void on_context_menu(std::function<menu(std::optional<std::string>)> f) override { context_ = std::move(f); }

protected:
    void on_disconnect() override
    {
        alive_ = false;
        if (popover_) gtk_widget_unparent(popover_);
        popover_ = nullptr;
    }

private:
    struct node
    {
        std::string label;
        std::vector<std::string> children;
        bool expanded = false;
    };
    struct quiet
    {
        explicit quiet(tree_impl& t)
            : t(t)
            , was(t.quiet_)
        {
            t.quiet_ = true;
        }
        ~quiet() { t.quiet_ = was; }
        tree_impl& t;
        bool was;
    };

    void add(const tree_item& t, const std::string& parent)
    {
        node n{t.label, {}, t.expanded};
        for (const tree_item& c : t.children)
        {
            n.children.push_back(c.id);
            add(c, t.id);
        }
        nodes_[t.id] = std::move(n);
        parent_[t.id] = parent;
    }
    GListModel* children_of(gpointer item)
    {
        auto it = nodes_.find(gtk_string_object_get_string(GTK_STRING_OBJECT(item)));
        if (it == nodes_.end() || it->second.children.empty()) return nullptr;
        GListStore* store = g_list_store_new(GTK_TYPE_STRING_OBJECT);
        for (const std::string& id : it->second.children)
        {
            GtkStringObject* o = gtk_string_object_new(id.c_str());
            g_list_store_append(store, o);
            g_object_unref(o);
        }
        return G_LIST_MODEL(store);
    }
    void setup(GtkListItem* li)
    {
        GtkWidget* expander = gtk_tree_expander_new();
        GtkWidget* label = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), label);
        gtk_list_item_set_child(li, expander);
        // A right-click on the row: selects it, then its menu. GTK recycles
        // and drops rows, so this isn't one of the connections the window
        // cuts (connect): the gesture goes with its row, and rows with the
        // view.
        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
        g_object_set_data(G_OBJECT(click), "rapport-item", li);
        g_signal_connect(click, "pressed", G_CALLBACK(row_pressed), this);
        gtk_widget_add_controller(expander, GTK_EVENT_CONTROLLER(click));
    }
    static void row_pressed(GtkGestureClick* g, int, double x, double y, gpointer p)
    {
        auto* self = static_cast<tree_impl*>(p);
        if (!self->alive_) return;
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        auto* li = static_cast<GtkListItem*>(g_object_get_data(G_OBJECT(g), "rapport-item"));
        const guint pos = gtk_list_item_get_position(li);
        if (pos == GTK_INVALID_LIST_POSITION) return;
        if (gtk_single_selection_get_selected(self->sel_) != pos)
            gtk_single_selection_set_selected(self->sel_, pos);  // the user's: reported
        GtkWidget* row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
        graphene_point_t in = GRAPHENE_POINT_INIT(float(x), float(y)), out;
        if (!gtk_widget_compute_point(row, self->w_, &in, &out)) out = in;
        self->popup(self->id_at(pos), out.x, out.y);
    }
    void bind_row(GtkListItem* li)
    {
        auto* row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(li));
        GtkWidget* expander = gtk_list_item_get_child(li);
        gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
        auto it = nodes_.find(id_of(row));
        gtk_label_set_text(GTK_LABEL(gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander))),
                           it != nodes_.end() ? it->second.label.c_str() : "");
    }
    guint rows() const { return g_list_model_get_n_items(G_LIST_MODEL(model_)); }
    static std::string id_of(GtkTreeListRow* row)
    {
        auto* o = static_cast<GtkStringObject*>(gtk_tree_list_row_get_item(row));
        std::string id = o ? gtk_string_object_get_string(o) : "";
        if (o) g_object_unref(o);
        return id;
    }
    std::string id_at(guint pos) const
    {
        GtkTreeListRow* row = gtk_tree_list_model_get_row(model_, pos);
        if (!row) return {};
        std::string id = id_of(row);
        g_object_unref(row);
        return id;
    }
    std::optional<guint> position_of(const std::string& id) const
    {
        for (guint i = 0, n = rows(); i < n; ++i)
            if (id_at(i) == id) return i;
        return std::nullopt;
    }
    bool select_quietly(const std::string& id)
    {
        if (!nodes_.contains(id)) return false;
        // Its ancestors open, outermost first.
        std::vector<std::string> chain;
        for (std::string p = parent_[id]; !p.empty(); p = parent_[p]) chain.push_back(p);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) set_expanded(*it, true);
        const std::optional<guint> pos = position_of(id);
        if (!pos) return false;
        gtk_single_selection_set_selected(sel_, *pos);
#if GTK_CHECK_VERSION(4, 12, 0)
        gtk_list_view_scroll_to(GTK_LIST_VIEW(w_), *pos, GTK_LIST_SCROLL_NONE, nullptr);
#endif
        return true;
    }
    void popup(std::optional<std::string> id, double x, double y)
    {
        if (!alive_ || !context_) return;
        menu m = context_(std::move(id));
        if (m.empty()) return;
        GMenu* model = ctx_menu_->build(std::move(m));
        if (popover_) gtk_widget_unparent(popover_);
        popover_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(model));
        g_object_unref(model);
        gtk_widget_set_parent(popover_, w_);
        GdkRectangle at{int(x), int(y), 1, 1};
        gtk_popover_set_pointing_to(GTK_POPOVER(popover_), &at);
        gtk_popover_set_has_arrow(GTK_POPOVER(popover_), false);
        gtk_popover_set_position(GTK_POPOVER(popover_), GTK_POS_BOTTOM);
        gtk_widget_set_halign(popover_, GTK_ALIGN_START);
        popup_when_built(popover_);
    }

    GListStore* root_ = nullptr;
    GtkTreeListModel* model_ = nullptr;
    GtkSingleSelection* sel_ = nullptr;
    std::unordered_map<std::string, node> nodes_;
    std::unordered_map<std::string, std::string> parent_;  // "" at the top
    std::unique_ptr<menu_actions> ctx_menu_;
    GtkWidget* popover_ = nullptr;
    std::function<void()> changed_;
    std::function<void(const std::string&)> activate_;
    std::function<menu(std::optional<std::string>)> context_;
    bool quiet_ = false;
    bool alive_ = true;
};

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
        // The font is known once styled, in a window.
        connect(w_, "realize", +[](GtkWidget*, gpointer p) { static_cast<drop_down_impl*>(p)->fit_widest(); }, this);
    }

    void set_items(std::vector<std::string> items) override
    {
        GtkStringList* list = gtk_string_list_new(nullptr);
        for (auto& i : items) gtk_string_list_append(list, i.c_str());
        gtk_drop_down_set_model(GTK_DROP_DOWN(w_), G_LIST_MODEL(list));
        g_object_unref(list);
        items_ = std::move(items);
        if (gtk_widget_get_realized(w_)) fit_widest();
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
    std::vector<std::string> items_;

    // GTK sizes the button to the item showing, so choosing a longer one
    // would widen it and push its neighbours about: as wide as the widest
    // item, as on GTK 3 and Qt. The button's own padding and arrow are
    // what its natural width has beyond the showing item's text.
    int text_width(const std::string& text) const
    {
        PangoLayout* layout = gtk_widget_create_pango_layout(w_, text.c_str());
        int w = 0;
        pango_layout_get_pixel_size(layout, &w, nullptr);
        g_object_unref(layout);
        return w;
    }
    void fit_widest()
    {
        gtk_widget_set_size_request(w_, -1, -1);
        int natural = 0;
        gtk_widget_measure(w_, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &natural, nullptr, nullptr);
        const int shown = current();
        const int chrome = natural - (shown >= 0 && size_t(shown) < items_.size() ? text_width(items_[shown]) : 0);
        int widest = 0;
        for (const std::string& i : items_) widest = std::max(widest, text_width(i));
        gtk_widget_set_size_request(w_, chrome + widest, -1);
    }
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
        {
            // Rows a line of text and a little, as Windows' list views have
            // them, rather than a rich list's: the cells have no vertical
            // padding, so painted cells (a log's graph) fill the row and meet
            // the next one's, and text cells take their space as margins.
            static bool installed = false;
            if (!installed)
            {
                GtkCssProvider* css = gtk_css_provider_new();
                gtk_css_provider_load_from_string(
                    css, "columnview.rapport-list > listview > row > cell { padding-top: 0; "
                         "padding-bottom: 0; }");
                gtk_style_context_add_provider_for_display(gtk_widget_get_display(w_),
                                                           GTK_STYLE_PROVIDER(css),
                                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
                g_object_unref(css);
                installed = true;
            }
            gtk_widget_add_css_class(w_, "rapport-list");
        }
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
        // Scrolling moves rows without redrawing them, so painted cells
        // drawn on whole device pixels (snap_to_device) would sit off them
        // at a fractional scale: once the scrolling stops, they redraw.
        for (GtkAdjustment* adj : {gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(outer_)),
                                   gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(outer_))})
            connect(adj, "value-changed",
                    +[](GtkAdjustment*, gpointer p) { static_cast<list_impl*>(p)->resnap_later(); }, this);
    }

    ~list_impl() override
    {
        if (resnap_) g_source_remove(resnap_);
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
    void on_reorder(reorder_fn f) override
    {
        reorder_ = std::move(f);
        if (reorder_) enable_rows_drag();
    }
    void set_reorderable(bool v) override { reorderable_ = v; }
    void on_drag_out(drag_out d) override
    {
        drag_out_ = std::move(d);
        if (!drag_out_.begin) return;
        enable_rows_drag();
        enable_drop_target();  // its own drag, back over it, reorders
    }
    void on_drop_in(drop_in d) override
    {
        drop_in_ = std::move(d);
        if (drop_in_.drop) enable_drop_target();
    }
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
    // The rows after the change keep their items and cells, renumbered;
    // the cells are bound again, as what they show can depend on the row.
    void rows_inserted(size_t first, size_t n) override
    {
        rapport_rows_renumber(rows_, guint(first), gint(n));
        rows_->count += guint(n);
        g_list_model_items_changed(G_LIST_MODEL(rows_), guint(first), 0, guint(n));
        rows_changed(first + n, rows_->count - first - n);
    }
    void rows_removed(size_t first, size_t n) override
    {
        rapport_rows_renumber(rows_, guint(first + n), -gint(n));
        rows_->count -= guint(n);
        g_list_model_items_changed(G_LIST_MODEL(rows_), guint(first), guint(n), 0);
        rows_changed(first, rows_->count - first);
    }

protected:
    void on_disconnect() override
    {
        alive_ = false;
        *life_ = false;
        stop_dnd_tick();
        set_line(nullptr, nullptr);
        end_drag_out();
        if (resnap_) g_source_remove(std::exchange(resnap_, 0));
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

    // -- dragging rows: reordering, drag_out and drop_in --------------------
    //
    // Reordering is done by hand, as on gtk3: a drag gesture on the column
    // view, the insertion line a CSS class on the row it's drawn against.
    // Only a drag that leaves the list (or any drag, with no on_reorder)
    // becomes a system drag, which other windows and processes can take.

    // The column view's own list of rows, under its header.
    GtkWidget* rows_view() const
    {
        for (GtkWidget* c = gtk_widget_get_first_child(w_); c; c = gtk_widget_get_next_sibling(c))
            if (std::string_view(gtk_widget_get_css_name(c)) == "listview") return c;
        return w_;
    }
    // A row widget's index, from the tagged cells in it.
    static std::optional<size_t> row_index(GtkWidget* row)
    {
        for (GtkWidget* c = gtk_widget_get_first_child(row); c; c = gtk_widget_get_next_sibling(c))
            if (GtkWidget* inner = gtk_widget_get_first_child(c); inner && row_of(inner))
                return row_of(inner) - 1;
        return std::nullopt;
    }
    // The row widget at a point of `in` (w_ or the rows view), if any.
    GtkWidget* row_widget_at(GtkWidget* in, double x, double y) const
    {
        for (GtkWidget* w = gtk_widget_pick(in, x, y, GTK_PICK_DEFAULT); w && w != w_;
             w = gtk_widget_get_parent(w))
            if (std::string_view(gtk_widget_get_css_name(w)) == "row") return w;
        return nullptr;
    }
    // The row widget showing `row` now; none when it's scrolled away.
    GtkWidget* row_widget_of(size_t row) const
    {
        for (auto [li, ctx] : bound_)
            if (row_of(gtk_list_item_get_item(li)) == row)
                return row_widget(gtk_list_item_get_child(li));
        return nullptr;
    }

    // The row a drop at (x, y) of w_ goes in front of (row_count() = the
    // end). Clamped to the visible rows, so the header (above them) means
    // the first visible row rather than the end, as on gtk3.
    size_t insertion_at(double x, double y) const
    {
        GtkWidget* view = rows_view();
        graphene_point_t in = GRAPHENE_POINT_INIT(float(x), float(y)), p;
        if (!gtk_widget_compute_point(w_, view, &in, &p)) p = in;
        const double w = gtk_widget_get_width(view), h = gtk_widget_get_height(view);
        const double px = std::clamp(double(p.x), 0.0, std::max(0.0, w - 1));
        const double py = std::clamp(double(p.y), 0.0, std::max(0.0, h - 1));
        GtkWidget* row = row_widget_at(view, px, py);
        std::optional<size_t> index = row ? row_index(row) : std::nullopt;
        graphene_rect_t r;
        if (!index || !gtk_widget_compute_bounds(row, view, &r))
            return rows_->count;  // empty space below the last row
        return py < r.origin.y + r.size.height / 2 ? *index : *index + 1;
    }

    // Dropping a contiguous block onto its own place changes nothing, so
    // that shows no line and a "not allowed" cursor (as gtk3 and qt6).
    bool is_noop_drop(size_t dest) const
    {
        auto sel = selection();
        if (sel.empty()) return true;
        const bool contiguous = sel.back() - sel.front() + 1 == sel.size();
        return contiguous && dest >= sel.front() && dest <= sel.back() + 1;
    }
    bool reorders() const { return reorder_ && reorderable_; }

    // The insertion line: a top or bottom edge drawn in the theme's
    // highlight colour on the row it's against. The rule is display-wide,
    // installed once; the class moves as the target does.
    void install_line_css()
    {
        static bool installed = false;
        if (installed) return;
        installed = true;
        const color c = theme_color_of(w_, theme_color::highlight);
        const std::string rgb = "rgb(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," +
                                std::to_string(c.b) + ")";
        const std::string rule =
            "columnview.rapport-list > listview > row.rapport-insert-before { box-shadow: inset 0 "
            "2px " +
            rgb +
            "; } columnview.rapport-list > listview > row.rapport-insert-after { box-shadow: "
            "inset 0 -2px " +
            rgb + "; }";
        GtkCssProvider* css = gtk_css_provider_new();
        gtk_css_provider_load_from_string(css, rule.c_str());
        gtk_style_context_add_provider_for_display(gtk_widget_get_display(w_),
                                                   GTK_STYLE_PROVIDER(css),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }
    void set_line(GtkWidget* row, const char* cls)
    {
        if (row == line_row_ && cls == line_class_) return;
        if (line_row_)
        {
            gtk_widget_remove_css_class(line_row_, line_class_);
            g_object_unref(line_row_);
        }
        line_row_ = row ? GTK_WIDGET(g_object_ref(row)) : nullptr;
        line_class_ = cls;
        if (line_row_) gtk_widget_add_css_class(line_row_, line_class_);
    }
    // Shows the line in front of row `dest`, or after the last row for the
    // end; nullopt hides it. (Re-found each time: scrolling rebinds rows.)
    void show_line(std::optional<size_t> dest)
    {
        const size_t count = rows_->count;
        if (!dest || !count) return set_line(nullptr, nullptr);
        const bool at_end = *dest >= count;
        GtkWidget* row = row_widget_of(at_end ? count - 1 : *dest);
        set_line(row, at_end ? "rapport-insert-after" : "rapport-insert-before");
    }

    struct rows_drag_state
    {
        bool pressed = false, dragging = false;
        bool asked_out = false;  // drag_out.begin was asked (once a drag)
        double x0 = 0, y0 = 0;   // the press, in w_
        double x = 0, y = 0;     // the pointer now
        size_t row = 0;
    };
    void enable_rows_drag()
    {
        if (rows_drag_) return;
        install_line_css();
        // Capture phase: a row's own click gesture would see the press
        // first otherwise. Nothing is claimed until the drag threshold, so
        // clicks still select; claiming cancels the row's click, which
        // selects on release, so a dragged multi-selection stays whole.
        rows_drag_ = gtk_gesture_drag_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rows_drag_), GDK_BUTTON_PRIMARY);
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(rows_drag_),
                                                   GTK_PHASE_CAPTURE);
        connect(
            rows_drag_, "drag-begin",
            +[](GtkGestureDrag* g, double x, double y, gpointer p)
            { static_cast<list_impl*>(p)->rows_drag_begin(g, x, y); },
            this);
        connect(
            rows_drag_, "drag-update",
            +[](GtkGestureDrag* g, double dx, double dy, gpointer p)
            { static_cast<list_impl*>(p)->rows_drag_update(g, dx, dy); },
            this);
        connect(
            rows_drag_, "drag-end",
            +[](GtkGestureDrag*, double, double, gpointer p)
            { static_cast<list_impl*>(p)->rows_drag_end(true); },
            this);
        // Another gesture took the press (a cell_drag): no reorder on "end".
        connect(
            rows_drag_, "cancel",
            +[](GtkGesture*, GdkEventSequence*, gpointer p)
            { static_cast<list_impl*>(p)->rows_drag_end(false); },
            this);
        gtk_widget_add_controller(w_, GTK_EVENT_CONTROLLER(rows_drag_));
        GtkEventController* keys = gtk_event_controller_key_new();
        connect(
            keys, "key-pressed",
            +[](GtkEventControllerKey*, guint key, guint, GdkModifierType, gpointer p) -> gboolean
            {
                auto* self = static_cast<list_impl*>(p);
                if (key != GDK_KEY_Escape || !self->rd_.dragging) return false;
                self->rows_drag_end(false);
                return true;
            },
            this);
        gtk_widget_add_controller(w_, keys);
    }
    void rows_drag_begin(GtkGestureDrag* g, double x, double y)
    {
        rd_ = {};
        const GdkModifierType mods =
            gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
        GtkWidget* row =
            (reorders() || drag_out_.begin) && !(mods & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
                ? row_widget_at(w_, x, y)
                : nullptr;  // Shift and Ctrl clicks select
        std::optional<size_t> index = row ? row_index(row) : std::nullopt;
        if (!index)
        {
            // Not on a row (the header, or below the rows).
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
            return;
        }
        rd_.pressed = true;
        rd_.x0 = rd_.x = x;
        rd_.y0 = rd_.y = y;
        rd_.row = *index;
    }
    void rows_drag_update(GtkGestureDrag* g, double dx, double dy)
    {
        if (!rd_.pressed || cell_.pressed) return;  // a cell_drag has it
        rd_.x = rd_.x0 + dx;
        rd_.y = rd_.y0 + dy;
        if (!rd_.dragging)
        {
            int threshold = 8;
            g_object_get(gtk_widget_get_settings(w_), "gtk-dnd-drag-threshold", &threshold,
                         nullptr);
            if (std::abs(dx) < threshold && std::abs(dy) < threshold) return;
            rd_.dragging = true;
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
            // A press on an unselected row drags that row alone.
            if (!gtk_selection_model_is_selected(sel_, guint(rd_.row)))
                gtk_selection_model_select_item(sel_, guint(rd_.row), true);
            gtk_widget_grab_focus(w_);  // Esc reaches the key controller
            if (!reorders())
            {
                rd_.asked_out = true;
                start_drag_out(g);  // or, declined, nothing until release
                return;
            }
            start_dnd_tick();
        }
        if (!reorders()) return;
        if (drag_out_.begin && !rd_.asked_out && outside_list(rd_.x, rd_.y))
        {
            rd_.asked_out = true;
            if (start_drag_out(g)) return;
        }
        update_reorder();
    }
    void update_reorder()
    {
        const size_t dest = insertion_at(rd_.x, rd_.y);
        const bool noop = is_noop_drop(dest);
        show_line(noop ? std::nullopt : std::optional<size_t>(dest));
        gtk_widget_set_cursor_from_name(w_, noop ? "not-allowed" : "grabbing");
    }
    void rows_drag_end(bool drop)
    {
        if (!rd_.pressed) return;
        const rows_drag_state d = rd_;
        rd_ = {};
        stop_dnd_tick();
        show_line(std::nullopt);
        gtk_widget_set_cursor(w_, nullptr);
        if (!drop || !d.dragging || !reorders()) return;
        const size_t dest = insertion_at(d.x, d.y);
        if (!is_noop_drop(dest)) reorder_(selection(), dest);
    }
    bool outside_list(double x, double y) const
    {
        graphene_point_t in = GRAPHENE_POINT_INIT(float(x), float(y)), p;
        if (!gtk_widget_compute_point(w_, outer_, &in, &p)) return false;
        return p.x < 0 || p.y < 0 || p.x >= gtk_widget_get_width(outer_) ||
               p.y >= gtk_widget_get_height(outer_);
    }

    // The selected rows' offer as a system drag (copy only). The gesture
    // is reset: the drag has the pointer now.
    bool start_drag_out(GtkGestureDrag* g)
    {
        const auto rows = selection();
        if (rows.empty() || out_drag_) return false;
        std::optional<data_offer> offer = drag_out_.begin(rows);
        if (!offer || offer->formats.empty()) return false;
        std::vector<GdkContentProvider*> providers;
        for (const auto& [type, data] : offer->formats)
        {
            GBytes* bytes = g_bytes_new(data.data(), data.size());
            providers.push_back(gdk_content_provider_new_for_bytes(type.c_str(), bytes));
            g_bytes_unref(bytes);
        }
        GdkContentProvider* content =
            gdk_content_provider_new_union(providers.data(), providers.size());

        // Where the drag started, relative to the pointer now, as
        // GtkDragSource works it out.
        GtkNative* native = gtk_widget_get_native(w_);
        GdkSurface* surface = gtk_native_get_surface(native);
        GdkDevice* device = gtk_gesture_get_device(GTK_GESTURE(g));
        graphene_point_t in = GRAPHENE_POINT_INIT(float(rd_.x0), float(rd_.y0)), start;
        if (!gtk_widget_compute_point(w_, GTK_WIDGET(native), &in, &start)) start = in;
        double px = 0, py = 0, nx = 0, ny = 0;
        gdk_surface_get_device_position(surface, device, &px, &py, nullptr);
        gtk_native_get_surface_transform(native, &nx, &ny);
        GdkDrag* drag =
            gdk_drag_begin(surface, device, content, GDK_ACTION_COPY, std::round(px - nx - start.x),
                           std::round(py - ny - start.y));
        g_object_unref(content);
        if (!drag) return false;

        // The icon: the first rows' first column, as the list shows them.
        std::string text;
        for (size_t i = 0; i < rows.size() && i < 3 && model_; ++i)
            text += (i ? "\n" : "") + model_->text(rows[i], 0);
        if (rows.size() > 3) text += "\n…";
        GtkWidget* icon = gtk_label_new(text.c_str());
        gtk_widget_add_css_class(icon, "rapport-drag-icon");
        gtk_drag_icon_set_child(GTK_DRAG_ICON(gtk_drag_icon_get_for_drag(drag)), icon);

        // Kept (and its handlers with it) until it ends, to know it when
        // it's dropped back on this list.
        out_drag_ = drag;
        g_signal_connect(
            drag, "dnd-finished",
            G_CALLBACK(+[](GdkDrag*, gpointer p) { static_cast<list_impl*>(p)->end_drag_out(); }),
            this);
        g_signal_connect(drag, "cancel",
                         G_CALLBACK(+[](GdkDrag*, GdkDragCancelReason, gpointer p)
                                    { static_cast<list_impl*>(p)->end_drag_out(); }),
                         this);

        rd_ = {};
        stop_dnd_tick();
        show_line(std::nullopt);
        gtk_widget_set_cursor(w_, nullptr);
        gtk_event_controller_reset(GTK_EVENT_CONTROLLER(g));
        return true;
    }

    void end_drag_out()
    {
        if (!out_drag_) return;
        g_signal_handlers_disconnect_by_data(out_drag_, this);
        g_clear_object(&out_drag_);
    }

    // Drops: data of drop_in.types from anywhere, and this list's own
    // drag, which reorders when it comes back. GtkDropTargetAsync, since
    // the data is read only at the drop (Wayland has none before).
    struct dnd_hover
    {
        GdkDrop* drop = nullptr;
        bool reorder = false;  // our own drag, back over the list
        std::string type;      // otherwise the drop_in type it carries
        double x = 0, y = 0;
        GdkDragAction action{};    // the last status given
        std::optional<size_t> at;  // where it would go, and whether it may
        bool ok = false;
    };
    void enable_drop_target()
    {
        if (drop_target_) return;
        install_line_css();
        drop_target_ = gtk_drop_target_async_new(nullptr, GDK_ACTION_COPY);
        connect(
            drop_target_, "accept", +[](GtkDropTargetAsync*, GdkDrop* drop, gpointer p) -> gboolean
            { return static_cast<list_impl*>(p)->dnd_accept(drop); }, this);
        auto motion = +[](GtkDropTargetAsync*, GdkDrop* drop, double x, double y, gpointer p)
        { return static_cast<list_impl*>(p)->dnd_motion(drop, x, y); };
        connect(drop_target_, "drag-enter", motion, this);
        connect(drop_target_, "drag-motion", motion, this);
        connect(
            drop_target_, "drag-leave",
            +[](GtkDropTargetAsync*, GdkDrop*, gpointer p)
            { static_cast<list_impl*>(p)->dnd_leave(); },
            this);
        connect(
            drop_target_, "drop",
            +[](GtkDropTargetAsync*, GdkDrop* drop, double x, double y, gpointer p) -> gboolean
            { return static_cast<list_impl*>(p)->dnd_drop(drop, x, y); }, this);
        gtk_widget_add_controller(w_, GTK_EVENT_CONTROLLER(drop_target_));
    }
    // Refused, a drag passes on to the window (its file drops).
    bool dnd_accept(GdkDrop* drop)
    {
        hover_ = {};
        if (!(gdk_drop_get_actions(drop) & GDK_ACTION_COPY)) return false;
        if (out_drag_ && gdk_drop_get_drag(drop) == out_drag_ && reorders())
        {
            hover_.reorder = true;
            return true;
        }
        if (!drop_in_.drop) return false;
        // The first of the drag's formats the list takes.
        gsize n = 0;
        const char* const* types =
            gdk_content_formats_get_mime_types(gdk_drop_get_formats(drop), &n);
        for (gsize i = 0; i < n; ++i)
            if (std::ranges::find(drop_in_.types, std::string_view(types[i])) !=
                drop_in_.types.end())
            {
                hover_.type = types[i];
                return true;
            }
        return false;
    }
    GdkDragAction dnd_motion(GdkDrop* drop, double x, double y)
    {
        hover_.drop = drop;
        hover_.x = x;
        hover_.y = y;
        start_dnd_tick();
        return hover_.action = dnd_target();
    }
    // Shows the line for where the hovering drag would go; its action.
    GdkDragAction dnd_target()
    {
        // Asked again only when the place changes: this runs every frame.
        if (const size_t at = insertion_at(hover_.x, hover_.y); at != hover_.at)
        {
            hover_.at = at;
            hover_.ok = hover_.reorder ? !is_noop_drop(at)
                                       : !drop_in_.can_drop || drop_in_.can_drop(at, hover_.type);
        }
        show_line(hover_.ok ? hover_.at : std::nullopt);
        return hover_.ok ? GDK_ACTION_COPY : GdkDragAction(0);
    }
    void dnd_leave()
    {
        hover_ = {};
        stop_dnd_tick();
        show_line(std::nullopt);
    }
    bool dnd_drop(GdkDrop* drop, double x, double y)
    {
        const dnd_hover h = hover_;
        dnd_leave();
        const size_t at = insertion_at(x, y);
        if (h.reorder)
        {
            const bool ok = reorders() && !is_noop_drop(at) && reorder_(selection(), at);
            gdk_drop_finish(drop, ok ? GDK_ACTION_COPY : GdkDragAction(0));
            return true;
        }
        if (h.type.empty() || !drop_in_.drop ||
            (drop_in_.can_drop && !drop_in_.can_drop(at, h.type)))
        {
            gdk_drop_finish(drop, GdkDragAction(0));
            return true;
        }
        // Read it all, then hand it over; the list may be gone by then.
        struct read
        {
            std::weak_ptr<bool> life;
            list_impl* self;
            GdkDrop* drop;
            size_t at;
            std::string type;
            GOutputStream* out = nullptr;
            void finish(bool ok)
            {
                gdk_drop_finish(drop, ok ? GDK_ACTION_COPY : GdkDragAction(0));
                g_object_unref(drop);
                if (out) g_object_unref(out);
                delete this;
            }
        };
        auto* r = new read{life_, this, GDK_DROP(g_object_ref(drop)), at, h.type};
        const char* types[] = {r->type.c_str(), nullptr};
        gdk_drop_read_async(
            drop, types, G_PRIORITY_DEFAULT, nullptr,
            +[](GObject* src, GAsyncResult* res, gpointer p)
            {
                auto* r = static_cast<read*>(p);
                GInputStream* in = gdk_drop_read_finish(GDK_DROP(src), res, nullptr, nullptr);
                if (!in) return r->finish(false);
                r->out = g_memory_output_stream_new_resizable();
                g_output_stream_splice_async(
                    r->out, in,
                    GOutputStreamSpliceFlags(G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                             G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET),
                    G_PRIORITY_DEFAULT, nullptr,
                    +[](GObject* out, GAsyncResult* res, gpointer p)
                    {
                        auto* r = static_cast<read*>(p);
                        if (g_output_stream_splice_finish(G_OUTPUT_STREAM(out), res, nullptr) < 0)
                            return r->finish(false);
                        auto life = r->life.lock();
                        if (!life || !*life || !r->self->drop_in_.drop) return r->finish(false);
                        GBytes* bytes =
                            g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(out));
                        gsize n = 0;
                        const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &n));
                        std::string s(data ? data : "", n);
                        g_bytes_unref(bytes);
                        r->finish(r->self->drop_in_.drop(r->at, r->type, s));
                    },
                    r);
                g_object_unref(in);
            },
            r);
        return true;
    }

    // While rows are dragged inside the list, it scrolls near its top and
    // bottom edges, faster the further in (or past) the edge the pointer
    // is. A system drag hovering over it is scrolled by GTK's list itself
    // (a drop motion controller on it); either way the rows move under a
    // still pointer, so the target is worked out again each frame.
    void start_dnd_tick()
    {
        if (dnd_tick_) return;
        dnd_last_ = 0;
        dnd_tick_ = gtk_widget_add_tick_callback(
            w_,
            +[](GtkWidget*, GdkFrameClock* clock, gpointer p) -> gboolean
            {
                static_cast<list_impl*>(p)->dnd_frame(clock);
                return G_SOURCE_CONTINUE;
            },
            this, nullptr);
    }
    void stop_dnd_tick()
    {
        if (dnd_tick_) gtk_widget_remove_tick_callback(w_, std::exchange(dnd_tick_, 0));
    }
    void dnd_frame(GdkFrameClock* clock)
    {
        const gint64 now = gdk_frame_clock_get_frame_time(clock);
        const double dt = dnd_last_ ? (now - dnd_last_) / 1e6 : 0;
        dnd_last_ = now;
        if (hover_.drop)
        {
            // GTK's status comes from motion: with a still pointer, it's
            // told here when the target under it changes its mind.
            if (const GdkDragAction a = dnd_target(); a != hover_.action)
                gdk_drop_status(hover_.drop, GDK_ACTION_COPY, hover_.action = a);
            return;
        }
        if (!rd_.dragging) return;
        GtkWidget* view = rows_view();
        graphene_rect_t r;
        if (gtk_widget_compute_bounds(view, w_, &r))
        {
            const double edge = 24, top = r.origin.y + edge,
                         bottom = r.origin.y + r.size.height - edge;
            const double over = rd_.y < top ? rd_.y - top : rd_.y > bottom ? rd_.y - bottom : 0;
            if (over != 0)
            {
                // Lines a second: 20 at the edge's depth, up to 60 past it.
                const double speed =
                    std::clamp(over / edge, -3.0, 3.0) * 20 * g_metrics.line_height;
                GtkAdjustment* adj =
                    gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(outer_));
                gtk_adjustment_set_value(adj, std::clamp(gtk_adjustment_get_value(adj) + speed * dt,
                                                         gtk_adjustment_get_lower(adj),
                                                         gtk_adjustment_get_upper(adj) -
                                                             gtk_adjustment_get_page_size(adj)));
            }
        }
        update_reorder();
    }

    reorder_fn reorder_;
    bool reorderable_ = true;
    drag_out drag_out_;
    drop_in drop_in_;
    GtkGesture* rows_drag_ = nullptr;
    rows_drag_state rd_;
    GtkDropTargetAsync* drop_target_ = nullptr;
    dnd_hover hover_;
    GdkDrag* out_drag_ = nullptr;  // our system drag, until it ends
    GtkWidget* line_row_ = nullptr;
    const char* line_class_ = nullptr;
    guint dnd_tick_ = 0;
    gint64 dnd_last_ = 0;
    std::shared_ptr<bool> life_ = std::make_shared<bool>(true);  // for async drop reads

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
            gtk_widget_set_vexpand(child, true);
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
            gtk_widget_set_margin_top(child, 2);
            gtk_widget_set_margin_bottom(child, 2);
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

    guint resnap_ = 0;
    void resnap_later()
    {
        if (!alive_ || device_scale_of(w_) == std::floor(device_scale_of(w_))) return;
        if (resnap_) g_source_remove(resnap_);
        resnap_ = g_timeout_add(
            100,
            +[](gpointer p) -> gboolean
            {
                auto* self = static_cast<list_impl*>(p);
                self->resnap_ = 0;
                for (auto [li, ctx] : self->bound_)
                    if (ctx->painted) gtk_widget_queue_draw(gtk_list_item_get_child(li));
                return G_SOURCE_REMOVE;
            },
            this);
    }

    void draw_cell(GtkDrawingArea* area, cairo_t* cr, int w, int h, column_ctx& ctx)
    {
        size_t tagged = row_of(area);
        if (!alive_ || !model_ || tagged == 0) return;
        size_t row = tagged - 1;
        if (row >= model_->row_count()) return;
        snap_to_device(cr, GTK_WIDGET(area));
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
        popup_when_built(popover_);
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
                // A new device scale is a resize in device pixels
                // (canvas::on_resize), told before the frame that uses it.
                // GTK has no signal for a fractional one: the widget's scale
                // factor is rounded (1.5 and 2 both read 2), and the surface
                // only exists once realized. It changes when the compositor's
                // fractional scale arrives after the window maps, and on a
                // move to another monitor.
                const float scale = device_scale_of(GTK_WIDGET(area));
                if (self->scale_ > 0 && scale != self->scale_)
                {
                    self->scale_ = scale;
                    if (self->resize_) self->resize_({float(w), float(h)});
                    self->target_changed();
                }
                // The frame and the painting go on the whole device pixels;
                // the slivers round them are under a pixel, and show the
                // window behind, the frame's own background.
                const device_span span = device_span_of(GTK_WIDGET(area), scale);
                if (span.width != self->span_.width || span.height != self->span_.height)
                {
                    self->span_ = span;
                    self->target_changed();
                }
                self->span_ = span;
                if (!self->paint_ && !self->frame_) return;
                cairo_translate(cr, span.shift.x, span.shift.y);
                cairo_painter painter(cr, GTK_WIDGET(area));
                self->frame_.paint(painter, span.size);
                if (self->paint_) self->paint_(painter, span.size);
            },
            this, nullptr);
        connect(
            w_, "resize",
            +[](GtkDrawingArea*, int w, int h, gpointer p)
            {
                auto* self = static_cast<canvas_impl*>(p);
                self->scale_ = device_scale_of(self->w_);
                if (self->resize_) self->resize_({float(w), float(h)});
                self->target_changed();
            },
            this);
        focusable_ = s.focusable;
        // GTK replays the last drawing at a new surface scale without calling
        // the draw func, so the check there wouldn't run: the surface's
        // scale changing asks for a draw. Watched while realized, since the
        // surface lives only that long.
        connect(
            w_, "realize",
            +[](GtkWidget*, gpointer p) { static_cast<canvas_impl*>(p)->watch_scale(); }, this);
        connect(
            w_, "unrealize",
            +[](GtkWidget*, gpointer p) { static_cast<canvas_impl*>(p)->unwatch_scale(); }, this);

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

    // The whole device pixels inside the canvas, where its frame is drawn.
    rap::frame_target target() const override
    {
        const float scale = device_scale_of(w_);
        const device_span span = device_span_of(w_, scale);
        frame_target t;
        t.width = span.width;
        t.height = span.height;
        t.scale = scale;
        return t;
    }
    void on_frame_target(std::function<void(const rap::frame_target&)> f) override { frame_target_ = std::move(f); }
    void present(std::shared_ptr<const rap::image> pixels, frame_color color) override
    {
        readback_.cancel();
        frame_ = {std::move(pixels), color};
        gtk_widget_queue_draw(w_);
    }
    void clear_frame() override
    {
        readback_.cancel();
        frame_ = {};
        gtk_widget_queue_draw(w_);
    }
    // The process's shared device; its textures are shown by reading them
    // back (gpu_readback.h) until this backend takes them as they are.
    std::shared_ptr<rap::gpu::device> gpu_device() const override
    {
        // Not before the window has GTK's own renderer: making it (Vulkan,
        // by default) loads the GPU driver on this thread, and NVIDIA's
        // crashes when rapport's thread loads it at the same time. GTK keeps
        // that instance for the display, so later windows don't load it
        // again. Until then null; drawing asks again.
        GtkNative* native = gtk_widget_get_native(w_);
        if (!native || !gtk_native_get_renderer(native)) return nullptr;
        // Redrawn when it arrives: an app drawing on it takes it then.
        return readback_.device(
            [](std::function<void()> f)
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
              },
            [this] { gtk_widget_queue_draw(w_); });
    }
    void present(rap::gpu::texture_ptr texture, rap::gpu::ticket, frame_color color) override
    {
        readback_.present(
            texture, color,
            [](std::function<void()> f)
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
              },
            [](int w, int h, pixel_format format, const uint8_t* px, size_t stride) -> std::shared_ptr<const rap::image>
            { return std::make_shared<cairo_image>(w, h, format, px, stride); },
            [this](std::shared_ptr<const rap::image> image, frame_color c) { present(std::move(image), c); });
    }
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
        unwatch_scale();
        if (!*alive_) return;
        *alive_ = false;
        for (guint id : ticks_) gtk_widget_remove_tick_callback(w_, id);
        ticks_.clear();
    }

private:
    float scale_ = 0;  // the device scale the app last heard of
    GdkSurface* scale_surface_ = nullptr;
    gulong scale_handler_ = 0;
    GdkFrameClock* layout_clock_ = nullptr;
    gulong layout_handler_ = 0;
    device_span span_;  // where the last drawing went

    void watch_scale()
    {
        unwatch_scale();
#if GTK_CHECK_VERSION(4, 12, 0)
        GtkNative* native = gtk_widget_get_native(w_);
        scale_surface_ = native ? gtk_native_get_surface(native) : nullptr;
        if (scale_surface_)
            scale_handler_ = g_signal_connect(
                scale_surface_, "notify::scale",
                G_CALLBACK(+[](GdkSurface*, GParamSpec*, gpointer p)
                           { gtk_widget_queue_draw(static_cast<canvas_impl*>(p)->w_); }),
                this);
#endif
        // A move that keeps the size reuses the last drawing without calling
        // the draw func, so the snapping to device pixels would go stale:
        // after each layout, a canvas that has moved off its pixels redraws.
        layout_clock_ = gtk_widget_get_frame_clock(w_);
        if (layout_clock_)
            layout_handler_ = g_signal_connect(
                layout_clock_, "layout",
                G_CALLBACK(+[](GdkFrameClock*, gpointer p)
                           {
                               auto* self = static_cast<canvas_impl*>(p);
                               if (device_span_of(self->w_, device_scale_of(self->w_)) != self->span_)
                                   gtk_widget_queue_draw(self->w_);
                           }),
                this);
    }
    void unwatch_scale()
    {
        if (scale_surface_ && scale_handler_)
            g_signal_handler_disconnect(scale_surface_, scale_handler_);
        scale_surface_ = nullptr;
        scale_handler_ = 0;
        if (layout_clock_ && layout_handler_) g_signal_handler_disconnect(layout_clock_, layout_handler_);
        layout_clock_ = nullptr;
        layout_handler_ = 0;
    }
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
    frames::frame_layer frame_;
    frames::gpu_readback readback_;  // GPU textures shown as pixels
    std::function<void(const rap::frame_target&)> frame_target_;
    void target_changed()
    {
        if (frame_target_) frame_target_(target());
    }
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
        // Test runs (RAPPORT_NO_ACTIVATE): shown, not presented (presenting
        // asks for the focus).
        if (std::getenv("RAPPORT_NO_ACTIVATE"))
            gtk_widget_set_visible(win_, TRUE);
        else
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
    static bool is_line_control(const rap::node_spec& s)
    {
        return std::holds_alternative<spec::label>(s) || std::holds_alternative<spec::button>(s) ||
               std::holds_alternative<spec::check>(s) || std::holds_alternative<spec::entry>(s) ||
               std::holds_alternative<spec::drop_down>(s) || std::holds_alternative<spec::combo>(s) ||
               std::holds_alternative<spec::menu_button>(s) ||
               std::holds_alternative<spec::progress_bar>(s) ||
               std::holds_alternative<spec::date_picker>(s);
    }

    static bool fills_form_row(const rap::node_spec& s)
    {
        return std::holds_alternative<spec::entry>(s) || std::holds_alternative<spec::combo>(s) ||
               std::holds_alternative<spec::text_view>(s) || std::holds_alternative<spec::list>(s) ||
               std::holds_alternative<spec::tree>(s) ||
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
        for (auto& c : s.items)
        {
            GtkWidget* w = build(c, s.dir);
            // Single-line controls keep their natural height when a taller
            // sibling (a stack of checkboxes) sets the row height.
            if (s.dir == orientation::horizontal && is_line_control(c.spec))
                gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(box), w);
        }
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
    GtkWidget* make(const node& n, const spec::tree& s, orientation) { return add<tree_impl>(n, s).outer(); }

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
    // Wayland hands the clipboard only to the client with keyboard focus, so
    // a window just created can't read it yet: wait (polling, 50 ms) until
    // one of ours is active, then a tick more for the selection to arrive;
    // after 1.5 s without (no window manager, say), read anyway.
    void read_clipboard_text(std::function<void(std::optional<std::string>)> done) override
    {
        struct waiting
        {
            std::function<void(std::optional<std::string>)> done;
            int ticks = 0;
            bool active = false;
        };
        g_timeout_add_full(
            G_PRIORITY_DEFAULT, 50,
            +[](gpointer p) -> gboolean
            {
                auto* w = static_cast<waiting*>(p);
                bool active = false;
                GListModel* windows = gtk_window_get_toplevels();
                for (guint i = 0, n = g_list_model_get_n_items(windows); i < n && !active; ++i)
                {
                    auto* win = static_cast<GtkWindow*>(g_list_model_get_item(windows, i));
                    active = gtk_window_is_active(win);
                    g_object_unref(win);
                }
                if (!w->active && active && ++w->ticks < 30)
                {
                    w->active = true;  // the selection follows the focus: next tick
                    return G_SOURCE_CONTINUE;
                }
                if (!w->active && ++w->ticks < 30) return G_SOURCE_CONTINUE;
                read_clipboard_now(std::move(w->done));
                return G_SOURCE_REMOVE;
            },
            new waiting{.done = std::move(done)}, +[](gpointer p) { delete static_cast<waiting*>(p); });
    }
    static void read_clipboard_now(std::function<void(std::optional<std::string>)> done)
    {
        GdkDisplay* d = gdk_display_get_default();
        if (!d) return done(std::nullopt);
        gdk_clipboard_read_text_async(
            gdk_display_get_clipboard(d), nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer user)
            {
                std::unique_ptr<std::function<void(std::optional<std::string>)>> f(
                    static_cast<std::function<void(std::optional<std::string>)>*>(user));
                char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, nullptr);
                if (!text) return (*f)(std::nullopt);
                std::string s = text;
                g_free(text);
                (*f)(std::move(s));
            },
            new std::function<void(std::optional<std::string>)>(std::move(done)));
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
