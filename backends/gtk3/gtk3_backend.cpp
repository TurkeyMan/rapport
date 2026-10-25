// GTK 3 backend (package rapport-gtk3).
//
// Lists use GtkTreeView in fixed-height mode over a virtual GtkTreeModel
// (RapportTreeRows) whose iters carry the row index; cells pull text from the
// rap::list_model through cell data functions, so only visible rows are
// ever touched. Painted columns use a custom cell renderer.
#include <gtk/gtk.h>

#include "builtin_icons.h"
#include "gtk_common.h"
#include "frame_layer.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>

#ifdef RAPPORT_GTK3_FRACTIONAL
#include <gdk/gdkwayland.h>

#include "fractional-scale-v1-client-protocol.h"
#endif

// -- RapportTreeRows ----------------------------------------------------------

G_BEGIN_DECLS
G_DECLARE_FINAL_TYPE(RapportTreeRows, rapport_tree_rows, RAPPORT, TREE_ROWS, GObject)
G_END_DECLS

struct _RapportTreeRows
{
    GObject parent_instance;
    gint stamp;
    guint count;
    rap::list_model* model;
    guint search_column;
};

static guint row_of(GtkTreeIter* iter)
{
    return GPOINTER_TO_UINT(iter->user_data);
}

static gboolean set_iter(RapportTreeRows* self, GtkTreeIter* iter, guint row)
{
    if (row >= self->count)
    {
        iter->stamp = 0;
        return false;
    }
    iter->stamp = self->stamp;
    iter->user_data = GUINT_TO_POINTER(row);
    return true;
}

static GtkTreeModelFlags rows_get_flags(GtkTreeModel*)
{
    return GtkTreeModelFlags(GTK_TREE_MODEL_LIST_ONLY | GTK_TREE_MODEL_ITERS_PERSIST);
}
static gint rows_get_n_columns(GtkTreeModel*)
{
    return 1;
}
static GType rows_get_column_type(GtkTreeModel*, gint)
{
    return G_TYPE_STRING;
}

static gboolean rows_get_iter(GtkTreeModel* m, GtkTreeIter* iter, GtkTreePath* path)
{
    if (gtk_tree_path_get_depth(path) != 1) return false;
    return set_iter(RAPPORT_TREE_ROWS(m), iter, guint(gtk_tree_path_get_indices(path)[0]));
}

static GtkTreePath* rows_get_path(GtkTreeModel*, GtkTreeIter* iter)
{
    return gtk_tree_path_new_from_indices(int(row_of(iter)), -1);
}

// Only used by type-ahead search; cells use data functions instead.
static void rows_get_value(GtkTreeModel* m, GtkTreeIter* iter, gint, GValue* value)
{
    auto* self = RAPPORT_TREE_ROWS(m);
    g_value_init(value, G_TYPE_STRING);
    if (self->model && row_of(iter) < self->count)
        g_value_set_string(value, self->model->text(row_of(iter), self->search_column).c_str());
}

static gboolean rows_iter_next(GtkTreeModel* m, GtkTreeIter* iter)
{
    return set_iter(RAPPORT_TREE_ROWS(m), iter, row_of(iter) + 1);
}

static gboolean rows_iter_previous(GtkTreeModel* m, GtkTreeIter* iter)
{
    guint row = row_of(iter);
    return row > 0 && set_iter(RAPPORT_TREE_ROWS(m), iter, row - 1);
}

static gboolean rows_iter_children(GtkTreeModel* m, GtkTreeIter* iter, GtkTreeIter* parent)
{
    return !parent && set_iter(RAPPORT_TREE_ROWS(m), iter, 0);
}

static gboolean rows_iter_has_child(GtkTreeModel*, GtkTreeIter*)
{
    return false;
}

static gint rows_iter_n_children(GtkTreeModel* m, GtkTreeIter* iter)
{
    return iter ? 0 : gint(RAPPORT_TREE_ROWS(m)->count);
}

static gboolean rows_iter_nth_child(GtkTreeModel* m, GtkTreeIter* iter, GtkTreeIter* parent, gint n)
{
    return !parent && n >= 0 && set_iter(RAPPORT_TREE_ROWS(m), iter, guint(n));
}

static gboolean rows_iter_parent(GtkTreeModel*, GtkTreeIter*, GtkTreeIter*)
{
    return false;
}

static void rapport_tree_rows_iface_init(GtkTreeModelIface* iface)
{
    iface->get_flags = rows_get_flags;
    iface->get_n_columns = rows_get_n_columns;
    iface->get_column_type = rows_get_column_type;
    iface->get_iter = rows_get_iter;
    iface->get_path = rows_get_path;
    iface->get_value = rows_get_value;
    iface->iter_next = rows_iter_next;
    iface->iter_previous = rows_iter_previous;
    iface->iter_children = rows_iter_children;
    iface->iter_has_child = rows_iter_has_child;
    iface->iter_n_children = rows_iter_n_children;
    iface->iter_nth_child = rows_iter_nth_child;
    iface->iter_parent = rows_iter_parent;
}

G_DEFINE_TYPE_WITH_CODE(RapportTreeRows, rapport_tree_rows, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GTK_TYPE_TREE_MODEL, rapport_tree_rows_iface_init))

static void rapport_tree_rows_class_init(RapportTreeRowsClass*) {}

static void rapport_tree_rows_init(RapportTreeRows* self)
{
    self->stamp = g_random_int();
    self->count = 0;
    self->model = nullptr;
    self->search_column = 0;
}

// -- RapportPaintRenderer -----------------------------------------------------

// Cell renderer for painted columns; the column's data function sets the
// row, and the owning list draws through `draw`.
using paint_fn = void (*)(gpointer ctx, guint row, cairo_t*, GtkWidget*, const GdkRectangle*,
                         GtkCellRendererState);

G_BEGIN_DECLS
G_DECLARE_FINAL_TYPE(RapportPaintRenderer, rapport_paint_renderer, RAPPORT, PAINT_RENDERER,
                     GtkCellRenderer)
G_END_DECLS

struct _RapportPaintRenderer
{
    GtkCellRenderer parent_instance;
    guint row;
    gpointer ctx;
    paint_fn draw;
    gint height;
};

G_DEFINE_TYPE(RapportPaintRenderer, rapport_paint_renderer, GTK_TYPE_CELL_RENDERER)

static void paint_renderer_render(GtkCellRenderer* cell, cairo_t* cr, GtkWidget* widget,
                                  const GdkRectangle* background, const GdkRectangle* area,
                                  GtkCellRendererState flags)
{
    auto* self = RAPPORT_PAINT_RENDERER(cell);
    // Full row height so graph lines join between rows; the cell area
    // excludes the view's vertical padding.
    GdkRectangle full{area->x, background->y, area->width, background->height};
    if (self->draw) self->draw(self->ctx, self->row, cr, widget, &full, flags);
}

static void paint_renderer_get_preferred_height(GtkCellRenderer* cell, GtkWidget*, gint* min,
                                                gint* natural)
{
    // Either pointer may be null.
    gint h = RAPPORT_PAINT_RENDERER(cell)->height;
    if (min) *min = h;
    if (natural) *natural = h;
}

static void paint_renderer_get_preferred_width(GtkCellRenderer*, GtkWidget*, gint* min,
                                               gint* natural)
{
    if (min) *min = 1;
    if (natural) *natural = 1;
}

static void rapport_paint_renderer_class_init(RapportPaintRendererClass* klass)
{
    auto* cell = GTK_CELL_RENDERER_CLASS(klass);
    cell->render = paint_renderer_render;
    cell->get_preferred_height = paint_renderer_get_preferred_height;
    cell->get_preferred_width = paint_renderer_get_preferred_width;
}

static void rapport_paint_renderer_init(RapportPaintRenderer* self)
{
    self->row = 0;
    self->ctx = nullptr;
    self->draw = nullptr;
    self->height = 16;
}

namespace rap::gtk3
{
namespace
{
using namespace gtkcommon;
}
}  // namespace rap::gtk3

rap::color rap::gtkcommon::theme_color_of(GtkWidget* w, theme_color role)
{
    GtkStyleContext* ctx = gtk_widget_get_style_context(w);
    GdkRGBA fg;
    gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg);

    // Named colours every GTK 3 theme is expected to define.
    const char* name = nullptr;
    switch (role)
    {
    case theme_color::window: name = "theme_bg_color"; break;
    case theme_color::window_text: name = "theme_fg_color"; break;
    case theme_color::base: name = "theme_base_color"; break;
    case theme_color::text: name = "theme_text_color"; break;
    case theme_color::highlight:
    case theme_color::accent: name = "theme_selected_bg_color"; break;
    case theme_color::highlight_text: name = "theme_selected_fg_color"; break;
    case theme_color::positive: name = "success_color"; break;
    case theme_color::negative: name = "error_color"; break;
    case theme_color::link: name = "link_color"; break;
    default: break;
    }
    GdkRGBA c;
    if (name && gtk_style_context_lookup_color(ctx, name, &c)) return to_color(c);
    return fallback_color(role, to_color(fg), is_dark_text(fg));
}

namespace rap::gtk3
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

GtkWidget* icon_widget(const icon& icon, int size);

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
    // link it takes the link's click and pointer (an event box, as GTK 3's
    // images have no window of their own).
    void add_icon(const icon& icon)
    {
        GtkWidget* img = icon_widget(icon, 16);
        if (!img) return;
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget* holder = img;
        if (link_)
        {
            holder = gtk_event_box_new();
            gtk_container_add(GTK_CONTAINER(holder), img);
            gtk_widget_add_events(holder, GDK_BUTTON_RELEASE_MASK);
            g_signal_connect(holder, "button-release-event",
                             G_CALLBACK(+[](GtkWidget*, GdkEventButton* e, gpointer p) -> gboolean
                                        {
                                            auto* self = static_cast<label_impl*>(p);
                                            if (e->button == 1 && self->on_click_) self->on_click_();
                                            return true;
                                        }),
                             this);
            g_signal_connect(holder, "realize",
                             G_CALLBACK(+[](GtkWidget* w, gpointer)
                                        {
                                            GdkCursor* c = gdk_cursor_new_from_name(gtk_widget_get_display(w), "pointer");
                                            gdk_window_set_cursor(gtk_widget_get_window(w), c);
                                            if (c) g_object_unref(c);
                                        }),
                             nullptr);
        }
        gtk_box_pack_start(GTK_BOX(box), holder, false, false, 0);
        gtk_box_pack_start(GTK_BOX(box), w_, true, true, 0);
        gtk_widget_show_all(box);
        outer_ = box;
    }

    bool link_ = false;
    std::function<void()> on_click_;
};

// A GtkImage for a named icon the theme has; otherwise a drawing area
// (rapport's own drawing of that name, else icon::draw) that runs whenever
// it paints, so drawn icons follow scale and theme (the area inherits its
// parent's CSS colour, e.g. greyed when insensitive); else the theme's
// image-missing, never a blank. Null for no icon at all.
GtkWidget* icon_widget(const icon& icon, int size)
{
    auto named = [&](const char* name)
    {
        GtkWidget* img = gtk_image_new_from_icon_name(name, GTK_ICON_SIZE_BUTTON);
        gtk_image_set_pixel_size(GTK_IMAGE(img), size);
        return img;
    };
    if (!icon.name.empty() &&
        gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), icon.name.c_str()))
        return named(icon.name.c_str());
    std::function<void(rap::painter&, rap::size_f)> draw = icon.draw;
    if (!icon.name.empty() && builtin::has_builtin_icon(icon.name))
        draw = [name = icon.name](rap::painter& p, rap::size_f s) { builtin::draw_builtin_icon(name, p, s); };
    if (!draw) return icon.name.empty() ? nullptr : named("image-missing");
    GtkWidget* area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, size, size);
    // Square and centred, however tall the toolbar row is.
    gtk_widget_set_halign(area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(area, GTK_ALIGN_CENTER);
    g_signal_connect_data(
        area, "draw",
        G_CALLBACK(+[](GtkWidget* w, cairo_t* cr, gpointer p) -> gboolean
                   {
                       cairo_painter painter(cr, w);
                       (*static_cast<std::function<void(rap::painter&, rap::size_f)>*>(p))(
                           painter, {float(gtk_widget_get_allocated_width(w)),
                                     float(gtk_widget_get_allocated_height(w))});
                       return true;
                   }),
        new std::function<void(rap::painter&, rap::size_f)>(std::move(draw)),
        +[](gpointer p, GClosure*)
        { delete static_cast<std::function<void(rap::painter&, rap::size_f)>*>(p); },
        GConnectFlags(0));
    return area;
}

class button_impl : public widget_base<rap::button>
{
public:
    explicit button_impl(const std::string& text)
        : widget_base(gtk_button_new_with_mnemonic(text.c_str()))
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

    void set_text(const std::string& t) override { gtk_button_set_label(GTK_BUTTON(w_), t.c_str()); }
    void set_icon(const icon& icon) override
    {
        GtkWidget* img = icon_widget(icon, 16);
        gtk_button_set_image(GTK_BUTTON(w_), img);
        gtk_button_set_always_show_image(GTK_BUTTON(w_), img != nullptr);
        if (img) gtk_widget_show(img);
    }
    void on_click(std::function<void()> f) override { click_ = std::move(f); }

private:
    std::function<void()> click_;
};

// "A file named “x” already exists. Replace it?" - true for Replace.
bool confirm_replace(GtkWindow* parent, const std::string& path)
{
    gchar* name = g_path_get_basename(path.c_str());
    const std::string question = rap::i18n::format(
        rap::i18n::tr("rapport", "A file named \u201c{0}\u201d already exists. Replace it?"), name);
    GtkWidget* msg = gtk_message_dialog_new(parent, GtkDialogFlags(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", question.c_str());
    g_free(name);
    gtk_dialog_add_buttons(GTK_DIALOG(msg), stock_label(dialog_role::cancel).c_str(), GTK_RESPONSE_CANCEL,
                           rap::i18n::tr_ctx("rapport", "button", "_Replace"), GTK_RESPONSE_ACCEPT, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(msg), GTK_RESPONSE_CANCEL);
    const bool replace = gtk_dialog_run(GTK_DIALOG(msg)) == GTK_RESPONSE_ACCEPT;
    gtk_widget_destroy(msg);
    return replace;
}

GtkWidget* build_menu(const menu& items);

// Appends `items` to `menu`; the GTK items point into `items`, which must
// outlive them.
void fill_menu(GtkWidget* menu, const rap::menu& items)
{
    for (const menu_item& item : items)
    {
        GtkWidget* mi;
        if (item.is_separator())
        {
            mi = gtk_separator_menu_item_new();
        }
        else if (!item.children.empty())
        {
            mi = gtk_menu_item_new_with_mnemonic(item.label.c_str());
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), build_menu(item.children));
        }
        else
        {
            if (item.checked)
            {
                mi = gtk_check_menu_item_new_with_mnemonic(item.label.c_str());
                gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(mi), item.radio);
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), *item.checked);
            }
            else
            {
                mi = gtk_menu_item_new_with_mnemonic(item.label.c_str());
            }
            gtk_widget_set_sensitive(mi, item.enabled && item.action);
            if (!item.shortcut.empty())
            {
                guint key;
                GdkModifierType mods;
                gtk_accelerator_parse(gtk_accel(item.shortcut).c_str(), &key, &mods);
                GtkWidget* label = gtk_bin_get_child(GTK_BIN(mi));
                if (key && GTK_IS_ACCEL_LABEL(label))
                    gtk_accel_label_set_accel(GTK_ACCEL_LABEL(label), key, mods);
            }
            // The menu lives no longer than menuItems_, which owns `item`.
            g_signal_connect(mi, "activate",
                             G_CALLBACK(+[](GtkMenuItem*, gpointer p)
                                        {
                                            auto* item = static_cast<const menu_item*>(p);
                                            if (item->action) item->action();
                                        }),
                             const_cast<menu_item*>(&item));
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
}

GtkWidget* build_menu(const menu& items)
{
    GtkWidget* menu = gtk_menu_new();
    fill_menu(menu, items);
    return menu;
}

// A popup menu built from a rap::menu. Its GTK items point into the
// Menu it holds, so the two are replaced together.
class popup_menu
{
public:
    // Shows below `attach` when `event` is null (keyboard / menu buttons),
    // else at the pointer.
    void show(menu items, GtkWidget* attach, GdkEvent* event, bool below = false)
    {
        destroy();
        items_ = std::move(items);
        menu_ = build_menu(items_);
        gtk_menu_attach_to_widget(GTK_MENU(menu_), attach, nullptr);
        if (on_hide_)
            g_signal_connect(menu_, "deactivate",
                             G_CALLBACK(+[](GtkMenuShell*, gpointer p)
                                        { (*static_cast<std::function<void()>*>(p))(); }),
                             &on_hide_);
        gtk_widget_show_all(menu_);
        if (below)
            gtk_menu_popup_at_widget(GTK_MENU(menu_), attach, GDK_GRAVITY_SOUTH_WEST,
                                     GDK_GRAVITY_NORTH_WEST, event);
        else if (event)
            gtk_menu_popup_at_pointer(GTK_MENU(menu_), event);
        else
            gtk_menu_popup_at_widget(GTK_MENU(menu_), attach, GDK_GRAVITY_CENTER,
                                     GDK_GRAVITY_NORTH_WEST, nullptr);
    }

    void on_hide(std::function<void()> f) { on_hide_ = std::move(f); }

    // Call before the attach widget goes away.
    void destroy()
    {
        if (menu_) gtk_widget_destroy(menu_);
        menu_ = nullptr;
    }

private:
    menu items_;
    GtkWidget* menu_ = nullptr;
    std::function<void()> on_hide_;
};

class tool_button_impl : public widget_base<rap::tool_button>
{
public:
    explicit tool_button_impl(const spec::tool_button& s)
        : widget_base(GTK_WIDGET(s.toggle ? gtk_toggle_tool_button_new()
                                : gtk_tool_button_new(nullptr, s.label.c_str())))
        , toggle_(s.toggle)
    {
        gtk_tool_button_set_label(GTK_TOOL_BUTTON(w_), s.label.c_str());
        gtk_widget_set_tooltip_text(w_, s.label.c_str());
        set_icon(s.icon);
        if (toggle_)
        {
            gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(w_), s.checked);
            connect(
                w_, "toggled",
                +[](GtkToggleToolButton* b, gpointer p)
                {
                    auto* self = static_cast<tool_button_impl*>(p);
                    if (!self->quiet_ && self->toggled_)
                        self->toggled_(gtk_toggle_tool_button_get_active(b));
                },
                this);
        }
        connect(
            w_, "clicked",
            +[](GtkToolButton*, gpointer p)
            {
                auto* self = static_cast<tool_button_impl*>(p);
                if (!self->quiet_ && self->click_) self->click_();
            },
            this);
    }

    void set_icon(const icon& icon) override
    {
        GtkWidget* img = icon_widget(icon, 16);
        gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(w_), img);
        if (img) gtk_widget_show(img);
    }
    bool checked() const override
    {
        return toggle_ && gtk_toggle_tool_button_get_active(GTK_TOGGLE_TOOL_BUTTON(w_));
    }
    void set_checked(bool c) override
    {
        if (!toggle_) return;
        quiet_ = true;
        gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(w_), c);
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

class slider_impl : public widget_base<rap::slider>
{
public:
    explicit slider_impl(const spec::slider& s)
        : widget_base(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, s.min, s.max,
                                     s.step > 0 ? s.step : 0.01))
    {
        gtk_range_set_increments(GTK_RANGE(w_), s.step, s.page);
        gtk_scale_set_digits(GTK_SCALE(w_), s.digits);
        gtk_scale_set_draw_value(GTK_SCALE(w_), s.show_value);
        gtk_range_set_value(GTK_RANGE(w_), s.value);
        for (auto& [at, label] : s.marks)
            gtk_scale_add_mark(GTK_SCALE(w_), at, GTK_POS_BOTTOM,
                               label.empty() ? nullptr : label.c_str());
        connect(
            w_, "value-changed",
            +[](GtkRange* r, gpointer p)
            {
                auto* self = static_cast<slider_impl*>(p);
                if (!self->quiet_ && self->changed_) self->changed_(gtk_range_get_value(r));
            },
            this);
    }

    double value() const override { return gtk_range_get_value(GTK_RANGE(w_)); }
    void set_value(double v) override
    {
        quiet_ = true;
        gtk_range_set_value(GTK_RANGE(w_), v);
        quiet_ = false;
    }
    void set_range(double min, double max) override
    {
        quiet_ = true;
        gtk_range_set_range(GTK_RANGE(w_), min, max);
        quiet_ = false;
    }
    void on_changed(std::function<void(double)> f) override { changed_ = std::move(f); }

private:
    bool quiet_ = false;
    std::function<void(double)> changed_;
};

class spin_impl : public widget_base<rap::spin>
{
public:
    explicit spin_impl(const spec::spin& s)
        : widget_base(gtk_spin_button_new_with_range(s.min, s.max, s.step > 0 ? s.step : 1))
    {
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(w_), guint(std::max(s.digits, 0)));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w_), s.value);
        connect(
            w_, "value-changed",
            +[](GtkSpinButton* b, gpointer p)
            {
                auto* self = static_cast<spin_impl*>(p);
                if (!self->quiet_ && self->changed_) self->changed_(gtk_spin_button_get_value(b));
            },
            this);
    }

    double value() const override { return gtk_spin_button_get_value(GTK_SPIN_BUTTON(w_)); }
    void set_value(double v) override
    {
        quiet_ = true;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w_), v);
        quiet_ = false;
    }
    void set_range(double min, double max) override
    {
        quiet_ = true;
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(w_), min, max);
        quiet_ = false;
    }
    void on_changed(std::function<void(double)> f) override { changed_ = std::move(f); }

private:
    bool quiet_ = false;
    std::function<void(double)> changed_;
};

GdkRGBA to_rgba(color c)
{
    return {c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0};
}

class color_button_impl : public widget_base<rap::color_button>
{
public:
    explicit color_button_impl(const spec::color_button& s)
        : widget_base(gtk_color_button_new())
    {
        const GdkRGBA rgba = to_rgba(s.color);
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(w_), &rgba);
        gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(w_), s.alpha);
        if (!s.title.empty()) g_object_set(w_, "title", s.title.c_str(), nullptr);
        connect(
            w_, "color-set",  // the user chose; not emitted by set_color
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
        const GdkRGBA rgba = to_rgba(c);
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
            +[](GtkToggleButton* b, gpointer p)
            {
                auto* self = static_cast<check_impl*>(p);
                if (self->updating_) return;
                // A click on the mixed state lands on checked (TortoiseGit's
                // tri-state All Branches toggles between on and off).
                if (gtk_toggle_button_get_inconsistent(b))
                {
                    gtk_toggle_button_set_inconsistent(b, false);
                    self->updating_ = true;
                    gtk_toggle_button_set_active(b, true);
                    self->updating_ = false;
                }
                if (self->toggled_) self->toggled_(gtk_toggle_button_get_active(b));
            },
            this);
    }

    bool checked() const override { return state() == check_state::checked; }
    void set_checked(bool v) override { set_state(v ? check_state::checked : check_state::unchecked); }

    check_state state() const override
    {
        auto* b = GTK_TOGGLE_BUTTON(w_);
        if (gtk_toggle_button_get_inconsistent(b)) return check_state::indeterminate;
        return gtk_toggle_button_get_active(b) ? check_state::checked : check_state::unchecked;
    }

    void set_state(check_state s) override
    {
        auto* b = GTK_TOGGLE_BUTTON(w_);
        updating_ = true;
        gtk_toggle_button_set_inconsistent(b, s == check_state::indeterminate);
        gtk_toggle_button_set_active(b, s == check_state::checked);
        updating_ = false;
    }

    void on_toggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }

private:
    std::function<void(bool)> toggled_;
    bool updating_ = false;
};

class menu_button_impl : public widget_base<rap::menu_button>
{
public:
    explicit menu_button_impl(const spec::menu_button& s)
        : widget_base(gtk_toggle_button_new())
    {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        box_ = box;
        label_ = gtk_label_new_with_mnemonic(s.text.c_str());
        gtk_label_set_mnemonic_widget(GTK_LABEL(label_), w_);
        gtk_container_add(GTK_CONTAINER(box), label_);
        gtk_container_add(GTK_CONTAINER(box),
                          gtk_image_new_from_icon_name("pan-down-symbolic", GTK_ICON_SIZE_BUTTON));
        gtk_container_add(GTK_CONTAINER(w_), box);
        pushed_ = s.pushed;
        sync();
        if (s.icon) set_icon(s.icon);

        menu_.on_hide([this] { sync(); });
        connect(
            w_, "toggled",
            +[](GtkToggleButton*, gpointer p) { static_cast<menu_button_impl*>(p)->clicked(); }, this);
    }

    void set_text(const std::string& t) override
    {
        gtk_label_set_text_with_mnemonic(GTK_LABEL(label_), t.c_str());
    }
    void set_icon(const icon& icon) override
    {
        if (icon_) gtk_widget_destroy(icon_);
        icon_ = icon_widget(icon, 16);
        if (!icon_) return;
        gtk_box_pack_start(GTK_BOX(box_), icon_, false, false, 0);
        gtk_box_reorder_child(GTK_BOX(box_), icon_, 0);
        gtk_widget_show(icon_);
    }
    void set_pushed(bool v) override
    {
        pushed_ = v;
        sync();
    }
    void on_menu(std::function<menu()> f) override { menu_fn_ = std::move(f); }

protected:
    void on_disconnect() override { menu_.destroy(); }

private:
    void sync()
    {
        updating_ = true;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w_), pushed_);
        updating_ = false;
    }

    void clicked()
    {
        if (updating_) return;
        menu items = menu_fn_ ? menu_fn_() : menu{};
        if (items.empty())
        {
            sync();
            return;
        }
        // Held down while the menu is open.
        updating_ = true;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w_), true);
        updating_ = false;
        GdkEvent* event = gtk_get_current_event();
        menu_.show(std::move(items), w_, event, true);
        if (event) gdk_event_free(event);
    }

    GtkWidget* label_;
    GtkWidget* box_ = nullptr;
    GtkWidget* icon_ = nullptr;
    popup_menu menu_;
    std::function<menu()> menu_fn_;
    bool pushed_ = false;
    bool updating_ = false;
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

// GTK 3 has no date picker: a button showing the date, with a calendar in
// a popover. on_changed fires when the popover closes on a new date.
// -- tree ---------------------------------------------------------------------

// A GtkTreeView over a GtkTreeStore: column 0 the label, column 1 the id.
class tree_impl : public widget_base<rap::tree>
{
public:
    explicit tree_impl(const spec::tree& s)
        : widget_base(gtk_tree_view_new(), gtk_scrolled_window_new(nullptr, nullptr))
    {
        store_ = gtk_tree_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
        auto* view = GTK_TREE_VIEW(w_);
        gtk_tree_view_set_model(view, GTK_TREE_MODEL(store_));
        gtk_container_add(GTK_CONTAINER(outer_), w_);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(outer_), GTK_SHADOW_IN);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outer_), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_tree_view_set_headers_visible(view, false);
        gtk_tree_view_set_enable_tree_lines(view, true);
        gtk_tree_view_append_column(
            view, gtk_tree_view_column_new_with_attributes("", gtk_cell_renderer_text_new(), "text", 0, nullptr));
        sel_ = gtk_tree_view_get_selection(view);
        gtk_tree_selection_set_mode(sel_, GTK_SELECTION_SINGLE);
        // Taking focus with no cursor, GTK puts it on the first row and
        // selects that (grab-focus, as a window shows, or focus-in): nobody
        // chose it, so it's refused.
        gtk_tree_selection_set_select_function(
            sel_,
            +[](GtkTreeSelection*, GtkTreeModel*, GtkTreePath*, gboolean selected, gpointer p) -> gboolean
            { return selected || !static_cast<tree_impl*>(p)->focusing_; },
            this, nullptr);
        connect(w_, "grab-focus", +[](GtkWidget*, gpointer p) { static_cast<tree_impl*>(p)->focusing_ = true; }, this);
        connect_after(w_, "grab-focus", +[](GtkWidget*, gpointer p) { static_cast<tree_impl*>(p)->focusing_ = false; },
                      this);
        connect(
            w_, "focus-in-event",
            +[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean
            {
                static_cast<tree_impl*>(p)->focusing_ = true;
                return false;
            },
            this);
        connect_after(
            w_, "focus-in-event",
            +[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean
            {
                static_cast<tree_impl*>(p)->focusing_ = false;
                return false;
            },
            this);
        connect(
            sel_, "changed",
            +[](GtkTreeSelection*, gpointer p)
            {
                auto* self = static_cast<tree_impl*>(p);
                if (!self->quiet_ && self->changed_) self->changed_();
            },
            this);
        connect(
            w_, "row-activated",
            +[](GtkTreeView*, GtkTreePath* path, GtkTreeViewColumn*, gpointer p)
            {
                auto* self = static_cast<tree_impl*>(p);
                if (self->activate_) self->activate_(self->id_at(path));
            },
            this);
        connect(
            w_, "button-press-event",
            +[](GtkWidget*, GdkEventButton* e, gpointer p) -> gboolean
            {
                auto* self = static_cast<tree_impl*>(p);
                if (e->type != GDK_BUTTON_PRESS || e->button != GDK_BUTTON_SECONDARY) return false;
                // The item under the pointer is selected first, as file managers do.
                GtkTreePath* path = nullptr;
                if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(self->w_), int(e->x), int(e->y), &path, nullptr,
                                                  nullptr, nullptr))
                {
                    gtk_tree_view_set_cursor(GTK_TREE_VIEW(self->w_), path, nullptr, false);  // reported
                    gtk_tree_path_free(path);
                }
                self->popup(reinterpret_cast<GdkEvent*>(e), path != nullptr);
                return true;
            },
            this);
        connect(
            w_, "popup-menu",
            +[](GtkWidget*, gpointer p) -> gboolean
            {
                static_cast<tree_impl*>(p)->popup(nullptr, true);
                return true;
            },
            this);
        set_items(s.items);
    }
    ~tree_impl() override { g_object_unref(store_); }

    void set_items(std::vector<tree_item> items) override
    {
        const std::optional<std::string> keep = selected();
        std::unordered_map<std::string, bool> open;
        for (auto& [id, ref] : by_id_)
            if (GtkTreePath* path = gtk_tree_row_reference_get_path(ref))
            {
                open[id] = gtk_tree_view_row_expanded(GTK_TREE_VIEW(w_), path);
                gtk_tree_path_free(path);
            }
        quiet q(*this);
        clear_refs();
        gtk_tree_store_clear(store_);
        std::vector<std::string> expand;
        insert(items, nullptr, open, expand);
        for (const std::string& id : expand)
            if (GtkTreePath* path = path_of(id))
            {
                gtk_tree_view_expand_row(GTK_TREE_VIEW(w_), path, false);
                gtk_tree_path_free(path);
            }
        if (keep) select_quietly(*keep);
    }
    std::optional<std::string> selected() const override
    {
        GtkTreeModel* model = nullptr;
        GtkTreeIter it;
        if (!gtk_tree_selection_get_selected(sel_, &model, &it)) return std::nullopt;
        return id_at(&it);
    }
    void select(const std::string& id) override
    {
        quiet q(*this);
        if (!select_quietly(id)) gtk_tree_selection_unselect_all(sel_);
    }
    void set_expanded(const std::string& id, bool open) override
    {
        if (GtkTreePath* path = path_of(id))
        {
            if (open)
                gtk_tree_view_expand_row(GTK_TREE_VIEW(w_), path, false);
            else
                gtk_tree_view_collapse_row(GTK_TREE_VIEW(w_), path);
            gtk_tree_path_free(path);
        }
    }
    void on_selection_changed(std::function<void()> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void(const std::string&)> f) override { activate_ = std::move(f); }
    void on_context_menu(std::function<menu(std::optional<std::string>)> f) override { context_ = std::move(f); }

protected:
    void on_disconnect() override
    {
        menu_.destroy();
        clear_refs();
    }

private:
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

    void insert(const std::vector<tree_item>& items, GtkTreeIter* parent,
                const std::unordered_map<std::string, bool>& open, std::vector<std::string>& expand)
    {
        for (const tree_item& t : items)
        {
            GtkTreeIter it;
            gtk_tree_store_insert_with_values(store_, &it, parent, -1, 0, t.label.c_str(), 1, t.id.c_str(), -1);
            GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &it);
            by_id_[t.id] = gtk_tree_row_reference_new(GTK_TREE_MODEL(store_), path);
            gtk_tree_path_free(path);
            insert(t.children, &it, open, expand);
            const auto was = open.find(t.id);
            if (!t.children.empty() && (was != open.end() ? was->second : t.expanded)) expand.push_back(t.id);
        }
    }
    void clear_refs()
    {
        for (auto& [id, ref] : by_id_) gtk_tree_row_reference_free(ref);
        by_id_.clear();
    }
    GtkTreePath* path_of(const std::string& id) const
    {
        const auto it = by_id_.find(id);
        return it == by_id_.end() ? nullptr : gtk_tree_row_reference_get_path(it->second);
    }
    std::string id_at(GtkTreeIter* it) const
    {
        gchar* id = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(store_), it, 1, &id, -1);
        std::string out = id ? id : "";
        g_free(id);
        return out;
    }
    std::string id_at(GtkTreePath* path) const
    {
        GtkTreeIter it;
        return gtk_tree_model_get_iter(GTK_TREE_MODEL(store_), &it, path) ? id_at(&it) : std::string();
    }
    bool select_quietly(const std::string& id)
    {
        GtkTreePath* path = path_of(id);
        if (!path) return false;
        // Its ancestors open, itself as it was.
        if (GtkTreePath* parent = gtk_tree_path_copy(path))
        {
            if (gtk_tree_path_up(parent) && gtk_tree_path_get_depth(parent) > 0)
                gtk_tree_view_expand_to_path(GTK_TREE_VIEW(w_), parent);
            gtk_tree_path_free(parent);
        }
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(w_), path, nullptr, false);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(w_), path, nullptr, false, 0, 0);
        gtk_tree_path_free(path);
        return true;
    }
    // A right-click (`event`) on an item, or the menu key on the selected one.
    void popup(GdkEvent* event, bool on_item)
    {
        if (!context_) return;
        const std::optional<std::string> id = on_item ? selected() : std::nullopt;
        menu m = context_(id);
        if (!m.empty()) menu_.show(std::move(m), w_, event);
    }

    GtkTreeStore* store_ = nullptr;
    GtkTreeSelection* sel_ = nullptr;
    std::unordered_map<std::string, GtkTreeRowReference*> by_id_;
    std::function<void()> changed_;
    std::function<void(const std::string&)> activate_;
    std::function<menu(std::optional<std::string>)> context_;
    popup_menu menu_;
    bool quiet_ = false;
    bool focusing_ = false;  // in focus-in: GTK's own cursor placement
};

class date_picker_impl : public widget_base<rap::date_picker>
{
public:
    explicit date_picker_impl(const spec::date_picker& s)
        : widget_base(gtk_button_new_with_label(""))
    {
        calendar_ = gtk_calendar_new();
        gtk_calendar_set_display_options(GTK_CALENDAR(calendar_),
                                         GtkCalendarDisplayOptions(GTK_CALENDAR_SHOW_HEADING |
                                                                   GTK_CALENDAR_SHOW_DAY_NAMES |
                                                                   GTK_CALENDAR_SHOW_WEEK_NUMBERS));
        popover_ = gtk_popover_new(w_);
        gtk_container_add(GTK_CONTAINER(popover_), calendar_);
        gtk_widget_show(calendar_);
        set_date(s.date);

        connect(
            w_, "clicked",
            +[](GtkButton*, gpointer p)
            {
                auto* self = static_cast<date_picker_impl*>(p);
                self->show_calendar();
            },
            this);
        connect(
            calendar_, "day-selected",
            +[](GtkCalendar* c, gpointer p)
            {
                auto* self = static_cast<date_picker_impl*>(p);
                if (self->updating_) return;
                guint y, m, d;
                gtk_calendar_get_date(c, &y, &m, &d);
                self->date_ = {int(y), int(m) + 1, int(d)};
                self->update_label();
            },
            this);
        connect(
            calendar_, "day-selected-double-click",
            +[](GtkCalendar*, gpointer p)
            { gtk_popover_popdown(GTK_POPOVER(static_cast<date_picker_impl*>(p)->popover_)); },
            this);
        connect(
            popover_, "closed",
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
        update_label();
    }
    void on_changed(std::function<void(rap::date)> f) override { changed_ = std::move(f); }

private:
    void show_calendar()
    {
        shown_ = date_;
        updating_ = true;
        auto* c = GTK_CALENDAR(calendar_);
        gtk_calendar_select_month(c, guint(date_.month - 1), guint(date_.year));
        gtk_calendar_select_day(c, guint(date_.day));
        updating_ = false;
        gtk_popover_popup(GTK_POPOVER(popover_));
    }

    void update_label()
    {
        GDate* d =
            g_date_new_dmy(GDateDay(date_.day), GDateMonth(date_.month), GDateYear(date_.year));
        char buf[64] = "";
        if (g_date_valid(d)) g_date_strftime(buf, sizeof buf, "%x", d);
        g_date_free(d);
        gtk_button_set_label(GTK_BUTTON(w_), buf);
    }

    GtkWidget* calendar_;
    GtkWidget* popover_;
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
        gtk_entry_set_text(GTK_ENTRY(w_), s.text.c_str());
        // Enter presses the dialog's default button (OK), as on Windows,
        // unless the app handles Enter itself (a find box).
        gtk_entry_set_activates_default(GTK_ENTRY(w_), true);
        if (!s.placeholder.empty()) set_placeholder(s.placeholder);
        connect(
            w_, "changed",
            +[](GtkEditable* e, gpointer p)
            {
                auto* self = static_cast<entry_impl*>(p);
                if (self->changed_) self->changed_(gtk_entry_get_text(GTK_ENTRY(e)));
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

    std::string text() const override { return gtk_entry_get_text(GTK_ENTRY(w_)); }
    void set_text(const std::string& t) override { gtk_entry_set_text(GTK_ENTRY(w_), t.c_str()); }
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
            gtk_css_provider_load_from_data(
                css,
                "entry.rapport-frameless { background-image: none; background-color: transparent; "
                "border-color: transparent; box-shadow: none; }",
                -1, nullptr);
            gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(w_),
                                                      GTK_STYLE_PROVIDER(css),
                                                      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
            installed = true;
        }
        GtkStyleContext* style = gtk_widget_get_style_context(w_);
        if (v)
            gtk_style_context_remove_class(style, "rapport-frameless");
        else
            gtk_style_context_add_class(style, "rapport-frameless");
    }
    void set_placeholder(const std::string& t) override
    {
        gtk_entry_set_placeholder_text(GTK_ENTRY(w_), t.c_str());
    }
    void on_changed(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void()> f) override
    {
        activate_ = std::move(f);
        gtk_entry_set_activates_default(GTK_ENTRY(w_), !activate_);
    }

private:
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

class drop_down_impl : public widget_base<rap::drop_down>
{
public:
    explicit drop_down_impl(const spec::drop_down& s)
        : widget_base(gtk_combo_box_text_new())
    {
        set_items(s.items);
        set_current(s.current);
        connect(
            w_, "changed",
            +[](GtkComboBox* c, gpointer p)
            {
                auto* self = static_cast<drop_down_impl*>(p);
                if (self->changed_) self->changed_(gtk_combo_box_get_active(c));
            },
            this);
    }

    void set_items(std::vector<std::string> items) override
    {
        auto* combo = GTK_COMBO_BOX_TEXT(w_);
        gtk_combo_box_text_remove_all(combo);
        for (auto& i : items) gtk_combo_box_text_append_text(combo, i.c_str());
    }
    int current() const override { return gtk_combo_box_get_active(GTK_COMBO_BOX(w_)); }
    void set_current(int i) override { gtk_combo_box_set_active(GTK_COMBO_BOX(w_), i); }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }

private:
    std::function<void(int)> changed_;
};

class combo_impl : public widget_base<rap::combo>
{
public:
    explicit combo_impl(const spec::combo& s)
        : widget_base(gtk_combo_box_text_new_with_entry())
    {
        entry_ = gtk_bin_get_child(GTK_BIN(w_));
        set_items(s.items);
        set_text(s.text);
        connect(
            entry_, "changed",
            +[](GtkEditable* e, gpointer p)
            {
                auto* self = static_cast<combo_impl*>(p);
                if (self->changed_) self->changed_(gtk_entry_get_text(GTK_ENTRY(e)));
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
    std::string text() const override { return gtk_entry_get_text(GTK_ENTRY(entry_)); }
    void set_text(const std::string& t) override
    {
        gtk_entry_set_text(GTK_ENTRY(entry_), t.c_str());
    }
    void on_changed(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void()> f) override { activate_ = std::move(f); }

private:
    GtkWidget* entry_;
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

class text_view_impl : public widget_base<rap::text_view>
{
public:
    explicit text_view_impl(const spec::text_view& s)
        : widget_base(gtk_text_view_new(), gtk_scrolled_window_new(nullptr, nullptr))
    {
        gtk_container_add(GTK_CONTAINER(outer_), w_);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(outer_), GTK_SHADOW_IN);
        auto* tv = GTK_TEXT_VIEW(w_);
        gtk_text_view_set_monospace(tv, s.monospace);
        gtk_text_view_set_wrap_mode(tv, s.wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
        gtk_text_view_set_left_margin(tv, 4);
        gtk_text_view_set_right_margin(tv, 4);
        if (s.compact)
        {
            std::string rule =
                "textview { font-size: " + std::to_string(compact_font(w_).px) + "px; }";
            GtkCssProvider* css = gtk_css_provider_new();
            gtk_css_provider_load_from_data(css, rule.c_str(), -1, nullptr);
            gtk_style_context_add_provider(gtk_widget_get_style_context(w_),
                                           GTK_STYLE_PROVIDER(css),
                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
        }
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
    // press and release on a divider with the position changed in between.
    void on_moved(std::function<void()> f) override
    {
        const bool first = !moved_;
        moved_ = std::move(f);
        if (!first) return;
        for (GtkWidget* w = w_; w;)
        {
            connect(
                w, "button-press-event",
                +[](GtkWidget*, GdkEventButton*, gpointer p) -> gboolean
                {
                    auto* self = static_cast<splitter_impl*>(p);
                    self->pressed_ = self->sizes();
                    return false;
                },
                this);
            connect(
                w, "button-release-event",
                +[](GtkWidget*, GdkEventButton*, gpointer p) -> gboolean
                {
                    auto* self = static_cast<splitter_impl*>(p);
                    if (!self->pressed_.empty() && self->sizes() != self->pressed_ && self->moved_)
                        self->moved_();
                    self->pressed_.clear();
                    return false;
                },
                this);
            GtkWidget* next = gtk_paned_get_child2(GTK_PANED(w));
            w = next && g_object_get_data(G_OBJECT(next), "rapport-split-chain") ? next : nullptr;
        }
    }

    float divider() const override
    {
        int handle = 0;
        gtk_widget_style_get(w_, "handle-size", &handle, nullptr);
        return float(handle);
    }
    std::vector<float> sizes() const override
    {
        bool vertical =
            gtk_orientable_get_orientation(GTK_ORIENTABLE(w_)) == GTK_ORIENTATION_VERTICAL;
        auto length = [&](GtkWidget* w)
        {
            return float(vertical ? gtk_widget_get_allocated_height(w)
                                  : gtk_widget_get_allocated_width(w));
        };
        std::vector<float> out;
        GtkWidget* w = w_;
        for (;;)
        {
            auto* p = GTK_PANED(w);
            out.push_back(float(gtk_paned_get_position(p)));
            GtkWidget* next = gtk_paned_get_child2(p);
            if (!next || !g_object_get_data(G_OBJECT(next), "rapport-split-chain"))
            {
                out.push_back(next ? length(next) : 0);
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
            GtkWidget* next = gtk_paned_get_child2(p);
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
        gtk_box_pack_start(GTK_BOX(pages_[size_t(i)]), w, TRUE, TRUE, 0);
        gtk_widget_show_all(w);
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
        gtk_box_pack_start(GTK_BOX(pages_[size_t(i)]), w, TRUE, TRUE, 0);
        gtk_widget_show_all(w);
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
        : widget_base(gtk_tree_view_new(), gtk_scrolled_window_new(nullptr, nullptr))
    {
        rows_ = RAPPORT_TREE_ROWS(g_object_new(rapport_tree_rows_get_type(), nullptr));
        auto* view = GTK_TREE_VIEW(w_);
        gtk_container_add(GTK_CONTAINER(outer_), w_);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(outer_), GTK_SHADOW_IN);
        gtk_tree_view_set_headers_visible(view, s.headers);
        gtk_tree_view_set_fixed_height_mode(view, true);
        // No rubber-band (drag-rectangle) selection: report-style lists on
        // Windows don't have it; Shift/Ctrl-click select ranges and sets.
        gtk_tree_view_set_rubber_banding(view, false);
        compact_ = s.compact;
        checks_ = s.checkboxes;
        if ((auto_scroll_ = s.auto_scroll))
        {
            auto_lists().insert(this);
            connect_after(w_, "draw", +[](GtkWidget* w, cairo_t* cr, gpointer p) -> gboolean
                         { static_cast<list_impl*>(p)->draw_auto_scroll(w, cr); return false; }, this);
        }
        if (s.soft_selection)
        {
            soft_ = soft_selection_colors(w_);
            auto rgb = [](color c)
            {
                return "rgb(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," +
                       std::to_string(c.b) + ")";
            };
            std::string rule = "treeview.view:selected, treeview.view:selected:focus, "
                               "treeview.view:selected:backdrop { background-color: " +
                               rgb(soft_->first) + "; color: " + rgb(soft_->second) + "; }";
            GtkCssProvider* css = gtk_css_provider_new();
            gtk_css_provider_load_from_data(css, rule.c_str(), -1, nullptr);
            gtk_style_context_add_provider(gtk_widget_get_style_context(w_),
                                           GTK_STYLE_PROVIDER(css),
                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
        }
        if (compact_)
        {
            // Font size is inherited by the header and the cells, and by the
            // Pango layouts painted columns draw with.
            compact_font_size font = compact_font(w_);
            compact_scale_ = font.scale();
            std::string rule = "treeview { font-size: " + std::to_string(font.px) +
                               "px; -GtkTreeView-vertical-separator: 0; }";
            GtkCssProvider* css = gtk_css_provider_new();
            gtk_css_provider_load_from_data(css, rule.c_str(), -1, nullptr);
            gtk_style_context_add_provider(gtk_widget_get_style_context(w_),
                                           GTK_STYLE_PROVIDER(css),
                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
        }

        sel_ = gtk_tree_view_get_selection(view);
        gtk_tree_selection_set_mode(sel_, s.selection == selection_mode::none ? GTK_SELECTION_NONE
                                          : s.selection == selection_mode::single
                                              ? GTK_SELECTION_SINGLE
                                              : GTK_SELECTION_MULTIPLE);

        for (size_t i = 0; i < s.columns.size(); ++i) add_column(s.columns[i], i);
        auto first_text = std::ranges::find_if(s.columns, [](auto& c) { return !c.painted; });
        if (first_text != s.columns.end())
            rows_->search_column = guint(first_text - s.columns.begin());
        else
            gtk_tree_view_set_enable_search(view, false);
        gtk_tree_view_set_search_column(view, 0);

        attach();

        connect(
            sel_, "changed",
            +[](GtkTreeSelection*, gpointer p)
            {
                auto* self = static_cast<list_impl*>(p);
                if (self->sel_changed_) self->sel_changed_();
            },
            this);
        connect(
            w_, "row-activated",
            +[](GtkTreeView*, GtkTreePath* path, GtkTreeViewColumn*, gpointer p)
            {
                auto* self = static_cast<list_impl*>(p);
                if (self->activate_) self->activate_(size_t(gtk_tree_path_get_indices(path)[0]));
            },
            this);
        connect(
            w_, "button-press-event", +[](GtkWidget*, GdkEventButton* e, gpointer p) -> gboolean
            { return static_cast<list_impl*>(p)->button_pressed(e); }, this);
        connect(
            w_, "motion-notify-event", +[](GtkWidget*, GdkEventMotion* e, gpointer p) -> gboolean
            { return static_cast<list_impl*>(p)->drag_motion(e); }, this);
        connect(
            w_, "leave-notify-event", +[](GtkWidget*, GdkEventCrossing*, gpointer p) -> gboolean
            { static_cast<list_impl*>(p)->set_hot(std::nullopt); return false; }, this);
        connect(
            w_, "button-release-event", +[](GtkWidget*, GdkEventButton* e, gpointer p) -> gboolean
            { return static_cast<list_impl*>(p)->drag_released(e); }, this);
        // Space toggles the cursor row's box (GTK would toggle its selection).
        connect(
            w_, "key-press-event",
            +[](GtkWidget*, GdkEventKey* e, gpointer p) -> gboolean
            {
                auto* self = static_cast<list_impl*>(p);
                if (!self->checks_ || !self->model_ || e->keyval != GDK_KEY_space ||
                    (e->state & gtk_accelerator_get_default_mod_mask()))
                    return false;
                if (auto row = self->cursor_row()) self->model_->toggle_checked(*row);
                return true;
            },
            this);
        connect(
            w_, "popup-menu",
            +[](GtkWidget*, gpointer p) -> gboolean
            {
                auto* self = static_cast<list_impl*>(p);
                return self->popup(self->cursor_row(), nullptr);
            },
            this);
        connect(
            w_, "size-allocate",
            +[](GtkWidget*, GdkRectangle* a, gpointer p)
            { static_cast<list_impl*>(p)->fit_stretch(a->width); },
            this);
    }

    // Stretch columns share the width the others leave, once, when the list
    // first has a size; then they become fixed like the rest, so widening
    // one column never narrows another (the list scrolls sideways instead).
    void fit_stretch(int width)
    {
        if (fitted_ || width <= 1) return;
        fitted_ = true;
        for (auto* col : cols_)
            if (gtk_tree_view_column_get_expand(col))
            {
                int w = gtk_tree_view_column_get_width(col);
                if (w > 0) gtk_tree_view_column_set_fixed_width(col, w);
                gtk_tree_view_column_set_expand(col, false);
            }
    }

    ~list_impl() override
    {
        auto_lists().erase(this);
        if (model_) model_->remove_observer(this);
        g_object_unref(rows_);
    }

    void set_model(list_model* m) override
    {
        if (model_) model_->remove_observer(this);
        model_ = m;
        rows_->model = m;
        if (model_) model_->add_observer(this);
        model_reset();
    }

    std::vector<size_t> selection() const override
    {
        std::vector<size_t> out;
        GList* paths = gtk_tree_selection_get_selected_rows(sel_, nullptr);
        for (GList* l = paths; l; l = l->next)
            out.push_back(size_t(gtk_tree_path_get_indices(static_cast<GtkTreePath*>(l->data))[0]));
        g_list_free_full(paths, GDestroyNotify(gtk_tree_path_free));
        std::ranges::sort(out);
        return out;
    }

    std::optional<size_t> current() const override { return cursor_row(); }

    void select(const std::vector<size_t>& rows) override
    {
        gtk_tree_selection_unselect_all(sel_);
        for (size_t r : rows)
        {
            if (r >= rows_->count) continue;
            GtkTreePath* path = gtk_tree_path_new_from_indices(int(r), -1);
            gtk_tree_selection_select_path(sel_, path);
            gtk_tree_path_free(path);
        }
    }

    void set_current(size_t row) override
    {
        if (row >= rows_->count) return;
        GtkTreePath* path = gtk_tree_path_new_from_indices(int(row), -1);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(w_), path, nullptr, false);
        gtk_tree_path_free(path);
    }

    void scroll_to(size_t row) override
    {
        if (row >= rows_->count) return;
        GtkTreePath* path = gtk_tree_path_new_from_indices(int(row), -1);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(w_), path, nullptr, false, 0, 0);
        gtk_tree_path_free(path);
    }

    void set_sort_indicator(std::optional<size_t> column, sort_order order) override
    {
        for (size_t i = 0; i < cols_.size(); ++i)
        {
            bool on = column && *column == i;
            gtk_tree_view_column_set_sort_indicator(cols_[i], on);
            if (on)
                gtk_tree_view_column_set_sort_order(cols_[i], order == sort_order::ascending
                                                                  ? GTK_SORT_ASCENDING
                                                                  : GTK_SORT_DESCENDING);
        }
    }

    std::vector<float> column_widths() const override
    {
        std::vector<float> out;
        for (auto* c : cols_) out.push_back(float(gtk_tree_view_column_get_width(c)));
        return out;
    }

    std::vector<size_t> column_order() const override
    {
        std::vector<size_t> out;
        GList* shown = gtk_tree_view_get_columns(GTK_TREE_VIEW(w_));
        for (GList* l = shown; l; l = l->next)
            if (auto it = std::ranges::find(cols_, GTK_TREE_VIEW_COLUMN(l->data));
                it != cols_.end())
                out.push_back(size_t(it - cols_.begin()));
        g_list_free(shown);
        return out;
    }
    void set_column_order(const std::vector<size_t>& order) override
    {
        if (!is_column_order(order, cols_.size())) return;
        GtkTreeViewColumn* prev = nullptr;
        for (size_t i : order)
        {
            gtk_tree_view_move_column_after(GTK_TREE_VIEW(w_), cols_[i], prev);
            prev = cols_[i];
        }
    }

    void set_column_widths(const std::vector<float>& widths) override
    {
        for (size_t i = 0; i < widths.size() && i < cols_.size(); ++i)
            if (widths[i] > 0)
            {
                gtk_tree_view_column_set_fixed_width(cols_[i], int(widths[i]));
                gtk_tree_view_column_set_expand(cols_[i], false);  // restored widths win
            }
        fitted_ = true;
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
    void on_header_menu(std::function<menu(size_t)> f) override { header_menu_ = std::move(f); }
    void on_cell_context_menu(std::function<menu(size_t, size_t, point_f)> f) override { cell_menu_ = std::move(f); }
    void on_reorder(reorder_fn f) override { reorder_ = std::move(f); }
    void set_reorderable(bool v) override { reorderable_ = v; }
    void on_cell_drag(cell_drag d) override
    {
        if (!cell_drag_.begin)  // the pointer's label, drawn over the rows
            connect_after(w_, "draw", +[](GtkWidget* w, cairo_t* cr, gpointer p) -> gboolean
                          { static_cast<list_impl*>(p)->draw_cell_drag(w, cr); return false; }, this);
        cell_drag_ = std::move(d);
    }

    bool column_visible(size_t column) const override
    {
        return column < cols_.size() && gtk_tree_view_column_get_visible(cols_[column]);
    }
    void set_column_visible(size_t column, bool v) override
    {
        if (column < cols_.size()) gtk_tree_view_column_set_visible(cols_[column], v);
    }

    // list_observer. Big changes detach the model: re-attaching is O(visible)
    // while per-row signals are O(rows).
    void model_reset() override
    {
        detach();
        rows_->count = model_ ? guint(model_->row_count()) : 0;
        attach();
    }

    void rows_changed(size_t first, size_t n) override
    {
        if (n > bulk)
        {
            gtk_widget_queue_draw(w_);
            return;
        }
        for (size_t r = first; r < first + n && r < rows_->count; ++r)
            row_signal(r, change::changed);
    }

    void rows_inserted(size_t first, size_t n) override
    {
        if (n > bulk) return model_reset();
        rows_->count += guint(n);
        for (size_t r = first; r < first + n; ++r) row_signal(r, change::inserted);
    }

    void rows_removed(size_t first, size_t n) override
    {
        if (n > bulk) return model_reset();
        for (size_t i = 0; i < n; ++i)
        {
            rows_->count -= 1;
            row_signal(first, change::deleted);
        }
    }

protected:
    void on_disconnect() override
    {
        alive_ = false;
        if (active == this) active = nullptr;
        menu_.destroy();
    }

private:
    static constexpr size_t bulk = 256;

    struct column_ctx
    {
        list_impl* list;
        size_t column;
    };

    enum class change
    {
        changed,
        inserted,
        deleted
    };

    void row_signal(size_t row, change c)
    {
        GtkTreePath* path = gtk_tree_path_new_from_indices(int(row), -1);
        GtkTreeIter iter{};
        iter.stamp = rows_->stamp;
        iter.user_data = GUINT_TO_POINTER(guint(row));
        auto* m = GTK_TREE_MODEL(rows_);
        switch (c)
        {
        case change::changed: gtk_tree_model_row_changed(m, path, &iter); break;
        case change::inserted: gtk_tree_model_row_inserted(m, path, &iter); break;
        case change::deleted: gtk_tree_model_row_deleted(m, path); break;
        }
        gtk_tree_path_free(path);
    }

    void detach()
    {
        gtk_tree_view_set_model(GTK_TREE_VIEW(w_), nullptr);
        rows_->stamp = g_random_int();
    }
    void attach() { gtk_tree_view_set_model(GTK_TREE_VIEW(w_), GTK_TREE_MODEL(rows_)); }

    void add_column(const column& c, size_t index)
    {
        auto& ctx = ctxs_.emplace_back(new column_ctx{this, index});
        GtkTreeViewColumn* col = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(col, c.title.c_str());
        gtk_tree_view_column_set_resizable(col, true);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);  // fixed-height mode
        gtk_tree_view_column_set_expand(col, c.stretch);
        int width = px(c.width);
        if (width < 0)
            width = int((c.painted ? 8 : std::max<size_t>(c.title.size() + 4, 10)) *
                        g_metrics.char_width);
        gtk_tree_view_column_set_fixed_width(col, width);
        gtk_tree_view_column_set_alignment(col, c.align == align::center ? 0.5f
                                                : c.align == align::end  ? 1.0f
                                                                         : 0.0f);

        GtkCellRenderer* r;
        if (c.painted)
        {
            r = GTK_CELL_RENDERER(g_object_new(rapport_paint_renderer_get_type(), nullptr));
            auto* pr = RAPPORT_PAINT_RENDERER(r);
            pr->ctx = ctx.get();
            pr->draw = &list_impl::paint_cell;
            pr->height = int(g_metrics.line_height * (compact_ ? compact_scale_ : 1.f) + 0.5f);
            gtk_tree_view_column_pack_start(col, r, true);
            gtk_tree_view_column_set_cell_data_func(
                col, r,
                +[](GtkTreeViewColumn*, GtkCellRenderer* r, GtkTreeModel*, GtkTreeIter* iter,
                    gpointer) { RAPPORT_PAINT_RENDERER(r)->row = row_of(iter); },
                nullptr, nullptr);
        }
        else
        {
            // The row's checkbox: GTK's own toggle cell, first in column 0.
            if (checks_ && index == 0)
            {
                GtkCellRenderer* box = gtk_cell_renderer_toggle_new();
                g_object_set(box, "activatable", TRUE, nullptr);
                gtk_tree_view_column_pack_start(col, box, false);
                gtk_tree_view_column_set_cell_data_func(
                    col, box,
                    +[](GtkTreeViewColumn*, GtkCellRenderer* r, GtkTreeModel*, GtkTreeIter* iter,
                        gpointer p)
                    {
                        auto* self = static_cast<column_ctx*>(p)->list;
                        const size_t row = row_of(iter);
                        const check_state st = self->model_ && row < self->model_->row_count()
                                                   ? self->model_->checked(row)
                                                   : check_state::unchecked;
                        g_object_set(r, "active", st == check_state::checked, "inconsistent",
                                     st == check_state::indeterminate, nullptr);
                    },
                    ctx.get(), nullptr);
                connect(
                    box, "toggled",
                    +[](GtkCellRendererToggle*, gchar* path, gpointer p)
                    {
                        auto* self = static_cast<list_impl*>(p);
                        if (self->model_) self->model_->toggle_checked(size_t(std::atoi(path)));
                    },
                    this);
            }
            // An icon before the text, when the model gives one (a file's
            // type); hidden otherwise.
            GtkCellRenderer* icon = gtk_cell_renderer_pixbuf_new();
            gtk_tree_view_column_pack_start(col, icon, false);
            gtk_tree_view_column_set_cell_data_func(col, icon, &list_impl::icon_cell, ctx.get(), nullptr);

            r = gtk_cell_renderer_text_new();
            g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xalign",
                         c.align == align::center ? 0.5f
                         : c.align == align::end  ? 1.0f
                                                  : 0.0f,
                         nullptr);
            gtk_tree_view_column_pack_start(col, r, true);
            gtk_tree_view_column_set_cell_data_func(col, r, &list_impl::text_cell, ctx.get(), nullptr);
        }

        if (c.sortable)
        {
            gtk_tree_view_column_set_clickable(col, true);
            connect(
                col, "clicked",
                +[](GtkTreeViewColumn* col, gpointer p)
                {
                    auto* ctx = static_cast<column_ctx*>(p);
                    ctx->list->header_clicked(col, ctx->column);
                },
                ctx.get());
        }
        gtk_tree_view_column_set_visible(col, c.visible);
        gtk_tree_view_column_set_reorderable(col, true);  // column_order()
        gtk_tree_view_append_column(GTK_TREE_VIEW(w_), col);
        cols_.push_back(col);

        // The header button exists once the column is in the view.
        if (GtkWidget* header = gtk_tree_view_column_get_button(col))
            connect(
                header, "button-press-event",
                +[](GtkWidget* header, GdkEventButton* e, gpointer p) -> gboolean
                {
                    auto* ctx = static_cast<column_ctx*>(p);
                    if (e->type != GDK_BUTTON_PRESS || e->button != GDK_BUTTON_SECONDARY)
                        return false;
                    auto* list = ctx->list;
                    if (!list->alive_ || !list->header_menu_) return false;
                    menu items = list->header_menu_(ctx->column);
                    if (items.empty()) return false;
                    list->menu_.show(std::move(items), header, reinterpret_cast<GdkEvent*>(e));
                    return true;
                },
                ctx.get());
    }

    static void icon_cell(GtkTreeViewColumn*, GtkCellRenderer* r, GtkTreeModel*, GtkTreeIter* iter,
                         gpointer p)
    {
        auto* ctx = static_cast<column_ctx*>(p);
        list_model* model = ctx->list->model_;
        GIcon* gicon = nullptr;
        if (model && ctx->list->alive_ && row_of(iter) < model->row_count())
        {
            const icon icon = model->icon(row_of(iter), ctx->column);
            if (!icon.file.empty()) gicon = file_type_icon(icon.file);
            else if (!icon.name.empty()) gicon = named_g_icon(icon.name);
        }
        g_object_set(r, "gicon", gicon, "visible", gicon != nullptr, nullptr);
    }

    static void text_cell(GtkTreeViewColumn*, GtkCellRenderer* r, GtkTreeModel*, GtkTreeIter* iter,
                         gpointer p)
    {
        auto* ctx = static_cast<column_ctx*>(p);
        list_model* model = ctx->list->model_;
        if (!model || !ctx->list->alive_) return;
        size_t row = row_of(iter);
        text_style style = model->style(row, ctx->column);
        // A style background colours the whole cell, not just behind the
        // glyphs; selected rows keep the selection colour.
        std::optional<paint> bg;
        std::swap(bg, style.bg);
        const bool selected = gtk_tree_selection_iter_is_selected(ctx->list->sel_, iter);
        std::optional<color> hover = !selected && ctx->list->hot_ == row ? ctx->list->hover_color() : std::nullopt;
        if ((bg && !selected) || hover)
        {
            color c = bg && !selected ? resolve(ctx->list->w_, *bg) : *hover;
            GdkRGBA rgba{c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0};
            g_object_set(r, "cell-background-rgba", &rgba, nullptr);
        }
        else
            g_object_set(r, "cell-background-set", false, nullptr);
        PangoAttrList* attrs = pango_attrs(ctx->list->w_, style);
        g_object_set(r, "text", model->text(row, ctx->column).c_str(), "attributes", attrs,
                     nullptr);
        if (attrs) pango_attr_list_unref(attrs);
    }

    static void paint_cell(gpointer p, guint row, cairo_t* cr, GtkWidget* widget,
                          const GdkRectangle* area, GtkCellRendererState flags)
    {
        auto* ctx = static_cast<column_ctx*>(p);
        list_model* model = ctx->list->model_;
        if (!model || !ctx->list->alive_ || row >= model->row_count()) return;

        bool selected = flags & GTK_CELL_RENDERER_SELECTED;
        cairo_save(cr);
        cairo_rectangle(cr, area->x, area->y, area->width, area->height);
        cairo_clip(cr);
        cairo_translate(cr, area->x, area->y);
        std::optional<color> hover = !selected && ctx->list->hot_ == row ? ctx->list->hover_color() : std::nullopt;
        if (auto bg = model->style(row, ctx->column).bg; (bg && !selected) || hover)
        {
            color c = bg && !selected ? resolve(widget, *bg) : *hover;
            cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0);
            cairo_paint(cr);
        }
        auto& soft = ctx->list->soft_;
        cairo_painter painter(cr, widget,
                             selected && !soft ? std::optional<paint>{theme_color::highlight_text}
                                               : std::nullopt);
        if (soft)
        {
            painter.override_color(theme_color::highlight, soft->first);
            painter.override_color(theme_color::highlight_text, soft->second);
        }
        cell_info info{{float(area->width), float(area->height)},
                      selected,
                      bool(flags & GTK_CELL_RENDERER_FOCUSED)};
        model->paint(row, ctx->column, painter, info);
        cairo_restore(cr);
    }

    void header_clicked(GtkTreeViewColumn* col, size_t index)
    {
        if (!sort_) return;
        bool asc = !(gtk_tree_view_column_get_sort_indicator(col) &&
                     gtk_tree_view_column_get_sort_order(col) == GTK_SORT_ASCENDING);
        sort_(index, asc ? sort_order::ascending : sort_order::descending);
    }

    std::optional<size_t> cursor_row() const
    {
        GtkTreePath* path = nullptr;
        gtk_tree_view_get_cursor(GTK_TREE_VIEW(w_), &path, nullptr);
        if (!path) return std::nullopt;
        size_t row = size_t(gtk_tree_path_get_indices(path)[0]);
        gtk_tree_path_free(path);
        return row;
    }

    // Drag-reordering is done by hand rather than with GTK's DnD (which
    // needs a GtkTreeDragSource model): press on a row, move past a small
    // threshold, and the native drop line follows the pointer until release.
    struct drag
    {
        bool pressed = false;
        bool dragging = false;
        bool defer_select = false;  // pressed inside a multi-selection
        double x0 = 0, y0 = 0;
        size_t row = 0;
    };
    // A cell_drag in progress: from a press `begin` accepted.
    struct cell_drag_state
    {
        bool pressed = false, dragging = false;
        double x0 = 0, y0 = 0;  // press, bin window coordinates
        double x = 0, y = 0;    // pointer now
        size_t from = 0;
        cell_drag_start start;
        std::optional<size_t> target;
    };

    bool on_bin_window(GdkEvent* e) const
    {
        return e->any.window == gtk_tree_view_get_bin_window(GTK_TREE_VIEW(w_));
    }

    // The row the dragged rows would go in front of, and where to draw the
    // native drop line for it.
    // Works in bin-window coordinates, clamped to the visible rows, so a
    // pointer over the header (above the rows) targets the first visible row
    // rather than counting as "past the end".
    size_t drop_target(double bx, double by)
    {
        auto* view = GTK_TREE_VIEW(w_);
        size_t count = rows_->count, dest = count;
        int height = gdk_window_get_height(gtk_tree_view_get_bin_window(view));
        int x = std::max(0, int(bx)), y = std::clamp(int(by), 0, std::max(0, height - 1));
        GtkTreePath* path = nullptr;
        if (gtk_tree_view_get_path_at_pos(view, x, y, &path, nullptr, nullptr, nullptr))
        {
            size_t row = size_t(gtk_tree_path_get_indices(path)[0]);
            GdkRectangle area;
            gtk_tree_view_get_background_area(view, path, nullptr, &area);
            bool before = by < area.y + area.height / 2.0;
            dest = before ? row : row + 1;
            gtk_tree_path_free(path);
        }
        // else: empty space below the last row, so the end.

        // Dropping a contiguous block onto its own place changes nothing, so
        // that shows no drop line and a "not allowed" cursor.
        bool noop = is_noop_drop(dest);
        set_drag_cursor(noop ? "not-allowed" : "grabbing");
        if (count && !noop)
        {
            bool at_end = dest >= count;
            GtkTreePath* line = gtk_tree_path_new_from_indices(int(at_end ? count - 1 : dest), -1);
            gtk_tree_view_set_drag_dest_row(
                view, line, at_end ? GTK_TREE_VIEW_DROP_AFTER : GTK_TREE_VIEW_DROP_BEFORE);
            gtk_tree_path_free(line);
        }
        else
            gtk_tree_view_set_drag_dest_row(view, nullptr, GTK_TREE_VIEW_DROP_BEFORE);
        return dest;
    }

public:
    // Esc during a drag cancels it, ahead of any window key handling (which
    // might close the dialog). Returns whether a drag was cancelled.
    static bool cancel_active_drag()
    {
        for (list_impl* l : auto_lists())
            if (l->auto_.on)
            {
                l->stop_auto_scroll();
                return true;
            }
        list_impl* list = active;
        if (!list) return false;
        list->drag_ = {};
        list->cell_ = {};
        gtk_widget_queue_draw(list->w_);
        active = nullptr;
        gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(list->w_), nullptr,
                                        GTK_TREE_VIEW_DROP_BEFORE);
        list->set_drag_cursor(nullptr);
        return true;
    }

private:
    static inline list_impl* active = nullptr;

    bool is_noop_drop(size_t dest) const
    {
        auto sel = selection();
        if (sel.empty()) return true;
        bool contiguous = sel.back() - sel.front() + 1 == sel.size();
        return contiguous && dest >= sel.front() && dest <= sel.back() + 1;
    }

    void set_drag_cursor(const char* name)
    {
        GdkWindow* bin = gtk_tree_view_get_bin_window(GTK_TREE_VIEW(w_));
        GdkCursor* c = name ? gdk_cursor_new_from_name(gdk_window_get_display(bin), name) : nullptr;
        gdk_window_set_cursor(bin, c);
        if (c) g_object_unref(c);
    }

    gboolean drag_pressed(GdkEventButton* e)
    {
        drag_ = {};
        if (!reorder_ || !reorderable_ || e->type != GDK_BUTTON_PRESS ||
            e->button != GDK_BUTTON_PRIMARY || !on_bin_window(reinterpret_cast<GdkEvent*>(e)) ||
            (e->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)))
            return false;
        GtkTreePath* path = nullptr;
        if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w_), int(e->x), int(e->y), &path, nullptr,
                                           nullptr, nullptr))
            return false;
        bool selected = gtk_tree_selection_path_is_selected(sel_, path);
        drag_ = {true, false, selected && gtk_tree_selection_count_selected_rows(sel_) > 1,
                 e->x, e->y,  size_t(gtk_tree_path_get_indices(path)[0])};
        gtk_tree_path_free(path);
        // Keep a multi-selection so it can be dragged; a plain click inside
        // it selects just that row on release instead.
        if (drag_.defer_select)
        {
            gtk_widget_grab_focus(w_);
            return true;
        }
        return false;
    }

    // Soft-selection lists tint the row under the pointer (as on win32,
    // and Explorer's and TortoiseGit's lists): just the two rows involved
    // repaint as it moves.
    std::optional<color> hover_color() const
    {
        if (!soft_) return std::nullopt;
        color base = theme_color_of(w_, theme_color::base), accent = theme_color_of(w_, theme_color::accent);
        bool dark = base.r * 30 + base.g * 59 + base.b * 11 < 12800;
        return mix_colors(base, accent, dark ? 0.16f : 0.09f);
    }
    void set_hot(std::optional<size_t> row)
    {
        if (row == hot_) return;
        for (std::optional<size_t> r : {hot_, row})
        {
            if (!r) continue;
            GtkTreePath* path = gtk_tree_path_new_from_indices(int(*r), -1);
            GdkRectangle a;
            gtk_tree_view_get_background_area(GTK_TREE_VIEW(w_), path, nullptr, &a);
            gtk_tree_path_free(path);
            int x, y;
            gtk_tree_view_convert_bin_window_to_widget_coords(GTK_TREE_VIEW(w_), 0, a.y, &x, &y);
            gtk_widget_queue_draw_area(w_, 0, y, gtk_widget_get_allocated_width(w_), a.height);
        }
        hot_ = row;
    }
    void track_hover(GdkEventMotion* e)
    {
        if (!soft_ || drag_.dragging || cell_.dragging) return set_hot(std::nullopt);
        GtkTreePath* path = nullptr;
        std::optional<size_t> row;
        if (on_bin_window(reinterpret_cast<GdkEvent*>(e)) &&
            gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w_), int(e->x), int(e->y), &path, nullptr, nullptr, nullptr))
        {
            row = size_t(gtk_tree_path_get_indices(path)[0]);
            gtk_tree_path_free(path);
        }
        set_hot(row);
    }

    gboolean drag_motion(GdkEventMotion* e)
    {
        if (auto_.on)
        {
            auto_.y = e->y;
            return true;
        }
        track_hover(e);
        if (cell_.pressed) return cell_motion(e);
        if (!drag_.pressed || !(e->state & GDK_BUTTON1_MASK)) return false;
        if (!drag_.dragging)
        {
            int threshold = 8;
            g_object_get(gtk_widget_get_settings(w_), "gtk-dnd-drag-threshold", &threshold,
                         nullptr);
            if (std::abs(e->x - drag_.x0) < threshold && std::abs(e->y - drag_.y0) < threshold)
                return true;
            drag_.dragging = true;
            active = this;
        }
        drop_target(e->x, e->y);

        // Scroll when the pointer nears the top or bottom edge.
        int wx, wy;
        gtk_tree_view_convert_bin_window_to_widget_coords(GTK_TREE_VIEW(w_), int(e->x), int(e->y),
                                                          &wx, &wy);
        int height = gtk_widget_get_allocated_height(w_), edge = 24;
        if (wy < edge || wy > height - edge)
            if (GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(w_)))
                gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) +
                                                  (wy < edge ? -1 : 1) * g_metrics.line_height);
        return true;
    }

    gboolean drag_released(GdkEventButton* e)
    {
        if (auto_.on && e->button == GDK_BUTTON_MIDDLE)
        {
            stop_auto_scroll();
            return true;
        }
        if (cell_.pressed) return cell_released(e);
        if (!drag_.pressed || e->button != GDK_BUTTON_PRIMARY) return false;
        drag d = drag_;
        drag_ = {};
        if (active == this) active = nullptr;
        if (d.dragging)
        {
            size_t dest = drop_target(e->x, e->y);
            gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w_), nullptr, GTK_TREE_VIEW_DROP_BEFORE);
            set_drag_cursor(nullptr);
            if (reorder_ && !is_noop_drop(dest)) reorder_(selection(), dest);
            return true;
        }
        if (d.defer_select)
        {
            select({d.row});
            set_current(d.row);
            return true;
        }
        return false;
    }

    // A press on something the app lets you drag out of a cell. Selection
    // still happens as for any click; the drag starts past the threshold.
    bool cell_pressed(GdkEventButton* e)
    {
        cell_ = {};
        if (!cell_drag_.begin || e->type != GDK_BUTTON_PRESS || e->button != GDK_BUTTON_PRIMARY ||
            !on_bin_window(reinterpret_cast<GdkEvent*>(e)) || (e->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)))
            return false;
        auto* view = GTK_TREE_VIEW(w_);
        GtkTreePath* path = nullptr;
        GtkTreeViewColumn* col = nullptr;
        if (!gtk_tree_view_get_path_at_pos(view, int(e->x), int(e->y), &path, &col, nullptr, nullptr)) return false;
        GdkRectangle area;
        gtk_tree_view_get_cell_area(view, path, col, &area);
        const size_t row = size_t(gtk_tree_path_get_indices(path)[0]);
        gtk_tree_path_free(path);
        auto c = std::find(cols_.begin(), cols_.end(), col);
        if (c == cols_.end()) return false;
        auto start = cell_drag_.begin(row, size_t(c - cols_.begin()), {float(e->x - area.x), float(e->y - area.y)});
        if (!start) return false;
        cell_ = {true, false, e->x, e->y, e->x, e->y, row, std::move(*start), std::nullopt};
        return true;
    }

    gboolean cell_motion(GdkEventMotion* e)
    {
        if (!(e->state & GDK_BUTTON1_MASK)) return false;
        cell_.x = e->x;
        cell_.y = e->y;
        if (!cell_.dragging)
        {
            int threshold = 8;
            g_object_get(gtk_widget_get_settings(w_), "gtk-dnd-drag-threshold", &threshold, nullptr);
            if (std::abs(e->x - cell_.x0) < threshold && std::abs(e->y - cell_.y0) < threshold) return true;
            cell_.dragging = true;
            active = this;
        }
        auto* view = GTK_TREE_VIEW(w_);
        cell_.target.reset();
        GtkTreePath* path = nullptr;
        if (gtk_tree_view_get_path_at_pos(view, int(e->x), int(e->y), &path, nullptr, nullptr, nullptr))
        {
            const size_t row = size_t(gtk_tree_path_get_indices(path)[0]);
            if (cell_drag_.can_drop && cell_drag_.can_drop(cell_.from, row, cell_.start.data)) cell_.target = row;
            gtk_tree_view_set_drag_dest_row(view, cell_.target ? path : nullptr, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
            gtk_tree_path_free(path);
        }
        else
            gtk_tree_view_set_drag_dest_row(view, nullptr, GTK_TREE_VIEW_DROP_BEFORE);
        set_drag_cursor(cell_.target ? "grabbing" : "not-allowed");
        gtk_widget_queue_draw(w_);
        return true;
    }

    gboolean cell_released(GdkEventButton* e)
    {
        if (e->button != GDK_BUTTON_PRIMARY) return false;
        cell_drag_state d = std::move(cell_);
        cell_ = {};
        if (!d.dragging) return false;  // a plain click
        if (active == this) active = nullptr;
        gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w_), nullptr, GTK_TREE_VIEW_DROP_BEFORE);
        set_drag_cursor(nullptr);
        gtk_widget_queue_draw(w_);
        if (d.target && cell_drag_.drop) cell_drag_.drop(d.from, *d.target, d.start.data);
        return true;
    }

    void draw_cell_drag(GtkWidget* w, cairo_t* cr)
    {
        if (!cell_.dragging || cell_.start.label.empty()) return;
        int wx, wy;
        gtk_tree_view_convert_bin_window_to_widget_coords(GTK_TREE_VIEW(w), int(cell_.x), int(cell_.y), &wx, &wy);
        cairo_painter p(cr, w);
        text_style ts;
        ts.bold = true;
        const size_f t = p.measure(cell_.start.label, ts);
        const rect_f box{float(wx) + 12, float(wy) + 4, t.w + 12, t.h + 4};
        p.rect(box, 4, paint{theme_color::highlight}, paint{theme_color::mid});
        ts.fg = theme_color::highlight_text;
        p.text({box.x + 6, box.y + 2}, cell_.start.label, ts);
    }

    // Autoscroll (spec.auto_scroll): the middle button held, the list
    // scrolls each frame by the pointer's offset from the anchor.
    void start_auto_scroll(GdkEventButton* e)
    {
        auto_ = auto_scroll_state{true, e->y, e->y, e->x, 0, 0};
        auto_.tick = gtk_widget_add_tick_callback(
            w_,
            +[](GtkWidget*, GdkFrameClock* clock, gpointer p) -> gboolean
            {
                auto* self = static_cast<list_impl*>(p);
                auto_scroll_state& a = self->auto_;
                const gint64 now = gdk_frame_clock_get_frame_time(clock);
                const double dt = a.last ? (now - a.last) / 1e6 : 0;
                a.last = now;
                if (GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(self->w_)))
                {
                    const double v = gtk_adjustment_get_value(adj) + auto_scroll_speed(float(a.y - a.y0)) * dt;
                    gtk_adjustment_set_value(adj, std::clamp(v, gtk_adjustment_get_lower(adj),
                                                             gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj)));
                }
                return G_SOURCE_CONTINUE;
            },
            this, nullptr);
        set_drag_cursor("all-scroll");
        gtk_widget_queue_draw(w_);
    }
    void stop_auto_scroll()
    {
        if (!auto_.on) return;
        gtk_widget_remove_tick_callback(w_, auto_.tick);
        auto_ = {};
        set_drag_cursor(nullptr);
        gtk_widget_queue_draw(w_);
    }

    gboolean button_pressed(GdkEventButton* e)
    {
        if (auto_scroll_ && e->button == GDK_BUTTON_MIDDLE && e->type == GDK_BUTTON_PRESS &&
            on_bin_window(reinterpret_cast<GdkEvent*>(e)))
        {
            start_auto_scroll(e);
            return true;
        }
        if (cell_pressed(e)) return false;  // selects as usual; dragging begins on motion
        if (drag_pressed(e)) return true;
        if (e->type != GDK_BUTTON_PRESS || e->button != GDK_BUTTON_SECONDARY) return false;
        std::optional<size_t> row;
        GtkTreePath* path = nullptr;
        GtkTreeViewColumn* col = nullptr;
        std::optional<std::pair<size_t, point_f>> at;  // column, point in the cell
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w_), int(e->x), int(e->y), &path, &col,
                                          nullptr, nullptr))
        {
            row = size_t(gtk_tree_path_get_indices(path)[0]);
            if (auto c = std::find(cols_.begin(), cols_.end(), col); c != cols_.end())
            {
                GdkRectangle area;
                gtk_tree_view_get_cell_area(GTK_TREE_VIEW(w_), path, col, &area);
                at = {size_t(c - cols_.begin()), point_f{float(e->x - area.x), float(e->y - area.y)}};
            }
            // Keep a multi-selection when right-clicking inside it (two
            // revisions to compare). set_cursor would select only its row,
            // so the cursor moves only with a new selection.
            if (!gtk_tree_selection_path_is_selected(sel_, path))
            {
                gtk_tree_selection_unselect_all(sel_);
                gtk_tree_view_set_cursor(GTK_TREE_VIEW(w_), path, nullptr, false);
            }
            gtk_tree_path_free(path);
        }
        if (cell_menu_ && row && at && alive_)
        {
            menu items = cell_menu_(*row, at->first, at->second);
            if (!items.empty()) menu_.show(std::move(items), w_, reinterpret_cast<GdkEvent*>(e));
            return true;
        }
        popup(row, reinterpret_cast<GdkEvent*>(e));
        return true;
    }

    gboolean popup(std::optional<size_t> row, GdkEvent* event)
    {
        if (!alive_ || !context_menu_) return false;
        menu items = context_menu_(row);
        if (items.empty()) return false;

        menu_.show(std::move(items), w_, event);
        return true;
    }

    RapportTreeRows* rows_ = nullptr;
    GtkTreeSelection* sel_ = nullptr;
    popup_menu menu_;
    list_model* model_ = nullptr;
    std::vector<GtkTreeViewColumn*> cols_;
    std::vector<std::unique_ptr<column_ctx>> ctxs_;
    bool alive_ = true;
    bool fitted_ = false;  // stretch columns have had their share of the width
    bool compact_ = false;
    bool checks_ = false;                          // a toggle cell at the start of column 0
    std::optional<std::pair<color, color>> soft_;  // selection background, text
    float compact_scale_ = 1;

    std::function<void()> sel_changed_;
    std::function<void(size_t)> activate_;
    std::function<void(size_t, sort_order)> sort_;
    std::function<menu(std::optional<size_t>)> context_menu_;
    std::function<menu(size_t)> header_menu_;
    std::function<menu(size_t, size_t, point_f)> cell_menu_;
    reorder_fn reorder_;
    bool reorderable_ = true;
    drag drag_;
    cell_drag cell_drag_;
    cell_drag_state cell_;
    struct auto_scroll_state
    {
        bool on = false;
        double y0 = 0, y = 0, x0 = 0;  // anchor and pointer, bin window coordinates
        gint64 last = 0;               // previous frame's time
        guint tick = 0;
    };
    bool auto_scroll_ = false;
    auto_scroll_state auto_;
    static std::set<list_impl*>& auto_lists()  // lists that autoscroll, for Esc
    {
        static std::set<list_impl*> lists;
        return lists;
    }
    void draw_auto_scroll(GtkWidget* w, cairo_t* cr)
    {
        if (!auto_.on) return;
        int wx, wy;
        gtk_tree_view_convert_bin_window_to_widget_coords(GTK_TREE_VIEW(w), int(auto_.x0), int(auto_.y0), &wx, &wy);
        cairo_painter p(cr, w);
        paint_auto_scroll_anchor(p, float(wx), float(wy));
    }
    std::optional<size_t> hot_;  // the row under the pointer
};

// -- window -------------------------------------------------------------------

// -- canvas -------------------------------------------------------------------

unsigned modifiers(guint state)
{
    unsigned m = 0;
    if (state & GDK_SHIFT_MASK) m |= mod_shift;
    if (state & GDK_CONTROL_MASK) m |= mod_ctrl;
    if (state & GDK_MOD1_MASK) m |= mod_alt;
    if (state & (GDK_SUPER_MASK | GDK_META_MASK)) m |= mod_meta;
    return m;
}

const char* cursor_name(cursor c)
{
    switch (c)
    {
    case cursor::default_: return "default";
    case cursor::pointer: return "pointer";
    case cursor::text: return "text";
    case cursor::crosshair: return "crosshair";
    case cursor::grab: return "grab";
    case cursor::grabbing: return "grabbing";
    case cursor::zoom_in: return "zoom-in";
    case cursor::zoom_out: return "zoom-out";
    case cursor::move: return "move";
    case cursor::wait: return "wait";
    case cursor::hidden: return "none";
    case cursor::resize_ew: return "ew-resize";
    case cursor::resize_ns: return "ns-resize";
    case cursor::resize_nwse: return "nwse-resize";
    case cursor::resize_nesw: return "nesw-resize";
    }
    return "default";
}

#ifdef RAPPORT_GTK3_FRACTIONAL
// The compositor's fractional scale for a toplevel (Wayland's
// fractional-scale protocol), which GTK 3 doesn't read: one per GdkWindow,
// shared by its canvases, kept in fractional_scale_of()'s key. GTK 3 makes
// a toplevel's wl_surface when it maps and destroys it when it unmaps, so
// the scale object lives from map to unmap, destroyed before its surface.
namespace fractional
{

constexpr char k_object_key[] = "rapport-fractional-object";
constexpr char k_hooked_key[] = "rapport-fractional-hooked";

// Bound once per display. The registry roundtrip runs on a queue of our
// own, so it dispatches none of GDK's events; the manager then moves to
// GDK's default queue, where its objects' events arrive on the main thread.
wp_fractional_scale_manager_v1* manager_for(wl_display* display)
{
    static wl_display* bound_for = nullptr;
    static wp_fractional_scale_manager_v1* manager = nullptr;
    if (bound_for == display) return manager;
    bound_for = display;
    manager = nullptr;
    wl_event_queue* queue = wl_display_create_queue(display);
    auto* wrapped = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapped), queue);
    wl_registry* registry = wl_display_get_registry(wrapped);
    static const wl_registry_listener listener{
        .global =
            +[](void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t)
            {
                if (std::strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0)
                    *static_cast<wp_fractional_scale_manager_v1**>(data) =
                        static_cast<wp_fractional_scale_manager_v1*>(wl_registry_bind(
                            r, name, &wp_fractional_scale_manager_v1_interface, 1));
            },
        .global_remove = +[](void*, wl_registry*, uint32_t) {},
    };
    wl_registry_add_listener(registry, &listener, &manager);
    wl_display_roundtrip_queue(display, queue);
    // The roundtrip has dispatched everything queued for it: nothing is left
    // on the private queue to strand when the manager leaves it.
    wl_display_dispatch_queue_pending(display, queue);
    if (manager) wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(manager), nullptr);
    wl_registry_destroy(registry);
    wl_proxy_wrapper_destroy(wrapped);
    wl_event_queue_destroy(queue);
    return manager;
}

struct tracker
{
    wp_fractional_scale_v1* object = nullptr;
    GdkWindow* window = nullptr;
    GtkWidget* top = nullptr;
};

void start(GtkWidget* top)
{
    GdkWindow* win = gtk_widget_get_window(top);
    if (!win || g_object_get_data(G_OBJECT(win), k_object_key)) return;
    GdkDisplay* display = gdk_window_get_display(win);
    if (!GDK_IS_WAYLAND_DISPLAY(display)) return;  // X11, XWayland: whole scales
    wp_fractional_scale_manager_v1* manager =
        manager_for(gdk_wayland_display_get_wl_display(display));
    wl_surface* surface = gdk_wayland_window_get_wl_surface(win);
    if (!manager || !surface) return;
    auto* t = new tracker{wp_fractional_scale_manager_v1_get_fractional_scale(manager, surface),
                          win, top};
    static const wp_fractional_scale_v1_listener listener{
        .preferred_scale =
            +[](void* data, wp_fractional_scale_v1*, uint32_t scale)
            {
                auto* t = static_cast<tracker*>(data);
                g_object_set_data(G_OBJECT(t->window), k_fractional_scale_key,
                                  GUINT_TO_POINTER(scale));
                // The canvases notice the new scale as they draw (on_resize).
                gtk_widget_queue_draw(t->top);
            },
    };
    wp_fractional_scale_v1_add_listener(t->object, &listener, t);
    g_object_set_data_full(
        G_OBJECT(win), k_object_key, t,
        +[](gpointer p)
        {
            auto* t = static_cast<tracker*>(p);
            wp_fractional_scale_v1_destroy(t->object);
            delete t;
        });
}

void stop(GtkWidget* top)
{
    if (GdkWindow* win = gtk_widget_get_window(top))
    {
        g_object_set_data(G_OBJECT(win), k_object_key, nullptr);  // destroys it
        g_object_set_data(G_OBJECT(win), k_fractional_scale_key, nullptr);
    }
}

// From a canvas's realize: its toplevel's scale is watched while mapped.
void watch(GtkWidget* canvas)
{
    GtkWidget* top = gtk_widget_get_toplevel(canvas);
    if (!top || !gtk_widget_is_toplevel(top)) return;
    if (!g_object_get_data(G_OBJECT(top), k_hooked_key))
    {
        g_object_set_data(G_OBJECT(top), k_hooked_key, GINT_TO_POINTER(1));
        g_signal_connect(top, "map", G_CALLBACK(+[](GtkWidget* w, gpointer) { start(w); }),
                         nullptr);
        g_signal_connect(top, "unmap", G_CALLBACK(+[](GtkWidget* w, gpointer) { stop(w); }),
                         nullptr);
    }
    if (gtk_widget_get_mapped(top)) start(top);
}

}  // namespace fractional
#endif

class canvas_impl : public widget_base<rap::canvas>
{
public:
    explicit canvas_impl(const spec::canvas& s)
        : widget_base(gtk_drawing_area_new())
    {
        gtk_widget_add_events(
            w_, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                    GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_ENTER_NOTIFY_MASK |
                    GDK_LEAVE_NOTIFY_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
        gtk_widget_set_can_focus(w_, s.focusable);
        focusable_ = s.focusable;
        connect(
            w_, "draw",
            +[](GtkWidget* w, cairo_t* cr, gpointer p) -> gboolean
            {
                auto* self = static_cast<canvas_impl*>(p);
                // A new fractional scale is a resize in device pixels
                // (canvas::on_resize), told before the frame that uses it;
                // a whole one also comes as notify::scale-factor.
                const float scale = device_scale_of(w);
                if (self->scale_ > 0 && scale != self->scale_)
                {
                    self->scale_ = scale;
                    if (self->resize_) self->resize_(self->size());
                    self->target_changed();
                }
                if (!self->paint_ && !self->frame_) return false;
                cairo_painter painter(cr, w);
                self->frame_.paint(painter, self->size());
                if (self->paint_) self->paint_(painter, self->size());
                return true;
            },
            this);
        connect(
            w_, "size-allocate",
            +[](GtkWidget*, GdkRectangle* a, gpointer p)
            {
                auto* self = static_cast<canvas_impl*>(p);
                self->allocated_ = true;
                self->scale_ = device_scale_of(self->w_);
                if (self->resize_) self->resize_({float(a->width), float(a->height)});
                self->target_changed();
            },
            this);
        connect(
            w_, "realize",
            +[](GtkWidget* w, gpointer p)
            {
                static_cast<canvas_impl*>(p)->apply_cursor();
#ifdef RAPPORT_GTK3_FRACTIONAL
                fractional::watch(w);
#else
                (void)w;
#endif
            },
            this);
        connect(
            w_, "notify::scale-factor",
            +[](GObject*, GParamSpec*, gpointer p)
            {
                auto* self = static_cast<canvas_impl*>(p);
                if (self->resize_) self->resize_(self->size());
                self->target_changed();
            },
            this);
        connect(w_, "button-press-event", &canvas_impl::button, this);
        connect(w_, "button-release-event", &canvas_impl::button, this);
        connect(
            w_, "motion-notify-event",
            +[](GtkWidget*, GdkEventMotion* e, gpointer p) -> gboolean
            {
                return static_cast<canvas_impl*>(p)->pointer({pointer_event_type::move,
                                                          {float(e->x), float(e->y)},
                                                          0,
                                                          1,
                                                          modifiers(e->state)});
            },
            this);
        connect(w_, "enter-notify-event", &canvas_impl::crossing, this);
        connect(w_, "leave-notify-event", &canvas_impl::crossing, this);
        connect(
            w_, "scroll-event",
            +[](GtkWidget*, GdkEventScroll* e, gpointer p) -> gboolean
            {
                auto* self = static_cast<canvas_impl*>(p);
                // With the smooth mask, GTK also emulates direction events
                // from smooth ones; take each motion once.
                if (!self->scroll_ || gdk_event_get_pointer_emulated((GdkEvent*)e)) return false;
                scroll_event ev{{float(e->x), float(e->y)}, 0, 0, false, modifiers(e->state)};
                switch (e->direction)
                {
                case GDK_SCROLL_UP: ev.dy = -1; break;
                case GDK_SCROLL_DOWN: ev.dy = 1; break;
                case GDK_SCROLL_LEFT: ev.dx = -1; break;
                case GDK_SCROLL_RIGHT: ev.dx = 1; break;
                case GDK_SCROLL_SMOOTH:
                {
                    ev.dx = float(e->delta_x);
                    ev.dy = float(e->delta_y);
                    GdkDevice* d = gdk_event_get_source_device((GdkEvent*)e);
                    GdkInputSource src = d ? gdk_device_get_source(d) : GDK_SOURCE_MOUSE;
                    ev.precise = src == GDK_SOURCE_TOUCHPAD || src == GDK_SOURCE_TOUCHSCREEN;
                    break;
                }
                }
                return self->scroll_(ev);
            },
            this);
        connect(w_, "key-press-event", &canvas_impl::make_key_event, this);
        connect(w_, "key-release-event", &canvas_impl::make_key_event, this);
    }

    ~canvas_impl() override { on_disconnect(); }

    void invalidate() override { gtk_widget_queue_draw(w_); }
    // Nothing until it has been allocated, as on GTK 4: before that GTK 3
    // reports 1x1, which an app sizing its window round the canvas would
    // take for real.
    size_f size() const override
    {
        if (!allocated_) return {};
        return {float(gtk_widget_get_allocated_width(w_)),
                float(gtk_widget_get_allocated_height(w_))};
    }
    float device_scale() const override { return device_scale_of(w_); }
    void set_cursor(cursor c) override
    {
        cursor_ = c;
        apply_cursor();
    }

    void on_paint(std::function<void(rap::painter&, rap::size_f)> f) override { paint_ = std::move(f); }
    void on_resize(std::function<void(size_f)> f) override { resize_ = std::move(f); }
    void on_pointer(std::function<bool(const pointer_event&)> f) override { pointer_ = std::move(f); }
    void on_scroll(std::function<bool(const scroll_event&)> f) override { scroll_ = std::move(f); }
    void on_key(std::function<bool(const key_event&)> f) override { key_ = std::move(f); }

    rap::frame_target target() const override { return frames::pixels_target(size(), float(gtk_widget_get_scale_factor(w_))); }
    void on_frame_target(std::function<void(const rap::frame_target&)> f) override { frame_target_ = std::move(f); }
    void present(std::shared_ptr<const rap::image> pixels, frame_color color) override
    {
        frame_ = {std::move(pixels), color};
        gtk_widget_queue_draw(w_);
    }
    void clear_frame() override
    {
        frame_ = {};
        gtk_widget_queue_draw(w_);
    }
    subscription on_frame(std::function<void(double)> f) override
    {
        if (!alive_) return {};
        auto* fn = new std::function<void(double)>(std::move(f));
        guint id = gtk_widget_add_tick_callback(
            w_,
            +[](GtkWidget*, GdkFrameClock* clock, gpointer p) -> gboolean
            {
                (*static_cast<std::function<void(double)>*>(p))(
                    double(gdk_frame_clock_get_frame_time(clock)) / 1e6);
                return G_SOURCE_CONTINUE;
            },
            fn, +[](gpointer p) { delete static_cast<std::function<void(double)>*>(p); });
        ticks_.push_back(id);
        return subscription(
            [this, alive = alive_, id]
            {
                if (!*alive) return;  // the widget went first; GTK dropped its ticks
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
    float scale_ = 0;  // the device scale the app last heard of
    bool allocated_ = false;
    bool pointer(const pointer_event& e) { return pointer_ && pointer_(e); }

    static gboolean button(GtkWidget*, GdkEventButton* e, gpointer p)
    {
        auto* self = static_cast<canvas_impl*>(p);
        // GTK adds 2BUTTON/3BUTTON events after the presses; count clicks
        // ourselves instead so each press arrives once.
        if (e->type != GDK_BUTTON_PRESS && e->type != GDK_BUTTON_RELEASE) return true;
        pointer_event ev{e->type == GDK_BUTTON_PRESS ? pointer_event_type::press
                                                    : pointer_event_type::release,
                        {float(e->x), float(e->y)},
                        int(e->button),
                        1,
                        modifiers(e->state)};
        if (ev.type == pointer_event_type::press)
        {
            if (self->focusable_ && !gtk_widget_has_focus(self->w_))
                gtk_widget_grab_focus(self->w_);
            int time = 400, distance = 5;
            g_object_get(gtk_widget_get_settings(self->w_), "gtk-double-click-time", &time,
                         "gtk-double-click-distance", &distance, nullptr);
            const bool again = ev.button == self->last_button_ &&
                               e->time - self->last_time_ <= guint32(time) &&
                               std::abs(ev.pos.x - self->last_pos_.x) <= distance &&
                               std::abs(ev.pos.y - self->last_pos_.y) <= distance;
            self->clicks_ = again ? self->clicks_ + 1 : 1;
            ev.clicks = self->clicks_;
            self->last_button_ = ev.button;
            self->last_time_ = e->time;
            self->last_pos_ = ev.pos;
        }
        return self->pointer(ev);
    }

    static gboolean crossing(GtkWidget*, GdkEventCrossing* e, gpointer p)
    {
        auto* self = static_cast<canvas_impl*>(p);
        return self->pointer(
            {e->type == GDK_ENTER_NOTIFY ? pointer_event_type::enter : pointer_event_type::leave,
             {float(e->x), float(e->y)},
             0,
             1,
             modifiers(e->state)});
    }

    static gboolean make_key_event(GtkWidget*, GdkEventKey* e, gpointer p)
    {
        auto* self = static_cast<canvas_impl*>(p);
        if (!self->key_) return false;
        key_event ev;
        ev.press = e->type == GDK_KEY_PRESS;
        ev.key = key_for(e->keyval);
        if (ev.key == key::character)
            ev.codepoint = gdk_keyval_to_unicode(gdk_keyval_to_lower(e->keyval));
        ev.modifiers = modifiers(e->state);
        if (ev.press)
        {
            ev.repeat = std::ranges::find(self->held_, e->hardware_keycode) != self->held_.end();
            if (!ev.repeat) self->held_.push_back(e->hardware_keycode);
        }
        else
        {
            std::erase(self->held_, e->hardware_keycode);
        }
        return self->key_(ev);
    }

    void apply_cursor()
    {
        GdkWindow* win = gtk_widget_get_window(w_);
        if (!win) return;  // applied on realize
        GdkCursor* c =
            cursor_ == cursor::default_
                ? nullptr
                : gdk_cursor_new_from_name(gtk_widget_get_display(w_), cursor_name(cursor_));
        gdk_window_set_cursor(win, c);
        if (c) g_object_unref(c);
    }

    std::function<void(rap::painter&, rap::size_f)> paint_;
    std::function<void(size_f)> resize_;
    frames::frame_layer frame_;
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
    std::vector<guint16> held_;
    cursor cursor_ = cursor::default_;
    bool focusable_ = true;
    int clicks_ = 0, last_button_ = 0;
    guint32 last_time_ = 0;
    point_f last_pos_;
};

class window_impl : public rap::window, public part
{
    // Development aid: RAPPORT_SNAPSHOT=file.png saves the window shown last,
    // RAPPORT_SNAPSHOT_DELAY ms (default 500) after it appears, then quits
    // the main loop. For checking layouts and canvases without a screen
    // grabber.
    // The most recently shown window is the one a snapshot captures (a
    // dialog over the main window, say).
    static GtkWidget*& last_shown()
    {
        static GtkWidget* w = nullptr;
        return w;
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
                if (!last_shown()) return G_SOURCE_REMOVE;
                save_last_shown(std::getenv("RAPPORT_SNAPSHOT"));
                gtk_main_quit();
                return G_SOURCE_REMOVE;
            },
            nullptr);
    }

public:
    // The window shown last, as drawn now, to a PNG.
    static bool save_last_shown(const std::string& path)
    {
        GtkWidget* win = last_shown();
        if (!win) return false;
        const int scale = gtk_widget_get_scale_factor(win);
        const int w = gtk_widget_get_allocated_width(win);
        const int h = gtk_widget_get_allocated_height(win);
        cairo_surface_t* surface =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * scale, h * scale);
        cairo_surface_set_device_scale(surface, scale, scale);
        cairo_t* cr = cairo_create(surface);
        gtk_widget_draw(win, cr);
        cairo_destroy(cr);
        const bool ok = cairo_surface_write_to_png(surface, path.c_str()) == CAIRO_STATUS_SUCCESS;
        cairo_surface_destroy(surface);
        return ok;
    }

    window_impl(const window_spec& spec, window_impl* parent)
        : win_(gtk_window_new(GTK_WINDOW_TOPLEVEL))
        , dialog_(spec.dialog)
    {
        auto* win = GTK_WINDOW(win_);
        gtk_window_set_title(win, spec.title.c_str());
        // Snapshot runs are automated: never take the user's keyboard focus.
        if (const char* snap = std::getenv("RAPPORT_SNAPSHOT"); snap && *snap)
        {
            gtk_window_set_accept_focus(win, false);
            gtk_window_set_focus_on_map(win, false);
        }
        gtk_window_set_default_size(win, px(spec.width), px(spec.height));
        if (parent) gtk_window_set_transient_for(win, GTK_WINDOW(parent->win_));
        gtk_window_set_resizable(win, spec.resizable);
        // First in line, so Esc ends a list drag rather than the dialog.
        connect(
            win_, "key-press-event", +[](GtkWidget*, GdkEventKey* e, gpointer) -> gboolean
            { return e->keyval == GDK_KEY_Escape && list_impl::cancel_active_drag(); }, this);
        if (dialog_)
        {
            gtk_window_set_modal(win, parent != nullptr && spec.modal);
            gtk_window_set_type_hint(win, GDK_WINDOW_TYPE_HINT_DIALOG);
            connect(
                win_, "key-press-event",
                +[](GtkWidget*, GdkEventKey* e, gpointer p) -> gboolean
                {
                    if (e->keyval != GDK_KEY_Escape) return false;
                    static_cast<window_impl*>(p)->close_requested();
                    return true;
                },
                this);
        }
        connect(
            win_, "delete-event",
            +[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean
            {
                static_cast<window_impl*>(p)->close_requested();
                return true;  // we own the window; never let GTK destroy it
            },
            this);
        connect(
            win_, "window-state-event",
            +[](GtkWidget*, GdkEventWindowState* e, gpointer p) -> gboolean
            {
                auto* self = static_cast<window_impl*>(p);
                self->maximized_ = e->new_window_state & (GDK_WINDOW_STATE_MAXIMIZED |
                                                          GDK_WINDOW_STATE_TILED);
                const bool fs = e->new_window_state & GDK_WINDOW_STATE_FULLSCREEN;
                if (fs != self->fullscreen_)
                {
                    self->fullscreen_ = fs;
                    self->show_menu_bar();
                    if (self->on_fullscreen_) self->on_fullscreen_(fs);
                }
                return false;
            },
            this);
        // Runs before GTK hands the key to the focused widget (key-press-event
        // is RUN_LAST, so this handler precedes the window's default one).
        auto key = +[](GtkWidget* w, GdkEventKey* e, gpointer p) -> gboolean
        {
            auto* self = static_cast<window_impl*>(p);
            if (e->keyval == GDK_KEY_F10 && e->type == GDK_KEY_PRESS)
                self->rebuild_menu_bar();  // opens it
            if (!self->on_key_) return false;
            GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(w));
            const bool typing =
                focus &&
                (GTK_IS_EDITABLE(focus) ||
                 (GTK_IS_TEXT_VIEW(focus) && gtk_text_view_get_editable(GTK_TEXT_VIEW(focus))));
            const bool not_text = e->keyval == GDK_KEY_Escape ||
                                 (e->keyval >= GDK_KEY_F1 && e->keyval <= GDK_KEY_F12);
            if (typing && !not_text) return false;
            key_event ev;
            ev.press = e->type == GDK_KEY_PRESS;
            ev.key = key_for(e->keyval);
            if (ev.key == key::character)
                ev.codepoint = gdk_keyval_to_unicode(gdk_keyval_to_lower(e->keyval));
            ev.modifiers = modifiers(e->state);
            return self->on_key_(ev);
        };
        connect(win_, "key-press-event", key, this);
        connect(win_, "key-release-event", key, this);
    }

    ~window_impl() override
    {
        if (last_shown() == win_) last_shown() = nullptr;
        for (auto& part : parts) part->disconnect();
        disconnect();
        gtk_widget_destroy(win_);
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
    void close() override { gtk_widget_hide(win_); }
    void on_finished(std::function<void(dialog_role)> f) override { on_finished_ = std::move(f); }
    void on_close_request(std::function<bool()> f) override { on_close_request_ = std::move(f); }
    void on_files_dropped(std::function<void(const std::vector<std::string>&)> f) override
    {
        const bool first = !on_files_dropped_;
        on_files_dropped_ = std::move(f);
        if (!first || !on_files_dropped_) return;
        // A drop on any widget without a target of its own lands here.
        static GtkTargetEntry uris{const_cast<gchar*>("text/uri-list"), 0, 0};
        gtk_drag_dest_set(win_, GTK_DEST_DEFAULT_ALL, &uris, 1, GDK_ACTION_COPY);
        connect(
            win_, "drag-data-received",
            +[](GtkWidget*, GdkDragContext*, gint, gint, GtkSelectionData* data, guint, guint,
                gpointer p)
            {
                auto* self = static_cast<window_impl*>(p);
                std::vector<std::string> paths;
                if (gchar** list = gtk_selection_data_get_uris(data))
                {
                    for (gchar** u = list; *u; ++u)
                        if (gchar* path = g_filename_from_uri(*u, nullptr, nullptr))
                        {
                            paths.emplace_back(path);
                            g_free(path);
                        }
                    g_strfreev(list);
                }
                if (!paths.empty() && self->on_files_dropped_) self->on_files_dropped_(paths);
            },
            this);
    }

    size_f size() const override
    {
        int w = 0, h = 0;
        gtk_window_get_size(GTK_WINDOW(win_), &w, &h);
        return {float(w), float(h)};
    }
    void resize(size_f s) override
    {
        int w = std::max(1, int(s.w)), h = std::max(1, int(s.h));
        int x = 0, y = 0;
        const bool place = frame_on_screen(w, h, x, y);
        gtk_window_resize(GTK_WINDOW(win_), w, h);
        if (place) gtk_window_move(GTK_WINDOW(win_), x, y);
    }
    // Keeps the whole frame (title bar, shadows) in the work area: the size
    // shrinks to fit, and the position slides back inside, as on win32.
    // Only where windows can be placed (X11), once shown.
    bool frame_on_screen(int& w, int& h, int& x, int& y) const
    {
        GdkWindow* gw = gtk_widget_get_window(win_);
        if (!placeable() || !gw || !gtk_widget_get_visible(win_)) return false;
        GdkRectangle frame;
        gdk_window_get_frame_extents(gw, &frame);
        int cw = 0, ch = 0, px = 0, py = 0;
        gtk_window_get_size(GTK_WINDOW(win_), &cw, &ch);
        gtk_window_get_position(GTK_WINDOW(win_), &px, &py);
        const int extra_w = std::max(0, frame.width - cw), extra_h = std::max(0, frame.height - ch);
        const rect_f wa = work_area();
        if (wa.w <= 0 || wa.h <= 0) return false;
        w = std::max(1, std::min(w, int(wa.w) - extra_w));
        h = std::max(1, std::min(h, int(wa.h) - extra_h));
        int fx = frame.x, fy = frame.y;
        fx = std::max(int(wa.x), std::min(fx, int(wa.x + wa.w) - (w + extra_w)));
        fy = std::max(int(wa.y), std::min(fy, int(wa.y + wa.h) - (h + extra_h)));
        x = px + (fx - frame.x);
        y = py + (fy - frame.y);
        return true;
    }
    void set_min_size(size_f s) override
    {
        GdkGeometry g{};
        g.min_width = int(s.w);
        g.min_height = int(s.h);
        gtk_window_set_geometry_hints(GTK_WINDOW(win_), nullptr, &g, GDK_HINT_MIN_SIZE);
    }
    std::optional<point_f> position() const override
    {
        if (!placeable()) return std::nullopt;
        int x = 0, y = 0;
        gtk_window_get_position(GTK_WINDOW(win_), &x, &y);
        return point_f{float(x), float(y)};
    }
    void move(point_f p) override
    {
        if (!placeable()) return;
        gtk_window_move(GTK_WINDOW(win_), int(p.x), int(p.y));
        int w = 0, h = 0, x = 0, y = 0;
        gtk_window_get_size(GTK_WINDOW(win_), &w, &h);
        if (frame_on_screen(w, h, x, y)) gtk_window_move(GTK_WINDOW(win_), x, y);
    }
    rect_f work_area() const override
    {
        GdkDisplay* d = gtk_widget_get_display(win_);
        GdkWindow* gw = gtk_widget_get_window(win_);
        GdkMonitor* m = gw ? gdk_display_get_monitor_at_window(d, gw) : nullptr;
        if (!m) m = gdk_display_get_primary_monitor(d);
        if (!m) m = gdk_display_get_monitor(d, 0);
        if (!m) return {};
        GdkRectangle r;
        gdk_monitor_get_workarea(m, &r);
        return without_windows_taskbar({float(r.x), float(r.y), float(r.width), float(r.height)});
    }

    bool fullscreen() const override { return fullscreen_; }
    bool maximized() const override { return maximized_; }
    void set_fullscreen(bool on) override
    {
        if (on)
            gtk_window_fullscreen(GTK_WINDOW(win_));
        else
            gtk_window_unfullscreen(GTK_WINDOW(win_));
    }
    void on_fullscreen_changed(std::function<void(bool)> f) override { on_fullscreen_ = std::move(f); }

    void set_icon(const icon& icon) override
    {
        if (!icon.name.empty() &&
            gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), icon.name.c_str()))
        {
            gtk_window_set_icon_name(GTK_WINDOW(win_), icon.name.c_str());
            return;
        }
        if (!icon.draw) return;
        constexpr int n = 256;
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, n, n);
        cairo_t* cr = cairo_create(surface);
        {
            cairo_painter painter(cr, win_);
            icon.draw(painter, {float(n), float(n)});
        }
        cairo_destroy(cr);
        if (GdkPixbuf* pb = gdk_pixbuf_get_from_surface(surface, 0, 0, n, n))
        {
            gtk_window_set_icon(GTK_WINDOW(win_), pb);
            g_object_unref(pb);
        }
        cairo_surface_destroy(surface);
    }

    void on_key(std::function<bool(const key_event&)> f) override { on_key_ = std::move(f); }

    void set_content(GtkWidget* content, bool margins, const std::vector<menu_bar_item>& menus)
    {
        int margin = !margins ? 0 : dialog_ ? 12 : 6;
        gtk_widget_set_margin_start(content, margin);
        gtk_widget_set_margin_end(content, margin);
        gtk_widget_set_margin_top(content, margin);
        gtk_widget_set_margin_bottom(content, margin);
        GtkWidget* top = content;
        if (!menus.empty())
        {
            top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_box_pack_start(GTK_BOX(top), build_menu_bar(menus), false, false, 0);
            gtk_box_pack_start(GTK_BOX(top), content, true, true, 0);
        }
        gtk_container_add(GTK_CONTAINER(win_), top);
        // GTK 3 widgets start hidden; widgets the app hides later stay hidden.
        gtk_widget_show_all(top);
    }

    void set_menu_bar_visible(bool on) override
    {
        menu_bar_wanted_ = on;
        show_menu_bar();
    }

private:
    // A menu bar's menus are rebuilt whenever the bar is entered (a click
    // on it, an access key, F10), so they show the app's state as it is.
    // Nothing can change that state while a menu is open but its items,
    // and choosing one closes the menus.
    struct bar_menu
    {
        menu_bar_item item;
        GtkWidget* menu = nullptr;  // the submenu, refilled in place
        rap::menu current;               // what its GTK items point into
    };

    GtkWidget* build_menu_bar(const std::vector<menu_bar_item>& menus)
    {
        menu_bar_ = gtk_menu_bar_new();
        for (const menu_bar_item& m : menus)
        {
            auto bar = std::make_unique<bar_menu>(bar_menu{m, nullptr, {}});
            GtkWidget* top = gtk_menu_item_new_with_mnemonic(m.title.c_str());
            bar->menu = gtk_menu_new();
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), bar->menu);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar_), top);
            connect(
                top, "mnemonic-activate",
                +[](GtkWidget*, gboolean, gpointer p) -> gboolean
                {
                    static_cast<window_impl*>(p)->rebuild_menu_bar();
                    return false;
                },
                this);
            bars_.push_back(std::move(bar));
        }
        connect(
            menu_bar_, "button-press-event",
            +[](GtkWidget*, GdkEventButton*, gpointer p) -> gboolean
            {
                static_cast<window_impl*>(p)->rebuild_menu_bar();
                return false;
            },
            this);
        rebuild_menu_bar();
        return menu_bar_;
    }

    void rebuild_menu_bar()
    {
        for (auto& bar : bars_)
        {
            GList* old = gtk_container_get_children(GTK_CONTAINER(bar->menu));
            for (GList* l = old; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
            g_list_free(old);
            bar->current = bar->item.menu ? bar->item.menu() : menu{};
            fill_menu(bar->menu, bar->current);
            gtk_widget_show_all(bar->menu);
        }
    }

    void show_menu_bar()
    {
        if (menu_bar_) gtk_widget_set_visible(menu_bar_, menu_bar_wanted_ && !fullscreen_);
    }

    GtkWidget* menu_bar_ = nullptr;
    std::vector<std::unique_ptr<bar_menu>> bars_;
    bool menu_bar_wanted_ = true;

public:
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
            gtk_style_context_add_class(gtk_widget_get_style_context(button), "suggested-action");
            gtk_widget_set_can_default(button, true);
            gtk_window_set_default(GTK_WINDOW(win_), button);
        }
    }

    GtkWidget* gtk_window() const { return win_; }

    std::vector<std::unique_ptr<part>> parts;

private:
    // Only X11 lets clients place their windows; Wayland's compositor does.
    // (By type name: <gdk/gdkx.h> would bring Xlib's macros, e.g. None.)
    bool placeable() const
    {
        GdkDisplay* d = gtk_widget_get_display(win_);
        return std::strcmp(G_OBJECT_TYPE_NAME(d), "GdkX11Display") == 0;
    }

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
    std::function<void(bool)> on_fullscreen_;
    std::function<bool(const key_event&)> on_key_;
    bool fullscreen_ = false;
    bool maximized_ = false;
};

// -- builder ------------------------------------------------------------------

void set_margins(GtkWidget* w, int m)
{
    gtk_widget_set_margin_start(w, m);
    gtk_widget_set_margin_end(w, m);
    gtk_widget_set_margin_top(w, m);
    gtk_widget_set_margin_bottom(w, m);
}

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
            gtk_container_add(GTK_CONTAINER(box), w);
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
            gtk_style_context_add_class(gtk_widget_get_style_context(label), "dim-label");
            // set_visible on the field shows and hides its label too.
            g_object_set_data(G_OBJECT(field), "rapport-form-label", label);
            if (!s.fields[i].common.visible)
            {
                gtk_widget_set_no_show_all(label, true);
                gtk_widget_hide(label);
            }
            gtk_grid_attach(GTK_GRID(grid), label, 0, int(i), 1, 1);
            gtk_grid_attach(GTK_GRID(grid), field, 1, int(i), 1, 1);
        }
        return grid;
    }

    GtkWidget* make(const node& n, const spec::splitter& s, orientation)
    {
        if (s.panes.empty()) return gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget* paned = panes(s, 0);
        if (GTK_IS_PANED(paned))
        {
            add<splitter_impl>(n, paned);
            initial_split(s, paned);
        }
        return paned;
    }

    // GtkPaned's first split follows natural sizes, which mean little for
    // lists and text views. On the first allocation, panes without stretch
    // get their natural size and the rest is shared by stretch factor.
    static void initial_split(const spec::splitter& s, GtkWidget* top)
    {
        struct split_layout
        {
            bool vertical;
            std::vector<GtkPaned*> paneds;
            std::vector<GtkWidget*> panes;
            std::vector<int> stretch;
            bool scheduled = false;
        };
        auto* split = new split_layout{s.dir == orientation::vertical, {}, {}, {}};
        GtkWidget* w = top;
        for (size_t i = 0; i + 1 < s.panes.size(); ++i)
        {
            auto* p = GTK_PANED(w);
            split->paneds.push_back(p);
            split->panes.push_back(gtk_paned_get_child1(p));
            w = gtk_paned_get_child2(p);
        }
        split->panes.push_back(w);
        for (auto& pane : s.panes) split->stretch.push_back(pane.common.stretch);
        if (std::ranges::all_of(split->stretch, [](int f) { return f == 0; }))
            std::ranges::fill(split->stretch, 1);
        g_object_set_data_full(
            G_OBJECT(top), "rapport-initial-split", split,
            +[](gpointer p) { delete static_cast<split_layout*>(p); });

        g_signal_connect(top, "size-allocate",
                         G_CALLBACK(+[](GtkWidget* top, GdkRectangle* alloc, gpointer p)
                                    {
                                        auto* split = static_cast<split_layout*>(p);
                                        int total = split->vertical ? alloc->height : alloc->width;
                                        if (split->scheduled || total <= 1) return;
                                        split->scheduled = true;
                                        // Positions are set outside the allocation pass.
                                        g_object_ref(top);
                                        g_idle_add_full(
                                            G_PRIORITY_HIGH_IDLE,
                                            +[](gpointer t) -> gboolean
                                            {
                                                auto* top = static_cast<GtkWidget*>(t);
                                                auto* split = static_cast<split_layout*>(g_object_get_data(
                                                    G_OBJECT(top), "rapport-initial-split"));
                                                if (split && !g_object_get_data(G_OBJECT(top),
                                                                                "rapport-user-sized"))
                                                    apply(*split, top);
                                                return G_SOURCE_REMOVE;
                                            },
                                            top, g_object_unref);
                                    }),
                         split);
    }

    template <class Split>
    static void apply(const Split& split, GtkWidget* top)
    {
        int total = split.vertical ? gtk_widget_get_allocated_height(top)
                                   : gtk_widget_get_allocated_width(top);
        int handle = 0;
        gtk_widget_style_get(top, "handle-size", &handle, nullptr);
        handle = std::max(handle, 5);  // wide handles are drawn a little larger

        int fixed = 0, stretch_sum = 0;
        std::vector<int> natural(split.panes.size());
        for (size_t i = 0; i < split.panes.size(); ++i)
        {
            if (split.stretch[i] > 0)
            {
                stretch_sum += split.stretch[i];
                continue;
            }
            int min = 0, nat = 0;
            if (split.vertical)
                gtk_widget_get_preferred_height(split.panes[i], &min, &nat);
            else
                gtk_widget_get_preferred_width(split.panes[i], &min, &nat);
            natural[i] = nat;
            fixed += nat;
        }
        int avail = total - fixed - handle * int(split.paneds.size());
        if (avail <= 0 || stretch_sum == 0) return;
        for (size_t i = 0; i < split.paneds.size(); ++i)
        {
            int size = split.stretch[i] > 0 ? avail * split.stretch[i] / stretch_sum : natural[i];
            gtk_paned_set_position(split.paneds[i], size);
        }
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
        gtk_paned_pack1(p, first, !any_stretch || s.panes[from].common.stretch > 0, false);
        gtk_paned_pack2(p, panes(s, from + 1), !any_stretch || rest_stretch, false);
        return paned;
    }

    GtkWidget* make(const node&, const spec::scroll& s, orientation)
    {
        GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                       s.horizontal ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller), true);
        gtk_widget_set_vexpand(scroller, true);
        if (!s.content.empty())
            gtk_container_add(GTK_CONTAINER(scroller),
                              build(s.content.front(), orientation::vertical));
        return scroller;
    }

    GtkWidget* make(const node&, const spec::group& s, orientation)
    {
        GtkWidget* frame = gtk_frame_new(s.title.c_str());
        if (!s.content.empty())
        {
            GtkWidget* child = build(s.content.front(), orientation::vertical);
            set_margins(child, 6);
            gtk_container_add(GTK_CONTAINER(frame), child);
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
            set_margins(page, 6);
            gtk_notebook_append_page(GTK_NOTEBOOK(nb), page,
                                     gtk_label_new_with_mnemonic(s.titles[i].c_str()));
            pages.push_back(page);
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
            gtk_container_add(GTK_CONTAINER(row), label);
            gtk_container_add(GTK_CONTAINER(list), row);
            const bool lazy = i < s.lazy.size() && s.lazy[i];
            GtkWidget* page = lazy ? gtk_box_new(GTK_ORIENTATION_VERTICAL, 0) : build(s.pages[i], orientation::vertical);
            set_margins(page, 6);
            gtk_stack_add_named(GTK_STACK(stack), page, std::to_string(i).c_str());
            pages.push_back(page);
        }
        gtk_box_pack_start(GTK_BOX(box), list, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), stack, TRUE, TRUE, 0);
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

    GtkWidget* make(const node& n, const spec::button& s, orientation)
    {
        auto& b = add<button_impl>(n, s.icon_only ? std::string() : s.text);
        if (s.icon) b.set_icon(s.icon);
        if (s.icon_only && n.common.tooltip.empty()) b.set_tooltip(s.text);
        if (s.flat) gtk_button_set_relief(GTK_BUTTON(b.outer()), GTK_RELIEF_NONE);
        return b.outer();
    }

    GtkWidget* make(const node& n, const spec::tool_button& s, orientation)
    {
        return add<tool_button_impl>(n, s).outer();
    }

    // Items: ToolButtons, Separator, Spacer (pushes later items to the end),
    // or any other widget, held in a GtkToolItem.
    GtkWidget* make(const node&, const spec::toolbar& s, orientation)
    {
        GtkWidget* bar = gtk_toolbar_new();
        gtk_toolbar_set_style(GTK_TOOLBAR(bar), GTK_TOOLBAR_ICONS);
        for (const node& item : s.items)
        {
            GtkToolItem* ti = nullptr;
            if (std::holds_alternative<spec::separator>(item.spec))
            {
                ti = gtk_separator_tool_item_new();
            }
            else if (std::holds_alternative<spec::spacer>(item.spec))
            {
                ti = gtk_separator_tool_item_new();
                gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(ti), false);
                gtk_tool_item_set_expand(ti, true);
            }
            else if (std::holds_alternative<spec::tool_button>(item.spec))
            {
                ti = GTK_TOOL_ITEM(build(item, orientation::horizontal));
            }
            else
            {
                ti = gtk_tool_item_new();
                GtkWidget* w = build(item, orientation::horizontal);
                gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
                gtk_container_add(GTK_CONTAINER(ti), w);
                if (item.common.stretch > 0) gtk_tool_item_set_expand(ti, true);
            }
            gtk_toolbar_insert(GTK_TOOLBAR(bar), ti, -1);
        }
        return bar;
    }

    GtkWidget* make(const node& n, const spec::slider& s, orientation)
    {
        return add<slider_impl>(n, s).outer();
    }

    GtkWidget* make(const node& n, const spec::spin& s, orientation)
    {
        return add<spin_impl>(n, s).outer();
    }

    GtkWidget* make(const node& n, const spec::color_button& s, orientation)
    {
        return add<color_button_impl>(n, s).outer();
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
                gtk_container_add(GTK_CONTAINER(box), gap);
                spaced = true;
            }
            auto* w = new button_impl(b.text.empty() ? stock_label(b.role) : b.text);
            win_.parts.emplace_back(w);
            if (!b.on_click) win_.add_role_button(w->outer(), b.role);
            sink_.created(b, *w);
            gtk_container_add(GTK_CONTAINER(box), w->outer());
        }
        return box;
    }

    window_impl& win_;
    build_sink& sink_;
};

// -- backend ------------------------------------------------------------------

class gtk3_backend : public backend
{
public:
    static backend* create(int* argc, char** argv)
    {
        // Under WSLg, GTK keeps its own choice, Wayland: WSLg's X server
        // doesn't keep the pointer's shape up to date, leaving whatever was
        // set last (often nothing) over the window. Wayland can't place
        // windows, but WSL is for testing.
        if (!gtk_init_check(argc, &argv)) return nullptr;
        // Dark when the desktop says so: the theme's dark variant.
        follow_color_scheme(
            +[](int scheme)
            {
                g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme",
                             scheme == 1, nullptr);
            });
        init_metrics();
        return new gtk3_backend;
    }

    std::string_view name() const override { return "gtk3"; }

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
        running_ = true;
        gtk_main();
        running_ = false;
        return code_;
    }

    void quit(int code) override
    {
        quit_ = true;
        code_ = code;
        if (running_) gtk_main_quit();
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
                auto state =
                    *static_cast<std::shared_ptr<timer_state>*>(p);  // fn may drop the subscription
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

    void message(rap::window* parent, const message_spec& m,
                 std::function<void(dialog_role)> done) override
    {
        GtkMessageType type = GTK_MESSAGE_INFO;
        switch (m.icon)
        {
        case message_spec::icon::info: type = GTK_MESSAGE_INFO; break;
        case message_spec::icon::warning: type = GTK_MESSAGE_WARNING; break;
        case message_spec::icon::error: type = GTK_MESSAGE_ERROR; break;
        case message_spec::icon::question: type = GTK_MESSAGE_QUESTION; break;
        }
        GtkWidget* dlg = gtk_message_dialog_new(parent_window(parent), GTK_DIALOG_MODAL, type,
                                                GTK_BUTTONS_NONE, "%s", m.text.c_str());
        if (!m.title.empty()) gtk_window_set_title(GTK_WINDOW(dlg), m.title.c_str());
        if (!m.detail.empty())
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s",
                                                     m.detail.c_str());

        struct context
        {
            std::vector<dialog_role> roles;
            std::function<void(dialog_role)> done;
        };
        auto* ctx = new context{m.buttons, std::move(done)};
        if (ctx->roles.empty()) ctx->roles = {dialog_role::ok};
        auto ordered = ctx->roles;
        std::ranges::stable_sort(ordered, {}, gnome_order);
        for (auto r : ordered)
        {
            auto i = std::ranges::find(ctx->roles, r) - ctx->roles.begin();
            gtk_dialog_add_button(GTK_DIALOG(dlg), stock_label(r).c_str(), int(i));
        }

        g_signal_connect_data(
            dlg, "response",
            G_CALLBACK(+[](GtkDialog* d, int resp, gpointer p)
                       {
                           auto* ctx = static_cast<context*>(p);
                           dialog_role role = resp >= 0 && size_t(resp) < ctx->roles.size()
                                                 ? ctx->roles[size_t(resp)]
                                                 : dialog_role::cancel;
                           auto done = std::move(ctx->done);
                           gtk_widget_destroy(GTK_WIDGET(d));  // frees ctx
                           if (done) done(role);
                       }),
            ctx, +[](gpointer p, GClosure*) { delete static_cast<context*>(p); }, GConnectFlags(0));
        gtk_widget_show(dlg);
    }

    void choose_files(rap::window* parent, const file_dialog_spec& f,
                     std::function<void(std::vector<std::string>)> done) override
    {
        GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_OPEN;
        if (f.mode == file_dialog_spec::mode::save) action = GTK_FILE_CHOOSER_ACTION_SAVE;
        if (f.mode == file_dialog_spec::mode::folder) action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;

        // GtkFileChooserNative goes through the portal when available.
        GtkFileChooserNative* dlg =
            gtk_file_chooser_native_new(f.title.empty() ? nullptr : f.title.c_str(),
                                        parent_window(parent), action, nullptr, nullptr);
        auto* chooser = GTK_FILE_CHOOSER(dlg);
        gtk_file_chooser_set_select_multiple(chooser, f.mode == file_dialog_spec::mode::open_many);
        if (action == GTK_FILE_CHOOSER_ACTION_SAVE)
            gtk_file_chooser_set_do_overwrite_confirmation(chooser, true);
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
            g_object_set_data_full(G_OBJECT(ff), "rapport-ext", g_strdup(default_extension(flt.pattern).c_str()),
                                   g_free);
            gtk_file_chooser_add_filter(chooser, ff);
        }
        if (!f.initial_path.empty())
        {
            if (g_file_test(f.initial_path.c_str(), G_FILE_TEST_IS_DIR))
                gtk_file_chooser_set_current_folder(chooser, f.initial_path.c_str());
            else
                gtk_file_chooser_set_filename(chooser, f.initial_path.c_str());
        }

        using done_fn = std::function<void(std::vector<std::string>)>;
        g_signal_connect_data(
            dlg, "response",
            G_CALLBACK(+[](GtkNativeDialog* d, int resp, gpointer p)
                       {
                           std::vector<std::string> out;
                           if (resp == GTK_RESPONSE_ACCEPT)
                           {
                               auto* chooser = GTK_FILE_CHOOSER(d);
                               GSList* names = gtk_file_chooser_get_filenames(chooser);
                               for (GSList* l = names; l; l = l->next)
                                   out.push_back(static_cast<char*>(l->data));
                               g_slist_free_full(names, g_free);
                               // A save typed without an extension gets the
                               // chosen filter's, as on Windows. GTK confirmed
                               // overwriting the name as typed, not that one,
                               // so replacing an existing file is asked here;
                               // Cancel reopens the chooser on the typed name.
                               GtkFileFilter* ff = gtk_file_chooser_get_filter(chooser);
                               const char* ext =
                                   ff ? static_cast<const char*>(g_object_get_data(G_OBJECT(ff), "rapport-ext")) : nullptr;
                               if (gtk_file_chooser_get_action(chooser) == GTK_FILE_CHOOSER_ACTION_SAVE &&
                                   out.size() == 1 && ext)
                               {
                                   const std::string typed = out[0];
                                   out[0] = with_extension(typed, ext);
                                   if (out[0] != typed && g_file_test(out[0].c_str(), G_FILE_TEST_EXISTS) &&
                                       !confirm_replace(gtk_native_dialog_get_transient_for(d), out[0]))
                                   {
                                       gtk_file_chooser_set_filename(chooser, typed.c_str());
                                       gtk_native_dialog_show(d);
                                       return;
                                   }
                               }
                           }
                           done_fn done = std::move(*static_cast<done_fn*>(p));
                           g_object_unref(d);  // frees p
                           if (done) done(std::move(out));
                       }),
            new done_fn(std::move(done)), +[](gpointer p, GClosure*) { delete static_cast<done_fn*>(p); },
            GConnectFlags(0));
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
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
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), s.c_str(),
                               int(s.size()));
    }

    bool save_snapshot(const std::string& path) override { return window_impl::save_last_shown(path); }

    void set_clipboard_image(int w, int h, pixel_format format, const uint8_t* pixels,
                           size_t stride) override
    {
        GdkPixbuf* pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, w, h);
        if (!pb) return;
        uint8_t* dst = gdk_pixbuf_get_pixels(pb);
        const int dst_stride = gdk_pixbuf_get_rowstride(pb);
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* src = pixels + size_t(y) * stride;
            uint8_t* row = dst + size_t(y) * dst_stride;
            if (format == pixel_format::rgba8)
            {
                std::memcpy(row, src, size_t(w) * 4);
                continue;
            }
            for (int x = 0; x < w; ++x)  // premultiplied BGRA -> straight RGBA
            {
                const unsigned a = src[x * 4 + 3];
                auto un = [a](unsigned c)
                { return uint8_t(a ? std::min(255u, (c * 255 + a / 2) / a) : 0); };
                row[x * 4 + 0] = un(src[x * 4 + 2]);
                row[x * 4 + 1] = un(src[x * 4 + 1]);
                row[x * 4 + 2] = un(src[x * 4 + 0]);
                row[x * 4 + 3] = uint8_t(a);
            }
        }
        gtk_clipboard_set_image(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), pb);
        g_object_unref(pb);
    }

private:
    static GtkWindow* parent_window(rap::window* w)
    {
        return w ? GTK_WINDOW(static_cast<window_impl*>(w)->gtk_window()) : nullptr;
    }

    bool quit_ = false;
    bool running_ = false;
    int code_ = 0;
};

}  // namespace
}  // namespace rap::gtk3

RAPPORT_BACKEND_ENTRY(gtk3, rap::gtk3::gtk3_backend)
