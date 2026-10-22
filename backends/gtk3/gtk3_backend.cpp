// GTK 3 backend (package anyui-gtk3).
//
// Lists use GtkTreeView in fixed-height mode over a virtual GtkTreeModel
// (AnyuiTreeRows) whose iters carry the row index; cells pull text from the
// anyui::ListModel through cell data functions, so only visible rows are
// ever touched. Painted columns use a custom cell renderer.
#include <gtk/gtk.h>

#include "gtk_common.h"

#include <cmath>
#include <cstdlib>
#include <memory>

// ---------------------------------------------------------------- AnyuiTreeRows

G_BEGIN_DECLS
G_DECLARE_FINAL_TYPE(AnyuiTreeRows, anyui_tree_rows, ANYUI, TREE_ROWS, GObject)
G_END_DECLS

struct _AnyuiTreeRows
{
    GObject parent_instance;
    gint stamp;
    guint count;
    anyui::ListModel* model;
    guint searchColumn;
};

static guint rowOf(GtkTreeIter* iter)
{
    return GPOINTER_TO_UINT(iter->user_data);
}

static gboolean setIter(AnyuiTreeRows* self, GtkTreeIter* iter, guint row)
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
    return setIter(ANYUI_TREE_ROWS(m), iter, guint(gtk_tree_path_get_indices(path)[0]));
}

static GtkTreePath* rows_get_path(GtkTreeModel*, GtkTreeIter* iter)
{
    return gtk_tree_path_new_from_indices(int(rowOf(iter)), -1);
}

// Only used by type-ahead search; cells use data functions instead.
static void rows_get_value(GtkTreeModel* m, GtkTreeIter* iter, gint, GValue* value)
{
    auto* self = ANYUI_TREE_ROWS(m);
    g_value_init(value, G_TYPE_STRING);
    if (self->model && rowOf(iter) < self->count)
        g_value_set_string(value, self->model->text(rowOf(iter), self->searchColumn).c_str());
}

static gboolean rows_iter_next(GtkTreeModel* m, GtkTreeIter* iter)
{
    return setIter(ANYUI_TREE_ROWS(m), iter, rowOf(iter) + 1);
}

static gboolean rows_iter_previous(GtkTreeModel* m, GtkTreeIter* iter)
{
    guint row = rowOf(iter);
    return row > 0 && setIter(ANYUI_TREE_ROWS(m), iter, row - 1);
}

static gboolean rows_iter_children(GtkTreeModel* m, GtkTreeIter* iter, GtkTreeIter* parent)
{
    return !parent && setIter(ANYUI_TREE_ROWS(m), iter, 0);
}

static gboolean rows_iter_has_child(GtkTreeModel*, GtkTreeIter*)
{
    return false;
}

static gint rows_iter_n_children(GtkTreeModel* m, GtkTreeIter* iter)
{
    return iter ? 0 : gint(ANYUI_TREE_ROWS(m)->count);
}

static gboolean rows_iter_nth_child(GtkTreeModel* m, GtkTreeIter* iter, GtkTreeIter* parent, gint n)
{
    return !parent && n >= 0 && setIter(ANYUI_TREE_ROWS(m), iter, guint(n));
}

static gboolean rows_iter_parent(GtkTreeModel*, GtkTreeIter*, GtkTreeIter*)
{
    return false;
}

static void anyui_tree_rows_iface_init(GtkTreeModelIface* iface)
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

G_DEFINE_TYPE_WITH_CODE(AnyuiTreeRows, anyui_tree_rows, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GTK_TYPE_TREE_MODEL, anyui_tree_rows_iface_init))

static void anyui_tree_rows_class_init(AnyuiTreeRowsClass*) {}

static void anyui_tree_rows_init(AnyuiTreeRows* self)
{
    self->stamp = g_random_int();
    self->count = 0;
    self->model = nullptr;
    self->searchColumn = 0;
}

// ---------------------------------------------------------------- AnyuiPaintRenderer

// Cell renderer for painted columns; the column's data function sets the
// row, and the owning list draws through `draw`.
using PaintFn = void (*)(gpointer ctx, guint row, cairo_t*, GtkWidget*, const GdkRectangle*,
                         GtkCellRendererState);

G_BEGIN_DECLS
G_DECLARE_FINAL_TYPE(AnyuiPaintRenderer, anyui_paint_renderer, ANYUI, PAINT_RENDERER,
                     GtkCellRenderer)
G_END_DECLS

struct _AnyuiPaintRenderer
{
    GtkCellRenderer parent_instance;
    guint row;
    gpointer ctx;
    PaintFn draw;
    gint height;
};

G_DEFINE_TYPE(AnyuiPaintRenderer, anyui_paint_renderer, GTK_TYPE_CELL_RENDERER)

static void paint_renderer_render(GtkCellRenderer* cell, cairo_t* cr, GtkWidget* widget,
                                  const GdkRectangle* background, const GdkRectangle* area,
                                  GtkCellRendererState flags)
{
    auto* self = ANYUI_PAINT_RENDERER(cell);
    // Full row height so graph lines join between rows; the cell area
    // excludes the view's vertical padding.
    GdkRectangle full{area->x, background->y, area->width, background->height};
    if (self->draw) self->draw(self->ctx, self->row, cr, widget, &full, flags);
}

static void paint_renderer_get_preferred_height(GtkCellRenderer* cell, GtkWidget*, gint* min,
                                                gint* natural)
{
    // Either pointer may be null.
    gint h = ANYUI_PAINT_RENDERER(cell)->height;
    if (min) *min = h;
    if (natural) *natural = h;
}

static void paint_renderer_get_preferred_width(GtkCellRenderer*, GtkWidget*, gint* min,
                                               gint* natural)
{
    if (min) *min = 1;
    if (natural) *natural = 1;
}

static void anyui_paint_renderer_class_init(AnyuiPaintRendererClass* klass)
{
    auto* cell = GTK_CELL_RENDERER_CLASS(klass);
    cell->render = paint_renderer_render;
    cell->get_preferred_height = paint_renderer_get_preferred_height;
    cell->get_preferred_width = paint_renderer_get_preferred_width;
}

static void anyui_paint_renderer_init(AnyuiPaintRenderer* self)
{
    self->row = 0;
    self->ctx = nullptr;
    self->draw = nullptr;
    self->height = 16;
}

namespace anyui::gtk3
{
namespace
{
using namespace gtkcommon;
}
}  // namespace anyui::gtk3

anyui::Color anyui::gtkcommon::themeColor(GtkWidget* w, ThemeColor role)
{
    GtkStyleContext* ctx = gtk_widget_get_style_context(w);
    GdkRGBA fg;
    gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg);

    // Named colours every GTK 3 theme is expected to define.
    const char* name = nullptr;
    switch (role)
    {
    case ThemeColor::Window: name = "theme_bg_color"; break;
    case ThemeColor::WindowText: name = "theme_fg_color"; break;
    case ThemeColor::Base: name = "theme_base_color"; break;
    case ThemeColor::Text: name = "theme_text_color"; break;
    case ThemeColor::Highlight:
    case ThemeColor::Accent: name = "theme_selected_bg_color"; break;
    case ThemeColor::HighlightText: name = "theme_selected_fg_color"; break;
    case ThemeColor::Positive: name = "success_color"; break;
    case ThemeColor::Negative: name = "error_color"; break;
    case ThemeColor::Link: name = "link_color"; break;
    default: break;
    }
    GdkRGBA c;
    if (name && gtk_style_context_lookup_color(ctx, name, &c)) return toColor(c);
    return fallbackColor(role, toColor(fg), isDarkText(fg));
}

namespace anyui::gtk3
{
namespace
{

// ---------------------------------------------------------------- widgets

// A container (box, form, group, toolbar) as a plain Widget, for handles.
class WPlain : public W<anyui::Widget>
{
public:
    using W::W;
};

class WLabel : public W<anyui::Label>
{
public:
    explicit WLabel(const spec::Label& s)
        : W(gtk_label_new(s.text.c_str()))
    {
        gtk_label_set_xalign(GTK_LABEL(w_), 0);
        gtk_label_set_selectable(GTK_LABEL(w_), s.selectable);
        if (s.ellipsize) gtk_label_set_ellipsize(GTK_LABEL(w_), PANGO_ELLIPSIZE_END);
    }

    void setText(const std::string& t) override { gtk_label_set_text(GTK_LABEL(w_), t.c_str()); }
};

// A GtkImage for a named icon the theme has; otherwise a drawing area that
// runs Icon::draw whenever it paints, so drawn icons follow scale and theme
// (the area inherits its parent's CSS colour, e.g. greyed when insensitive).
GtkWidget* iconWidget(const Icon& icon, int size)
{
    if (!icon.name.empty() &&
        gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), icon.name.c_str()))
    {
        GtkWidget* img = gtk_image_new_from_icon_name(icon.name.c_str(), GTK_ICON_SIZE_BUTTON);
        gtk_image_set_pixel_size(GTK_IMAGE(img), size);
        return img;
    }
    if (!icon.draw) return nullptr;
    GtkWidget* area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, size, size);
    g_signal_connect_data(
        area, "draw",
        G_CALLBACK(+[](GtkWidget* w, cairo_t* cr, gpointer p) -> gboolean
                   {
                       CairoPainter painter(cr, w);
                       (*static_cast<std::function<void(Painter&, SizeF)>*>(p))(
                           painter, {float(gtk_widget_get_allocated_width(w)),
                                     float(gtk_widget_get_allocated_height(w))});
                       return true;
                   }),
        new std::function<void(Painter&, SizeF)>(icon.draw),
        +[](gpointer p, GClosure*)
        { delete static_cast<std::function<void(Painter&, SizeF)>*>(p); },
        GConnectFlags(0));
    return area;
}

class WButton : public W<anyui::Button>
{
public:
    explicit WButton(const std::string& text)
        : W(gtk_button_new_with_mnemonic(text.c_str()))
    {
        connect(
            w_, "clicked",
            +[](GtkButton*, gpointer p)
            {
                auto* self = static_cast<WButton*>(p);
                if (self->click_) self->click_();
            },
            this);
    }

    void setText(const std::string& t) override { gtk_button_set_label(GTK_BUTTON(w_), t.c_str()); }
    void setIcon(const Icon& icon) override
    {
        GtkWidget* img = iconWidget(icon, 16);
        gtk_button_set_image(GTK_BUTTON(w_), img);
        gtk_button_set_always_show_image(GTK_BUTTON(w_), img != nullptr);
        if (img) gtk_widget_show(img);
    }
    void onClick(std::function<void()> f) override { click_ = std::move(f); }

private:
    std::function<void()> click_;
};

GtkWidget* buildMenu(const Menu& items);

// Appends `items` to `menu`; the GTK items point into `items`, which must
// outlive them.
void fillMenu(GtkWidget* menu, const Menu& items)
{
    for (const MenuItem& item : items)
    {
        GtkWidget* mi;
        if (item.isSeparator())
        {
            mi = gtk_separator_menu_item_new();
        }
        else if (!item.children.empty())
        {
            mi = gtk_menu_item_new_with_mnemonic(item.label.c_str());
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), buildMenu(item.children));
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
                gtk_accelerator_parse(gtkAccel(item.shortcut).c_str(), &key, &mods);
                GtkWidget* label = gtk_bin_get_child(GTK_BIN(mi));
                if (key && GTK_IS_ACCEL_LABEL(label))
                    gtk_accel_label_set_accel(GTK_ACCEL_LABEL(label), key, mods);
            }
            // The menu lives no longer than menuItems_, which owns `item`.
            g_signal_connect(mi, "activate",
                             G_CALLBACK(+[](GtkMenuItem*, gpointer p)
                                        {
                                            auto* item = static_cast<const MenuItem*>(p);
                                            if (item->action) item->action();
                                        }),
                             const_cast<MenuItem*>(&item));
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
}

GtkWidget* buildMenu(const Menu& items)
{
    GtkWidget* menu = gtk_menu_new();
    fillMenu(menu, items);
    return menu;
}

// A popup menu built from an anyui::Menu. Its GTK items point into the
// Menu it holds, so the two are replaced together.
class PopupMenu
{
public:
    // Shows below `attach` when `event` is null (keyboard / menu buttons),
    // else at the pointer.
    void show(Menu items, GtkWidget* attach, GdkEvent* event, bool below = false)
    {
        destroy();
        items_ = std::move(items);
        menu_ = buildMenu(items_);
        gtk_menu_attach_to_widget(GTK_MENU(menu_), attach, nullptr);
        if (onHide_)
            g_signal_connect(menu_, "deactivate",
                             G_CALLBACK(+[](GtkMenuShell*, gpointer p)
                                        { (*static_cast<std::function<void()>*>(p))(); }),
                             &onHide_);
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

    void onHide(std::function<void()> f) { onHide_ = std::move(f); }

    // Call before the attach widget goes away.
    void destroy()
    {
        if (menu_) gtk_widget_destroy(menu_);
        menu_ = nullptr;
    }

private:
    Menu items_;
    GtkWidget* menu_ = nullptr;
    std::function<void()> onHide_;
};

class WToolButton : public W<anyui::ToolButton>
{
public:
    explicit WToolButton(const spec::ToolButton& s)
        : W(GTK_WIDGET(s.toggle ? gtk_toggle_tool_button_new()
                                : gtk_tool_button_new(nullptr, s.label.c_str())))
        , toggle_(s.toggle)
    {
        gtk_tool_button_set_label(GTK_TOOL_BUTTON(w_), s.label.c_str());
        gtk_widget_set_tooltip_text(w_, s.label.c_str());
        setIcon(s.icon);
        if (toggle_)
        {
            gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(w_), s.checked);
            connect(
                w_, "toggled",
                +[](GtkToggleToolButton* b, gpointer p)
                {
                    auto* self = static_cast<WToolButton*>(p);
                    if (!self->quiet_ && self->toggled_)
                        self->toggled_(gtk_toggle_tool_button_get_active(b));
                },
                this);
        }
        connect(
            w_, "clicked",
            +[](GtkToolButton*, gpointer p)
            {
                auto* self = static_cast<WToolButton*>(p);
                if (!self->quiet_ && self->click_) self->click_();
            },
            this);
    }

    void setIcon(const Icon& icon) override
    {
        GtkWidget* img = iconWidget(icon, 16);
        gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(w_), img);
        if (img) gtk_widget_show(img);
    }
    bool checked() const override
    {
        return toggle_ && gtk_toggle_tool_button_get_active(GTK_TOGGLE_TOOL_BUTTON(w_));
    }
    void setChecked(bool c) override
    {
        if (!toggle_) return;
        quiet_ = true;
        gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(w_), c);
        quiet_ = false;
    }
    void onClick(std::function<void()> f) override { click_ = std::move(f); }
    void onToggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }

private:
    bool toggle_;
    bool quiet_ = false;
    std::function<void()> click_;
    std::function<void(bool)> toggled_;
};

class WSlider : public W<anyui::Slider>
{
public:
    explicit WSlider(const spec::Slider& s)
        : W(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, s.min, s.max,
                                     s.step > 0 ? s.step : 0.01))
    {
        gtk_range_set_increments(GTK_RANGE(w_), s.step, s.page);
        gtk_scale_set_digits(GTK_SCALE(w_), s.digits);
        gtk_scale_set_draw_value(GTK_SCALE(w_), s.showValue);
        gtk_range_set_value(GTK_RANGE(w_), s.value);
        for (auto& [at, label] : s.marks)
            gtk_scale_add_mark(GTK_SCALE(w_), at, GTK_POS_BOTTOM,
                               label.empty() ? nullptr : label.c_str());
        connect(
            w_, "value-changed",
            +[](GtkRange* r, gpointer p)
            {
                auto* self = static_cast<WSlider*>(p);
                if (!self->quiet_ && self->changed_) self->changed_(gtk_range_get_value(r));
            },
            this);
    }

    double value() const override { return gtk_range_get_value(GTK_RANGE(w_)); }
    void setValue(double v) override
    {
        quiet_ = true;
        gtk_range_set_value(GTK_RANGE(w_), v);
        quiet_ = false;
    }
    void setRange(double min, double max) override
    {
        quiet_ = true;
        gtk_range_set_range(GTK_RANGE(w_), min, max);
        quiet_ = false;
    }
    void onChanged(std::function<void(double)> f) override { changed_ = std::move(f); }

private:
    bool quiet_ = false;
    std::function<void(double)> changed_;
};

class WSpin : public W<anyui::Spin>
{
public:
    explicit WSpin(const spec::Spin& s)
        : W(gtk_spin_button_new_with_range(s.min, s.max, s.step > 0 ? s.step : 1))
    {
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(w_), guint(std::max(s.digits, 0)));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w_), s.value);
        connect(
            w_, "value-changed",
            +[](GtkSpinButton* b, gpointer p)
            {
                auto* self = static_cast<WSpin*>(p);
                if (!self->quiet_ && self->changed_) self->changed_(gtk_spin_button_get_value(b));
            },
            this);
    }

    double value() const override { return gtk_spin_button_get_value(GTK_SPIN_BUTTON(w_)); }
    void setValue(double v) override
    {
        quiet_ = true;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w_), v);
        quiet_ = false;
    }
    void setRange(double min, double max) override
    {
        quiet_ = true;
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(w_), min, max);
        quiet_ = false;
    }
    void onChanged(std::function<void(double)> f) override { changed_ = std::move(f); }

private:
    bool quiet_ = false;
    std::function<void(double)> changed_;
};

GdkRGBA toRgba(Color c)
{
    return {c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0};
}

class WColorButton : public W<anyui::ColorButton>
{
public:
    explicit WColorButton(const spec::ColorButton& s)
        : W(gtk_color_button_new())
    {
        const GdkRGBA rgba = toRgba(s.color);
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(w_), &rgba);
        gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(w_), s.alpha);
        if (!s.title.empty()) g_object_set(w_, "title", s.title.c_str(), nullptr);
        connect(
            w_, "color-set",  // the user chose; not emitted by setColor
            +[](GtkColorButton*, gpointer p)
            {
                auto* self = static_cast<WColorButton*>(p);
                if (self->changed_) self->changed_(self->color());
            },
            this);
    }

    Color color() const override
    {
        GdkRGBA c;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(w_), &c);
        return toColor(c);
    }
    void setColor(Color c) override
    {
        const GdkRGBA rgba = toRgba(c);
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(w_), &rgba);
    }
    void onChanged(std::function<void(Color)> f) override { changed_ = std::move(f); }

private:
    std::function<void(Color)> changed_;
};

class WCheck : public W<anyui::Check>
{
public:
    explicit WCheck(const spec::Check& s)
        : W(gtk_check_button_new_with_mnemonic(s.text.c_str()))
    {
        setState(s.state);
        connect(
            w_, "toggled",
            +[](GtkToggleButton* b, gpointer p)
            {
                auto* self = static_cast<WCheck*>(p);
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

    bool checked() const override { return state() == CheckState::Checked; }
    void setChecked(bool v) override { setState(v ? CheckState::Checked : CheckState::Unchecked); }

    CheckState state() const override
    {
        auto* b = GTK_TOGGLE_BUTTON(w_);
        if (gtk_toggle_button_get_inconsistent(b)) return CheckState::Indeterminate;
        return gtk_toggle_button_get_active(b) ? CheckState::Checked : CheckState::Unchecked;
    }

    void setState(CheckState s) override
    {
        auto* b = GTK_TOGGLE_BUTTON(w_);
        updating_ = true;
        gtk_toggle_button_set_inconsistent(b, s == CheckState::Indeterminate);
        gtk_toggle_button_set_active(b, s == CheckState::Checked);
        updating_ = false;
    }

    void onToggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }

private:
    std::function<void(bool)> toggled_;
    bool updating_ = false;
};

class WMenuButton : public W<anyui::MenuButton>
{
public:
    explicit WMenuButton(const spec::MenuButton& s)
        : W(gtk_toggle_button_new())
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
        if (s.icon) setIcon(s.icon);

        menu_.onHide([this] { sync(); });
        connect(
            w_, "toggled",
            +[](GtkToggleButton*, gpointer p) { static_cast<WMenuButton*>(p)->clicked(); }, this);
    }

    void setText(const std::string& t) override
    {
        gtk_label_set_text_with_mnemonic(GTK_LABEL(label_), t.c_str());
    }
    void setIcon(const Icon& icon) override
    {
        if (icon_) gtk_widget_destroy(icon_);
        icon_ = iconWidget(icon, 16);
        if (!icon_) return;
        gtk_box_pack_start(GTK_BOX(box_), icon_, false, false, 0);
        gtk_box_reorder_child(GTK_BOX(box_), icon_, 0);
        gtk_widget_show(icon_);
    }
    void setPushed(bool v) override
    {
        pushed_ = v;
        sync();
    }
    void onMenu(std::function<Menu()> f) override { menuFn_ = std::move(f); }

protected:
    void onDisconnect() override { menu_.destroy(); }

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
        Menu items = menuFn_ ? menuFn_() : Menu{};
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
    PopupMenu menu_;
    std::function<Menu()> menuFn_;
    bool pushed_ = false;
    bool updating_ = false;
};

class WProgressBar : public W<anyui::ProgressBar>
{
public:
    WProgressBar()
        : W(gtk_progress_bar_new())
    {
        gtk_widget_set_valign(w_, GTK_ALIGN_CENTER);
    }

    void setFraction(float f) override
    {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w_), std::clamp(f, 0.f, 1.f));
    }
    void pulse() override { gtk_progress_bar_pulse(GTK_PROGRESS_BAR(w_)); }
};

// GTK 3 has no date picker: a button showing the date, with a calendar in
// a popover. onChanged fires when the popover closes on a new date.
class WDatePicker : public W<anyui::DatePicker>
{
public:
    explicit WDatePicker(const spec::DatePicker& s)
        : W(gtk_button_new_with_label(""))
    {
        calendar_ = gtk_calendar_new();
        gtk_calendar_set_display_options(GTK_CALENDAR(calendar_),
                                         GtkCalendarDisplayOptions(GTK_CALENDAR_SHOW_HEADING |
                                                                   GTK_CALENDAR_SHOW_DAY_NAMES |
                                                                   GTK_CALENDAR_SHOW_WEEK_NUMBERS));
        popover_ = gtk_popover_new(w_);
        gtk_container_add(GTK_CONTAINER(popover_), calendar_);
        gtk_widget_show(calendar_);
        setDate(s.date);

        connect(
            w_, "clicked",
            +[](GtkButton*, gpointer p)
            {
                auto* self = static_cast<WDatePicker*>(p);
                self->showCalendar();
            },
            this);
        connect(
            calendar_, "day-selected",
            +[](GtkCalendar* c, gpointer p)
            {
                auto* self = static_cast<WDatePicker*>(p);
                if (self->updating_) return;
                guint y, m, d;
                gtk_calendar_get_date(c, &y, &m, &d);
                self->date_ = {int(y), int(m) + 1, int(d)};
                self->updateLabel();
            },
            this);
        connect(
            calendar_, "day-selected-double-click",
            +[](GtkCalendar*, gpointer p)
            { gtk_popover_popdown(GTK_POPOVER(static_cast<WDatePicker*>(p)->popover_)); },
            this);
        connect(
            popover_, "closed",
            +[](GtkPopover*, gpointer p)
            {
                auto* self = static_cast<WDatePicker*>(p);
                if (self->date_ != self->shown_ && self->changed_) self->changed_(self->date_);
            },
            this);
    }

    Date date() const override { return date_; }
    void setDate(Date d) override
    {
        date_ = d;
        updateLabel();
    }
    void onChanged(std::function<void(Date)> f) override { changed_ = std::move(f); }

private:
    void showCalendar()
    {
        shown_ = date_;
        updating_ = true;
        auto* c = GTK_CALENDAR(calendar_);
        gtk_calendar_select_month(c, guint(date_.month - 1), guint(date_.year));
        gtk_calendar_select_day(c, guint(date_.day));
        updating_ = false;
        gtk_popover_popup(GTK_POPOVER(popover_));
    }

    void updateLabel()
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
    Date date_, shown_;
    std::function<void(Date)> changed_;
    bool updating_ = false;
};

class WEntry : public W<anyui::Entry>
{
public:
    explicit WEntry(const spec::Entry& s)
        : W(gtk_entry_new())
    {
        gtk_entry_set_text(GTK_ENTRY(w_), s.text.c_str());
        if (!s.placeholder.empty()) setPlaceholder(s.placeholder);
        connect(
            w_, "changed",
            +[](GtkEditable* e, gpointer p)
            {
                auto* self = static_cast<WEntry*>(p);
                if (self->changed_) self->changed_(gtk_entry_get_text(GTK_ENTRY(e)));
            },
            this);
        connect(
            w_, "activate",
            +[](GtkEntry*, gpointer p)
            {
                auto* self = static_cast<WEntry*>(p);
                if (self->activate_) self->activate_();
            },
            this);
    }

    std::string text() const override { return gtk_entry_get_text(GTK_ENTRY(w_)); }
    void setText(const std::string& t) override { gtk_entry_set_text(GTK_ENTRY(w_), t.c_str()); }
    void setPlaceholder(const std::string& t) override
    {
        gtk_entry_set_placeholder_text(GTK_ENTRY(w_), t.c_str());
    }
    void onChanged(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void onActivate(std::function<void()> f) override { activate_ = std::move(f); }

private:
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

class WSelect : public W<anyui::Select>
{
public:
    explicit WSelect(const spec::Select& s)
        : W(gtk_combo_box_text_new())
    {
        setItems(s.items);
        setCurrent(s.current);
        connect(
            w_, "changed",
            +[](GtkComboBox* c, gpointer p)
            {
                auto* self = static_cast<WSelect*>(p);
                if (self->changed_) self->changed_(gtk_combo_box_get_active(c));
            },
            this);
    }

    void setItems(std::vector<std::string> items) override
    {
        auto* combo = GTK_COMBO_BOX_TEXT(w_);
        gtk_combo_box_text_remove_all(combo);
        for (auto& i : items) gtk_combo_box_text_append_text(combo, i.c_str());
    }
    int current() const override { return gtk_combo_box_get_active(GTK_COMBO_BOX(w_)); }
    void setCurrent(int i) override { gtk_combo_box_set_active(GTK_COMBO_BOX(w_), i); }
    void onChanged(std::function<void(int)> f) override { changed_ = std::move(f); }

private:
    std::function<void(int)> changed_;
};

class WCombo : public W<anyui::Combo>
{
public:
    explicit WCombo(const spec::Combo& s)
        : W(gtk_combo_box_text_new_with_entry())
    {
        entry_ = gtk_bin_get_child(GTK_BIN(w_));
        setItems(s.items);
        setText(s.text);
        connect(
            entry_, "changed",
            +[](GtkEditable* e, gpointer p)
            {
                auto* self = static_cast<WCombo*>(p);
                if (self->changed_) self->changed_(gtk_entry_get_text(GTK_ENTRY(e)));
            },
            this);
        connect(
            entry_, "activate",
            +[](GtkEntry*, gpointer p)
            {
                auto* self = static_cast<WCombo*>(p);
                if (self->activate_) self->activate_();
            },
            this);
    }

    void setItems(std::vector<std::string> items) override
    {
        auto* combo = GTK_COMBO_BOX_TEXT(w_);
        gtk_combo_box_text_remove_all(combo);
        for (auto& i : items) gtk_combo_box_text_append_text(combo, i.c_str());
    }
    std::string text() const override { return gtk_entry_get_text(GTK_ENTRY(entry_)); }
    void setText(const std::string& t) override
    {
        gtk_entry_set_text(GTK_ENTRY(entry_), t.c_str());
    }
    void onChanged(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void onActivate(std::function<void()> f) override { activate_ = std::move(f); }

private:
    GtkWidget* entry_;
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

class WTextView : public W<anyui::TextView>
{
public:
    explicit WTextView(const spec::TextView& s)
        : W(gtk_text_view_new(), gtk_scrolled_window_new(nullptr, nullptr))
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
                "textview { font-size: " + std::to_string(compactFont(w_).px) + "px; }";
            GtkCssProvider* css = gtk_css_provider_new();
            gtk_css_provider_load_from_data(css, rule.c_str(), -1, nullptr);
            gtk_style_context_add_provider(gtk_widget_get_style_context(w_),
                                           GTK_STYLE_PROVIDER(css),
                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
        }
        setReadOnly(s.readOnly);
        buffer_ = gtk_text_view_get_buffer(tv);
        setText(s.text);
        connect(
            buffer_, "changed",
            +[](GtkTextBuffer*, gpointer p)
            {
                auto* self = static_cast<WTextView*>(p);
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

    void setText(const std::string& t, std::vector<Span> spans = {}) override
    {
        gtk_text_buffer_set_text(buffer_, t.c_str(), int(t.size()));
        tags_.apply(buffer_, w_, t, std::move(spans));
    }

    void append(const std::string& t, const TextStyle& style) override
    {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer_, &end);
        gtk_text_buffer_insert_with_tags(buffer_, &end, t.c_str(), int(t.size()),
                                         tags_.get(buffer_, w_, style), nullptr);
    }

    void setReadOnly(bool ro) override
    {
        gtk_text_view_set_editable(GTK_TEXT_VIEW(w_), !ro);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(w_), !ro);
    }

    void onChanged(std::function<void()> f) override { changed_ = std::move(f); }

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
    TagCache tags_;
    std::function<void()> changed_;
};

// A splitter of n panes is a chain of n-1 nested GtkPaneds (each marked
// "anyui-split-chain"); sizes are one per pane, along the split.
class WSplitter : public W<anyui::Splitter>
{
public:
    explicit WSplitter(GtkWidget* paned)
        : W(paned)
    {
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
            if (!next || !g_object_get_data(G_OBJECT(next), "anyui-split-chain"))
            {
                out.push_back(next ? length(next) : 0);
                return out;
            }
            w = next;
        }
    }

    // Overrides the first layout from stretch factors.
    void setSizes(const std::vector<float>& s) override
    {
        g_object_set_data(G_OBJECT(w_), "anyui-user-sized", GINT_TO_POINTER(1));
        GtkWidget* w = w_;
        for (size_t i = 0; i + 1 < s.size() && w; ++i)
        {
            auto* p = GTK_PANED(w);
            if (s[i] > 0) gtk_paned_set_position(p, int(s[i]));
            GtkWidget* next = gtk_paned_get_child2(p);
            w = next && g_object_get_data(G_OBJECT(next), "anyui-split-chain") ? next : nullptr;
        }
    }
};

class WTabs : public W<anyui::Tabs>
{
public:
    explicit WTabs(GtkWidget* notebook)
        : W(notebook)
    {
        connect(
            w_, "switch-page",
            +[](GtkNotebook*, GtkWidget*, guint page, gpointer p)
            {
                auto* self = static_cast<WTabs*>(p);
                self->ensureBuilt(int(page));
                if (self->changed_) self->changed_(int(page));
            },
            this);
    }

    int current() const override { return gtk_notebook_get_current_page(GTK_NOTEBOOK(w_)); }
    void setCurrent(int i) override { gtk_notebook_set_current_page(GTK_NOTEBOOK(w_), i); }
    void onChanged(std::function<void(int)> f) override { changed_ = std::move(f); }

    // Lazy pages: an empty box each, filled the first time it's shown.
    void setLazy(std::vector<std::function<Node()>> lazy, std::vector<GtkWidget*> pages,
                 std::function<GtkWidget*(const Node&)> build)
    {
        lazy_ = std::move(lazy);
        pages_ = std::move(pages);
        build_ = std::move(build);
        ensureBuilt(current());
    }
    void ensureBuilt(int i)
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
    std::vector<std::function<Node()>> lazy_;
    std::vector<GtkWidget*> pages_;
    std::function<GtkWidget*(const Node&)> build_;
};

// Tabs as a list of page names down the left, the page beside it.
class WSideTabs : public W<anyui::Tabs>
{
public:
    WSideTabs(GtkWidget* box, GtkWidget* list, GtkWidget* stack)
        : W(box)
        , list_(list)
        , stack_(stack)
    {
        connect(
            list_, "row-selected",
            +[](GtkListBox*, GtkListBoxRow* row, gpointer p)
            {
                auto* self = static_cast<WSideTabs*>(p);
                if (!row) return;
                const int i = gtk_list_box_row_get_index(row);
                self->ensureBuilt(i);
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
    void setCurrent(int i) override { gtk_list_box_select_row(GTK_LIST_BOX(list_), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), i)); }
    void onChanged(std::function<void(int)> f) override { changed_ = std::move(f); }

    // Lazy pages: an empty box each, filled the first time it's shown.
    void setLazy(std::vector<std::function<Node()>> lazy, std::vector<GtkWidget*> pages,
                 std::function<GtkWidget*(const Node&)> build)
    {
        lazy_ = std::move(lazy);
        pages_ = std::move(pages);
        build_ = std::move(build);
        ensureBuilt(current());
    }
    void ensureBuilt(int i)
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
    std::vector<std::function<Node()>> lazy_;
    std::vector<GtkWidget*> pages_;
    std::function<GtkWidget*(const Node&)> build_;
};

// ---------------------------------------------------------------- list

class WList : public W<anyui::List>, public ListObserver
{
public:
    explicit WList(const spec::List& s)
        : W(gtk_tree_view_new(), gtk_scrolled_window_new(nullptr, nullptr))
    {
        rows_ = ANYUI_TREE_ROWS(g_object_new(anyui_tree_rows_get_type(), nullptr));
        auto* view = GTK_TREE_VIEW(w_);
        gtk_container_add(GTK_CONTAINER(outer_), w_);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(outer_), GTK_SHADOW_IN);
        gtk_tree_view_set_headers_visible(view, s.headers);
        gtk_tree_view_set_fixed_height_mode(view, true);
        // No rubber-band (drag-rectangle) selection: report-style lists on
        // Windows don't have it; Shift/Ctrl-click select ranges and sets.
        gtk_tree_view_set_rubber_banding(view, false);
        compact_ = s.compact;
        if (s.softSelection)
        {
            soft_ = softSelectionColors(w_);
            auto rgb = [](Color c)
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
            CompactFont font = compactFont(w_);
            compactScale_ = font.scale();
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
        gtk_tree_selection_set_mode(sel_, s.selection == SelectionMode::None ? GTK_SELECTION_NONE
                                          : s.selection == SelectionMode::Single
                                              ? GTK_SELECTION_SINGLE
                                              : GTK_SELECTION_MULTIPLE);

        for (size_t i = 0; i < s.columns.size(); ++i) addColumn(s.columns[i], i);
        auto firstText = std::ranges::find_if(s.columns, [](auto& c) { return !c.painted; });
        if (firstText != s.columns.end())
            rows_->searchColumn = guint(firstText - s.columns.begin());
        else
            gtk_tree_view_set_enable_search(view, false);
        gtk_tree_view_set_search_column(view, 0);

        attach();

        connect(
            sel_, "changed",
            +[](GtkTreeSelection*, gpointer p)
            {
                auto* self = static_cast<WList*>(p);
                if (self->selChanged_) self->selChanged_();
            },
            this);
        connect(
            w_, "row-activated",
            +[](GtkTreeView*, GtkTreePath* path, GtkTreeViewColumn*, gpointer p)
            {
                auto* self = static_cast<WList*>(p);
                if (self->activate_) self->activate_(size_t(gtk_tree_path_get_indices(path)[0]));
            },
            this);
        connect(
            w_, "button-press-event", +[](GtkWidget*, GdkEventButton* e, gpointer p) -> gboolean
            { return static_cast<WList*>(p)->buttonPressed(e); }, this);
        connect(
            w_, "motion-notify-event", +[](GtkWidget*, GdkEventMotion* e, gpointer p) -> gboolean
            { return static_cast<WList*>(p)->dragMotion(e); }, this);
        connect(
            w_, "button-release-event", +[](GtkWidget*, GdkEventButton* e, gpointer p) -> gboolean
            { return static_cast<WList*>(p)->dragReleased(e); }, this);
        connect(
            w_, "popup-menu",
            +[](GtkWidget*, gpointer p) -> gboolean
            {
                auto* self = static_cast<WList*>(p);
                return self->popup(self->cursorRow(), nullptr);
            },
            this);
        connect(
            w_, "size-allocate",
            +[](GtkWidget*, GdkRectangle* a, gpointer p)
            { static_cast<WList*>(p)->fitStretch(a->width); },
            this);
    }

    // Stretch columns share the width the others leave, once, when the list
    // first has a size; then they become fixed like the rest, so widening
    // one column never narrows another (the list scrolls sideways instead).
    void fitStretch(int width)
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

    ~WList() override
    {
        if (model_) model_->removeObserver(this);
        g_object_unref(rows_);
    }

    void setModel(ListModel* m) override
    {
        if (model_) model_->removeObserver(this);
        model_ = m;
        rows_->model = m;
        if (model_) model_->addObserver(this);
        modelReset();
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

    std::optional<size_t> current() const override { return cursorRow(); }

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

    void setCurrent(size_t row) override
    {
        if (row >= rows_->count) return;
        GtkTreePath* path = gtk_tree_path_new_from_indices(int(row), -1);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(w_), path, nullptr, false);
        gtk_tree_path_free(path);
    }

    void scrollTo(size_t row) override
    {
        if (row >= rows_->count) return;
        GtkTreePath* path = gtk_tree_path_new_from_indices(int(row), -1);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(w_), path, nullptr, false, 0, 0);
        gtk_tree_path_free(path);
    }

    void setSortIndicator(std::optional<size_t> column, SortOrder order) override
    {
        for (size_t i = 0; i < cols_.size(); ++i)
        {
            bool on = column && *column == i;
            gtk_tree_view_column_set_sort_indicator(cols_[i], on);
            if (on)
                gtk_tree_view_column_set_sort_order(cols_[i], order == SortOrder::Ascending
                                                                  ? GTK_SORT_ASCENDING
                                                                  : GTK_SORT_DESCENDING);
        }
    }

    std::vector<float> columnWidths() const override
    {
        std::vector<float> out;
        for (auto* c : cols_) out.push_back(float(gtk_tree_view_column_get_width(c)));
        return out;
    }

    void setColumnWidths(const std::vector<float>& widths) override
    {
        for (size_t i = 0; i < widths.size() && i < cols_.size(); ++i)
            if (widths[i] > 0)
            {
                gtk_tree_view_column_set_fixed_width(cols_[i], int(widths[i]));
                gtk_tree_view_column_set_expand(cols_[i], false);  // restored widths win
            }
        fitted_ = true;
    }

    void onSelectionChanged(std::function<void()> f) override { selChanged_ = std::move(f); }
    void onActivate(std::function<void(size_t)> f) override { activate_ = std::move(f); }
    void onSortRequested(std::function<void(size_t, SortOrder)> f) override
    {
        sort_ = std::move(f);
    }
    void onContextMenu(std::function<Menu(std::optional<size_t>)> f) override
    {
        contextMenu_ = std::move(f);
    }
    void onHeaderMenu(std::function<Menu(size_t)> f) override { headerMenu_ = std::move(f); }
    void onReorder(ReorderFn f) override { reorder_ = std::move(f); }
    void setReorderable(bool v) override { reorderable_ = v; }

    bool columnVisible(size_t column) const override
    {
        return column < cols_.size() && gtk_tree_view_column_get_visible(cols_[column]);
    }
    void setColumnVisible(size_t column, bool v) override
    {
        if (column < cols_.size()) gtk_tree_view_column_set_visible(cols_[column], v);
    }

    // ListObserver. Big changes detach the model: re-attaching is O(visible)
    // while per-row signals are O(rows).
    void modelReset() override
    {
        detach();
        rows_->count = model_ ? guint(model_->rowCount()) : 0;
        attach();
    }

    void rowsChanged(size_t first, size_t n) override
    {
        if (n > kBulk)
        {
            gtk_widget_queue_draw(w_);
            return;
        }
        for (size_t r = first; r < first + n && r < rows_->count; ++r)
            rowSignal(r, Change::Changed);
    }

    void rowsInserted(size_t first, size_t n) override
    {
        if (n > kBulk) return modelReset();
        rows_->count += guint(n);
        for (size_t r = first; r < first + n; ++r) rowSignal(r, Change::Inserted);
    }

    void rowsRemoved(size_t first, size_t n) override
    {
        if (n > kBulk) return modelReset();
        for (size_t i = 0; i < n; ++i)
        {
            rows_->count -= 1;
            rowSignal(first, Change::Deleted);
        }
    }

protected:
    void onDisconnect() override
    {
        alive_ = false;
        if (active_ == this) active_ = nullptr;
        menu_.destroy();
    }

private:
    static constexpr size_t kBulk = 256;

    struct ColumnCtx
    {
        WList* list;
        size_t column;
    };

    enum class Change
    {
        Changed,
        Inserted,
        Deleted
    };

    void rowSignal(size_t row, Change c)
    {
        GtkTreePath* path = gtk_tree_path_new_from_indices(int(row), -1);
        GtkTreeIter iter{};
        iter.stamp = rows_->stamp;
        iter.user_data = GUINT_TO_POINTER(guint(row));
        auto* m = GTK_TREE_MODEL(rows_);
        switch (c)
        {
        case Change::Changed: gtk_tree_model_row_changed(m, path, &iter); break;
        case Change::Inserted: gtk_tree_model_row_inserted(m, path, &iter); break;
        case Change::Deleted: gtk_tree_model_row_deleted(m, path); break;
        }
        gtk_tree_path_free(path);
    }

    void detach()
    {
        gtk_tree_view_set_model(GTK_TREE_VIEW(w_), nullptr);
        rows_->stamp = g_random_int();
    }
    void attach() { gtk_tree_view_set_model(GTK_TREE_VIEW(w_), GTK_TREE_MODEL(rows_)); }

    void addColumn(const Column& c, size_t index)
    {
        auto& ctx = ctxs_.emplace_back(new ColumnCtx{this, index});
        GtkTreeViewColumn* col = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(col, c.title.c_str());
        gtk_tree_view_column_set_resizable(col, true);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);  // fixed-height mode
        gtk_tree_view_column_set_expand(col, c.stretch);
        int width = px(c.width);
        if (width < 0)
            width = int((c.painted ? 8 : std::max<size_t>(c.title.size() + 4, 10)) *
                        g_metrics.charWidth);
        gtk_tree_view_column_set_fixed_width(col, width);
        gtk_tree_view_column_set_alignment(col, c.align == Align::Center ? 0.5f
                                                : c.align == Align::End  ? 1.0f
                                                                         : 0.0f);

        GtkCellRenderer* r;
        if (c.painted)
        {
            r = GTK_CELL_RENDERER(g_object_new(anyui_paint_renderer_get_type(), nullptr));
            auto* pr = ANYUI_PAINT_RENDERER(r);
            pr->ctx = ctx.get();
            pr->draw = &WList::paintCell;
            pr->height = int(g_metrics.lineHeight * (compact_ ? compactScale_ : 1.f) + 0.5f);
            gtk_tree_view_column_pack_start(col, r, true);
            gtk_tree_view_column_set_cell_data_func(
                col, r,
                +[](GtkTreeViewColumn*, GtkCellRenderer* r, GtkTreeModel*, GtkTreeIter* iter,
                    gpointer) { ANYUI_PAINT_RENDERER(r)->row = rowOf(iter); },
                nullptr, nullptr);
        }
        else
        {
            r = gtk_cell_renderer_text_new();
            g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xalign",
                         c.align == Align::Center ? 0.5f
                         : c.align == Align::End  ? 1.0f
                                                  : 0.0f,
                         nullptr);
            if (compact_) g_object_set(r, "ypad", 0, nullptr);
            gtk_tree_view_column_pack_start(col, r, true);
            gtk_tree_view_column_set_cell_data_func(col, r, &WList::textCell, ctx.get(), nullptr);
        }

        if (c.sortable)
        {
            gtk_tree_view_column_set_clickable(col, true);
            connect(
                col, "clicked",
                +[](GtkTreeViewColumn* col, gpointer p)
                {
                    auto* ctx = static_cast<ColumnCtx*>(p);
                    ctx->list->headerClicked(col, ctx->column);
                },
                ctx.get());
        }
        gtk_tree_view_column_set_visible(col, c.visible);
        gtk_tree_view_append_column(GTK_TREE_VIEW(w_), col);
        cols_.push_back(col);

        // The header button exists once the column is in the view.
        if (GtkWidget* header = gtk_tree_view_column_get_button(col))
            connect(
                header, "button-press-event",
                +[](GtkWidget* header, GdkEventButton* e, gpointer p) -> gboolean
                {
                    auto* ctx = static_cast<ColumnCtx*>(p);
                    if (e->type != GDK_BUTTON_PRESS || e->button != GDK_BUTTON_SECONDARY)
                        return false;
                    auto* list = ctx->list;
                    if (!list->alive_ || !list->headerMenu_) return false;
                    Menu items = list->headerMenu_(ctx->column);
                    if (items.empty()) return false;
                    list->menu_.show(std::move(items), header, reinterpret_cast<GdkEvent*>(e));
                    return true;
                },
                ctx.get());
    }

    static void textCell(GtkTreeViewColumn*, GtkCellRenderer* r, GtkTreeModel*, GtkTreeIter* iter,
                         gpointer p)
    {
        auto* ctx = static_cast<ColumnCtx*>(p);
        ListModel* model = ctx->list->model_;
        if (!model || !ctx->list->alive_) return;
        size_t row = rowOf(iter);
        TextStyle style = model->style(row, ctx->column);
        // A style background colours the whole cell, not just behind the
        // glyphs; selected rows keep the selection colour.
        std::optional<Paint> bg;
        std::swap(bg, style.bg);
        if (bg && !gtk_tree_selection_iter_is_selected(ctx->list->sel_, iter))
        {
            Color c = resolve(ctx->list->w_, *bg);
            GdkRGBA rgba{c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0};
            g_object_set(r, "cell-background-rgba", &rgba, nullptr);
        }
        else
            g_object_set(r, "cell-background-set", false, nullptr);
        PangoAttrList* attrs = pangoAttrs(ctx->list->w_, style);
        g_object_set(r, "text", model->text(row, ctx->column).c_str(), "attributes", attrs,
                     nullptr);
        if (attrs) pango_attr_list_unref(attrs);
    }

    static void paintCell(gpointer p, guint row, cairo_t* cr, GtkWidget* widget,
                          const GdkRectangle* area, GtkCellRendererState flags)
    {
        auto* ctx = static_cast<ColumnCtx*>(p);
        ListModel* model = ctx->list->model_;
        if (!model || !ctx->list->alive_ || row >= model->rowCount()) return;

        bool selected = flags & GTK_CELL_RENDERER_SELECTED;
        cairo_save(cr);
        cairo_rectangle(cr, area->x, area->y, area->width, area->height);
        cairo_clip(cr);
        cairo_translate(cr, area->x, area->y);
        if (auto bg = model->style(row, ctx->column).bg; bg && !selected)
        {
            Color c = resolve(widget, *bg);
            cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0);
            cairo_paint(cr);
        }
        auto& soft = ctx->list->soft_;
        CairoPainter painter(cr, widget,
                             selected && !soft ? std::optional<Paint>{ThemeColor::HighlightText}
                                               : std::nullopt);
        if (soft)
        {
            painter.overrideColor(ThemeColor::Highlight, soft->first);
            painter.overrideColor(ThemeColor::HighlightText, soft->second);
        }
        CellInfo info{{float(area->width), float(area->height)},
                      selected,
                      bool(flags & GTK_CELL_RENDERER_FOCUSED)};
        model->paint(row, ctx->column, painter, info);
        cairo_restore(cr);
    }

    void headerClicked(GtkTreeViewColumn* col, size_t index)
    {
        if (!sort_) return;
        bool asc = !(gtk_tree_view_column_get_sort_indicator(col) &&
                     gtk_tree_view_column_get_sort_order(col) == GTK_SORT_ASCENDING);
        sort_(index, asc ? SortOrder::Ascending : SortOrder::Descending);
    }

    std::optional<size_t> cursorRow() const
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
    struct Drag
    {
        bool pressed = false;
        bool dragging = false;
        bool deferSelect = false;  // pressed inside a multi-selection
        double x0 = 0, y0 = 0;
        size_t row = 0;
    };

    bool onBinWindow(GdkEvent* e) const
    {
        return e->any.window == gtk_tree_view_get_bin_window(GTK_TREE_VIEW(w_));
    }

    // The row the dragged rows would go in front of, and where to draw the
    // native drop line for it.
    // Works in bin-window coordinates, clamped to the visible rows, so a
    // pointer over the header (above the rows) targets the first visible row
    // rather than counting as "past the end".
    size_t dropTarget(double bx, double by)
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
        bool noop = isNoopDrop(dest);
        setDragCursor(noop ? "not-allowed" : "grabbing");
        if (count && !noop)
        {
            bool atEnd = dest >= count;
            GtkTreePath* line = gtk_tree_path_new_from_indices(int(atEnd ? count - 1 : dest), -1);
            gtk_tree_view_set_drag_dest_row(
                view, line, atEnd ? GTK_TREE_VIEW_DROP_AFTER : GTK_TREE_VIEW_DROP_BEFORE);
            gtk_tree_path_free(line);
        }
        else
            gtk_tree_view_set_drag_dest_row(view, nullptr, GTK_TREE_VIEW_DROP_BEFORE);
        return dest;
    }

public:
    // Esc during a drag cancels it, ahead of any window key handling (which
    // might close the dialog). Returns whether a drag was cancelled.
    static bool cancelActiveDrag()
    {
        WList* list = active_;
        if (!list) return false;
        list->drag_ = {};
        active_ = nullptr;
        gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(list->w_), nullptr,
                                        GTK_TREE_VIEW_DROP_BEFORE);
        list->setDragCursor(nullptr);
        return true;
    }

private:
    static inline WList* active_ = nullptr;

    bool isNoopDrop(size_t dest) const
    {
        auto sel = selection();
        if (sel.empty()) return true;
        bool contiguous = sel.back() - sel.front() + 1 == sel.size();
        return contiguous && dest >= sel.front() && dest <= sel.back() + 1;
    }

    void setDragCursor(const char* name)
    {
        GdkWindow* bin = gtk_tree_view_get_bin_window(GTK_TREE_VIEW(w_));
        GdkCursor* c = name ? gdk_cursor_new_from_name(gdk_window_get_display(bin), name) : nullptr;
        gdk_window_set_cursor(bin, c);
        if (c) g_object_unref(c);
    }

    gboolean dragPressed(GdkEventButton* e)
    {
        drag_ = {};
        if (!reorder_ || !reorderable_ || e->type != GDK_BUTTON_PRESS ||
            e->button != GDK_BUTTON_PRIMARY || !onBinWindow(reinterpret_cast<GdkEvent*>(e)) ||
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
        if (drag_.deferSelect)
        {
            gtk_widget_grab_focus(w_);
            return true;
        }
        return false;
    }

    gboolean dragMotion(GdkEventMotion* e)
    {
        if (!drag_.pressed || !(e->state & GDK_BUTTON1_MASK)) return false;
        if (!drag_.dragging)
        {
            int threshold = 8;
            g_object_get(gtk_widget_get_settings(w_), "gtk-dnd-drag-threshold", &threshold,
                         nullptr);
            if (std::abs(e->x - drag_.x0) < threshold && std::abs(e->y - drag_.y0) < threshold)
                return true;
            drag_.dragging = true;
            active_ = this;
        }
        dropTarget(e->x, e->y);

        // Scroll when the pointer nears the top or bottom edge.
        int wx, wy;
        gtk_tree_view_convert_bin_window_to_widget_coords(GTK_TREE_VIEW(w_), int(e->x), int(e->y),
                                                          &wx, &wy);
        int height = gtk_widget_get_allocated_height(w_), edge = 24;
        if (wy < edge || wy > height - edge)
            if (GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(w_)))
                gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) +
                                                  (wy < edge ? -1 : 1) * g_metrics.lineHeight);
        return true;
    }

    gboolean dragReleased(GdkEventButton* e)
    {
        if (!drag_.pressed || e->button != GDK_BUTTON_PRIMARY) return false;
        Drag d = drag_;
        drag_ = {};
        if (active_ == this) active_ = nullptr;
        if (d.dragging)
        {
            size_t dest = dropTarget(e->x, e->y);
            gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w_), nullptr, GTK_TREE_VIEW_DROP_BEFORE);
            setDragCursor(nullptr);
            if (reorder_ && !isNoopDrop(dest)) reorder_(selection(), dest);
            return true;
        }
        if (d.deferSelect)
        {
            select({d.row});
            setCurrent(d.row);
            return true;
        }
        return false;
    }

    gboolean buttonPressed(GdkEventButton* e)
    {
        if (dragPressed(e)) return true;
        if (e->type != GDK_BUTTON_PRESS || e->button != GDK_BUTTON_SECONDARY) return false;
        std::optional<size_t> row;
        GtkTreePath* path = nullptr;
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w_), int(e->x), int(e->y), &path, nullptr,
                                          nullptr, nullptr))
        {
            row = size_t(gtk_tree_path_get_indices(path)[0]);
            // Keep a multi-selection when right-clicking inside it.
            if (!gtk_tree_selection_path_is_selected(sel_, path))
            {
                gtk_tree_selection_unselect_all(sel_);
                gtk_tree_selection_select_path(sel_, path);
            }
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(w_), path, nullptr, false);
            gtk_tree_path_free(path);
        }
        popup(row, reinterpret_cast<GdkEvent*>(e));
        return true;
    }

    gboolean popup(std::optional<size_t> row, GdkEvent* event)
    {
        if (!alive_ || !contextMenu_) return false;
        Menu items = contextMenu_(row);
        if (items.empty()) return false;

        menu_.show(std::move(items), w_, event);
        return true;
    }

    AnyuiTreeRows* rows_ = nullptr;
    GtkTreeSelection* sel_ = nullptr;
    PopupMenu menu_;
    ListModel* model_ = nullptr;
    std::vector<GtkTreeViewColumn*> cols_;
    std::vector<std::unique_ptr<ColumnCtx>> ctxs_;
    bool alive_ = true;
    bool fitted_ = false;  // stretch columns have had their share of the width
    bool compact_ = false;
    std::optional<std::pair<Color, Color>> soft_;  // selection background, text
    float compactScale_ = 1;

    std::function<void()> selChanged_;
    std::function<void(size_t)> activate_;
    std::function<void(size_t, SortOrder)> sort_;
    std::function<Menu(std::optional<size_t>)> contextMenu_;
    std::function<Menu(size_t)> headerMenu_;
    ReorderFn reorder_;
    bool reorderable_ = true;
    Drag drag_;
};

// ---------------------------------------------------------------- window

// ---------------------------------------------------------------- canvas

unsigned modifiers(guint state)
{
    unsigned m = 0;
    if (state & GDK_SHIFT_MASK) m |= ModShift;
    if (state & GDK_CONTROL_MASK) m |= ModCtrl;
    if (state & GDK_MOD1_MASK) m |= ModAlt;
    if (state & (GDK_SUPER_MASK | GDK_META_MASK)) m |= ModMeta;
    return m;
}

Key keyFor(guint keyval)
{
    switch (keyval)
    {
    case GDK_KEY_Escape: return Key::Escape;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter: return Key::Enter;
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: return Key::Tab;
    case GDK_KEY_BackSpace: return Key::Backspace;
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete: return Key::Delete;
    case GDK_KEY_Insert:
    case GDK_KEY_KP_Insert: return Key::Insert;
    case GDK_KEY_space: return Key::Space;
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left: return Key::Left;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right: return Key::Right;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up: return Key::Up;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down: return Key::Down;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home: return Key::Home;
    case GDK_KEY_End:
    case GDK_KEY_KP_End: return Key::End;
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up: return Key::PageUp;
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down: return Key::PageDown;
    default: break;
    }
    if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12)
        return Key(int(Key::F1) + int(keyval - GDK_KEY_F1));
    return gdk_keyval_to_unicode(keyval) ? Key::Character : Key::Other;
}

const char* cursorName(Cursor c)
{
    switch (c)
    {
    case Cursor::Default: return "default";
    case Cursor::Pointer: return "pointer";
    case Cursor::Text: return "text";
    case Cursor::Crosshair: return "crosshair";
    case Cursor::Grab: return "grab";
    case Cursor::Grabbing: return "grabbing";
    case Cursor::ZoomIn: return "zoom-in";
    case Cursor::ZoomOut: return "zoom-out";
    case Cursor::Move: return "move";
    case Cursor::Wait: return "wait";
    case Cursor::Hidden: return "none";
    }
    return "default";
}

class WCanvas : public W<anyui::Canvas>
{
public:
    explicit WCanvas(const spec::Canvas& s)
        : W(gtk_drawing_area_new())
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
                auto* self = static_cast<WCanvas*>(p);
                if (!self->paint_) return false;
                CairoPainter painter(cr, w);
                self->paint_(painter, self->size());
                return true;
            },
            this);
        connect(
            w_, "size-allocate",
            +[](GtkWidget*, GdkRectangle* a, gpointer p)
            {
                auto* self = static_cast<WCanvas*>(p);
                if (self->resize_) self->resize_({float(a->width), float(a->height)});
            },
            this);
        connect(
            w_, "realize", +[](GtkWidget*, gpointer p) { static_cast<WCanvas*>(p)->applyCursor(); },
            this);
        connect(
            w_, "notify::scale-factor",
            +[](GObject*, GParamSpec*, gpointer p)
            {
                auto* self = static_cast<WCanvas*>(p);
                if (self->resize_) self->resize_(self->size());
            },
            this);
        connect(w_, "button-press-event", &WCanvas::button, this);
        connect(w_, "button-release-event", &WCanvas::button, this);
        connect(
            w_, "motion-notify-event",
            +[](GtkWidget*, GdkEventMotion* e, gpointer p) -> gboolean
            {
                return static_cast<WCanvas*>(p)->pointer({PointerEvent::Type::Move,
                                                          {float(e->x), float(e->y)},
                                                          0,
                                                          1,
                                                          modifiers(e->state)});
            },
            this);
        connect(w_, "enter-notify-event", &WCanvas::crossing, this);
        connect(w_, "leave-notify-event", &WCanvas::crossing, this);
        connect(
            w_, "scroll-event",
            +[](GtkWidget*, GdkEventScroll* e, gpointer p) -> gboolean
            {
                auto* self = static_cast<WCanvas*>(p);
                // With the smooth mask, GTK also emulates direction events
                // from smooth ones; take each motion once.
                if (!self->scroll_ || gdk_event_get_pointer_emulated((GdkEvent*)e)) return false;
                ScrollEvent ev{{float(e->x), float(e->y)}, 0, 0, false, modifiers(e->state)};
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
        connect(w_, "key-press-event", &WCanvas::keyEvent, this);
        connect(w_, "key-release-event", &WCanvas::keyEvent, this);
    }

    ~WCanvas() override { onDisconnect(); }

    void invalidate() override { gtk_widget_queue_draw(w_); }
    SizeF size() const override
    {
        return {float(gtk_widget_get_allocated_width(w_)),
                float(gtk_widget_get_allocated_height(w_))};
    }
    float deviceScale() const override { return float(gtk_widget_get_scale_factor(w_)); }
    void setCursor(Cursor c) override
    {
        cursor_ = c;
        applyCursor();
    }

    void onPaint(std::function<void(Painter&, SizeF)> f) override { paint_ = std::move(f); }
    void onResize(std::function<void(SizeF)> f) override { resize_ = std::move(f); }
    void onPointer(std::function<bool(const PointerEvent&)> f) override { pointer_ = std::move(f); }
    void onScroll(std::function<bool(const ScrollEvent&)> f) override { scroll_ = std::move(f); }
    void onKey(std::function<bool(const KeyEvent&)> f) override { key_ = std::move(f); }

    Subscription onFrame(std::function<void(double)> f) override
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
        return Subscription(
            [this, alive = alive_, id]
            {
                if (!*alive) return;  // the widget went first; GTK dropped its ticks
                gtk_widget_remove_tick_callback(w_, id);
                std::erase(ticks_, id);
            });
    }

protected:
    void onDisconnect() override
    {
        if (!*alive_) return;
        *alive_ = false;
        for (guint id : ticks_) gtk_widget_remove_tick_callback(w_, id);
        ticks_.clear();
    }

private:
    bool pointer(const PointerEvent& e) { return pointer_ && pointer_(e); }

    static gboolean button(GtkWidget*, GdkEventButton* e, gpointer p)
    {
        auto* self = static_cast<WCanvas*>(p);
        // GTK adds 2BUTTON/3BUTTON events after the presses; count clicks
        // ourselves instead so each press arrives once.
        if (e->type != GDK_BUTTON_PRESS && e->type != GDK_BUTTON_RELEASE) return true;
        PointerEvent ev{e->type == GDK_BUTTON_PRESS ? PointerEvent::Type::Press
                                                    : PointerEvent::Type::Release,
                        {float(e->x), float(e->y)},
                        int(e->button),
                        1,
                        modifiers(e->state)};
        if (ev.type == PointerEvent::Type::Press)
        {
            if (self->focusable_ && !gtk_widget_has_focus(self->w_))
                gtk_widget_grab_focus(self->w_);
            int time = 400, distance = 5;
            g_object_get(gtk_widget_get_settings(self->w_), "gtk-double-click-time", &time,
                         "gtk-double-click-distance", &distance, nullptr);
            const bool again = ev.button == self->lastButton_ &&
                               e->time - self->lastTime_ <= guint32(time) &&
                               std::abs(ev.pos.x - self->lastPos_.x) <= distance &&
                               std::abs(ev.pos.y - self->lastPos_.y) <= distance;
            self->clicks_ = again ? self->clicks_ + 1 : 1;
            ev.clicks = self->clicks_;
            self->lastButton_ = ev.button;
            self->lastTime_ = e->time;
            self->lastPos_ = ev.pos;
        }
        return self->pointer(ev);
    }

    static gboolean crossing(GtkWidget*, GdkEventCrossing* e, gpointer p)
    {
        auto* self = static_cast<WCanvas*>(p);
        return self->pointer(
            {e->type == GDK_ENTER_NOTIFY ? PointerEvent::Type::Enter : PointerEvent::Type::Leave,
             {float(e->x), float(e->y)},
             0,
             1,
             modifiers(e->state)});
    }

    static gboolean keyEvent(GtkWidget*, GdkEventKey* e, gpointer p)
    {
        auto* self = static_cast<WCanvas*>(p);
        if (!self->key_) return false;
        KeyEvent ev;
        ev.press = e->type == GDK_KEY_PRESS;
        ev.key = keyFor(e->keyval);
        if (ev.key == Key::Character)
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

    void applyCursor()
    {
        GdkWindow* win = gtk_widget_get_window(w_);
        if (!win) return;  // applied on realize
        GdkCursor* c =
            cursor_ == Cursor::Default
                ? nullptr
                : gdk_cursor_new_from_name(gtk_widget_get_display(w_), cursorName(cursor_));
        gdk_window_set_cursor(win, c);
        if (c) g_object_unref(c);
    }

    std::function<void(Painter&, SizeF)> paint_;
    std::function<void(SizeF)> resize_;
    std::function<bool(const PointerEvent&)> pointer_;
    std::function<bool(const ScrollEvent&)> scroll_;
    std::function<bool(const KeyEvent&)> key_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::vector<guint> ticks_;
    std::vector<guint16> held_;
    Cursor cursor_ = Cursor::Default;
    bool focusable_ = true;
    int clicks_ = 0, lastButton_ = 0;
    guint32 lastTime_ = 0;
    PointF lastPos_;
};

class WWindow : public anyui::Window, public Part
{
    // Development aid: ANYUI_SNAPSHOT=file.png saves the window shown last,
    // ANYUI_SNAPSHOT_DELAY ms (default 500) after it appears, then quits
    // the main loop. For checking layouts and canvases without a screen
    // grabber.
    // The most recently shown window is the one a snapshot captures (a
    // dialog over the main window, say).
    static GtkWidget*& lastShown()
    {
        static GtkWidget* w = nullptr;
        return w;
    }

    void snapshotIfAsked()
    {
        lastShown() = win_;
        static bool taken = false;
        const char* path = std::getenv("ANYUI_SNAPSHOT");
        if (taken || !path || !*path) return;
        taken = true;
        const char* delay = std::getenv("ANYUI_SNAPSHOT_DELAY");
        g_timeout_add(
            delay ? guint(std::atoi(delay)) : 500u,
            +[](gpointer) -> gboolean
            {
                GtkWidget* win = lastShown();
                if (!win) return G_SOURCE_REMOVE;
                const int scale = gtk_widget_get_scale_factor(win);
                const int w = gtk_widget_get_allocated_width(win);
                const int h = gtk_widget_get_allocated_height(win);
                cairo_surface_t* surface =
                    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * scale, h * scale);
                cairo_surface_set_device_scale(surface, scale, scale);
                cairo_t* cr = cairo_create(surface);
                gtk_widget_draw(win, cr);
                cairo_destroy(cr);
                cairo_surface_write_to_png(surface, std::getenv("ANYUI_SNAPSHOT"));
                cairo_surface_destroy(surface);
                gtk_main_quit();
                return G_SOURCE_REMOVE;
            },
            nullptr);
    }

public:
    WWindow(const WindowSpec& spec, WWindow* parent)
        : win_(gtk_window_new(GTK_WINDOW_TOPLEVEL))
        , dialog_(spec.dialog)
    {
        auto* win = GTK_WINDOW(win_);
        gtk_window_set_title(win, spec.title.c_str());
        // Snapshot runs are automated: never take the user's keyboard focus.
        if (const char* snap = std::getenv("ANYUI_SNAPSHOT"); snap && *snap)
        {
            gtk_window_set_accept_focus(win, false);
            gtk_window_set_focus_on_map(win, false);
        }
        gtk_window_set_default_size(win, px(spec.width), px(spec.height));
        if (parent) gtk_window_set_transient_for(win, GTK_WINDOW(parent->win_));
        // First in line, so Esc ends a list drag rather than the dialog.
        connect(
            win_, "key-press-event", +[](GtkWidget*, GdkEventKey* e, gpointer) -> gboolean
            { return e->keyval == GDK_KEY_Escape && WList::cancelActiveDrag(); }, this);
        if (dialog_)
        {
            gtk_window_set_modal(win, parent != nullptr);
            gtk_window_set_type_hint(win, GDK_WINDOW_TYPE_HINT_DIALOG);
            connect(
                win_, "key-press-event",
                +[](GtkWidget*, GdkEventKey* e, gpointer p) -> gboolean
                {
                    if (e->keyval != GDK_KEY_Escape) return false;
                    static_cast<WWindow*>(p)->closeRequested();
                    return true;
                },
                this);
        }
        connect(
            win_, "delete-event",
            +[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean
            {
                static_cast<WWindow*>(p)->closeRequested();
                return true;  // we own the window; never let GTK destroy it
            },
            this);
        connect(
            win_, "window-state-event",
            +[](GtkWidget*, GdkEventWindowState* e, gpointer p) -> gboolean
            {
                auto* self = static_cast<WWindow*>(p);
                const bool fs = e->new_window_state & GDK_WINDOW_STATE_FULLSCREEN;
                if (fs != self->fullscreen_)
                {
                    self->fullscreen_ = fs;
                    self->showMenuBar();
                    if (self->onFullscreen_) self->onFullscreen_(fs);
                }
                return false;
            },
            this);
        // Runs before GTK hands the key to the focused widget (key-press-event
        // is RUN_LAST, so this handler precedes the window's default one).
        auto key = +[](GtkWidget* w, GdkEventKey* e, gpointer p) -> gboolean
        {
            auto* self = static_cast<WWindow*>(p);
            if (e->keyval == GDK_KEY_F10 && e->type == GDK_KEY_PRESS)
                self->rebuildMenuBar();  // opens it
            if (!self->onKey_) return false;
            GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(w));
            const bool typing =
                focus &&
                (GTK_IS_EDITABLE(focus) ||
                 (GTK_IS_TEXT_VIEW(focus) && gtk_text_view_get_editable(GTK_TEXT_VIEW(focus))));
            const bool notText = e->keyval == GDK_KEY_Escape ||
                                 (e->keyval >= GDK_KEY_F1 && e->keyval <= GDK_KEY_F12);
            if (typing && !notText) return false;
            KeyEvent ev;
            ev.press = e->type == GDK_KEY_PRESS;
            ev.key = keyFor(e->keyval);
            if (ev.key == Key::Character)
                ev.codepoint = gdk_keyval_to_unicode(gdk_keyval_to_lower(e->keyval));
            ev.modifiers = modifiers(e->state);
            return self->onKey_(ev);
        };
        connect(win_, "key-press-event", key, this);
        connect(win_, "key-release-event", key, this);
    }

    ~WWindow() override
    {
        if (lastShown() == win_) lastShown() = nullptr;
        for (auto& part : parts) part->disconnect();
        disconnect();
        gtk_widget_destroy(win_);
    }

    void setTitle(const std::string& t) override
    {
        gtk_window_set_title(GTK_WINDOW(win_), t.c_str());
    }
    void show() override
    {
        finished_ = false;
        gtk_window_present(GTK_WINDOW(win_));
        snapshotIfAsked();
    }
    void close() override { gtk_widget_hide(win_); }
    void onFinished(std::function<void(DialogRole)> f) override { onFinished_ = std::move(f); }
    void onCloseRequest(std::function<bool()> f) override { onCloseRequest_ = std::move(f); }

    SizeF size() const override
    {
        int w = 0, h = 0;
        gtk_window_get_size(GTK_WINDOW(win_), &w, &h);
        return {float(w), float(h)};
    }
    void resize(SizeF s) override
    {
        gtk_window_resize(GTK_WINDOW(win_), std::max(1, int(s.w)), std::max(1, int(s.h)));
    }
    void setMinSize(SizeF s) override
    {
        GdkGeometry g{};
        g.min_width = int(s.w);
        g.min_height = int(s.h);
        gtk_window_set_geometry_hints(GTK_WINDOW(win_), nullptr, &g, GDK_HINT_MIN_SIZE);
    }
    std::optional<PointF> position() const override
    {
        if (!placeable()) return std::nullopt;
        int x = 0, y = 0;
        gtk_window_get_position(GTK_WINDOW(win_), &x, &y);
        return PointF{float(x), float(y)};
    }
    void move(PointF p) override
    {
        if (placeable()) gtk_window_move(GTK_WINDOW(win_), int(p.x), int(p.y));
    }
    RectF workArea() const override
    {
        GdkDisplay* d = gtk_widget_get_display(win_);
        GdkWindow* gw = gtk_widget_get_window(win_);
        GdkMonitor* m = gw ? gdk_display_get_monitor_at_window(d, gw) : nullptr;
        if (!m) m = gdk_display_get_primary_monitor(d);
        if (!m) m = gdk_display_get_monitor(d, 0);
        if (!m) return {};
        GdkRectangle r;
        gdk_monitor_get_workarea(m, &r);
        return {float(r.x), float(r.y), float(r.width), float(r.height)};
    }

    bool fullscreen() const override { return fullscreen_; }
    void setFullscreen(bool on) override
    {
        if (on)
            gtk_window_fullscreen(GTK_WINDOW(win_));
        else
            gtk_window_unfullscreen(GTK_WINDOW(win_));
    }
    void onFullscreenChanged(std::function<void(bool)> f) override { onFullscreen_ = std::move(f); }

    void setIcon(const Icon& icon) override
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
            CairoPainter painter(cr, win_);
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

    void onKey(std::function<bool(const KeyEvent&)> f) override { onKey_ = std::move(f); }

    void setContent(GtkWidget* content, bool margins, const std::vector<MenuBarItem>& menus)
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
            gtk_box_pack_start(GTK_BOX(top), buildMenuBar(menus), false, false, 0);
            gtk_box_pack_start(GTK_BOX(top), content, true, true, 0);
        }
        gtk_container_add(GTK_CONTAINER(win_), top);
        // GTK 3 widgets start hidden; widgets the app hides later stay hidden.
        gtk_widget_show_all(top);
    }

    void setMenuBarVisible(bool on) override
    {
        menuBarWanted_ = on;
        showMenuBar();
    }

private:
    // A menu bar's menus are rebuilt whenever the bar is entered (a click
    // on it, an access key, F10), so they show the app's state as it is.
    // Nothing can change that state while a menu is open but its items,
    // and choosing one closes the menus.
    struct BarMenu
    {
        MenuBarItem item;
        GtkWidget* menu = nullptr;  // the submenu, refilled in place
        Menu current;               // what its GTK items point into
    };

    GtkWidget* buildMenuBar(const std::vector<MenuBarItem>& menus)
    {
        menuBar_ = gtk_menu_bar_new();
        for (const MenuBarItem& m : menus)
        {
            auto bar = std::make_unique<BarMenu>(BarMenu{m, nullptr, {}});
            GtkWidget* top = gtk_menu_item_new_with_mnemonic(m.title.c_str());
            bar->menu = gtk_menu_new();
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), bar->menu);
            gtk_menu_shell_append(GTK_MENU_SHELL(menuBar_), top);
            connect(
                top, "mnemonic-activate",
                +[](GtkWidget*, gboolean, gpointer p) -> gboolean
                {
                    static_cast<WWindow*>(p)->rebuildMenuBar();
                    return false;
                },
                this);
            bars_.push_back(std::move(bar));
        }
        connect(
            menuBar_, "button-press-event",
            +[](GtkWidget*, GdkEventButton*, gpointer p) -> gboolean
            {
                static_cast<WWindow*>(p)->rebuildMenuBar();
                return false;
            },
            this);
        rebuildMenuBar();
        return menuBar_;
    }

    void rebuildMenuBar()
    {
        for (auto& bar : bars_)
        {
            GList* old = gtk_container_get_children(GTK_CONTAINER(bar->menu));
            for (GList* l = old; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
            g_list_free(old);
            bar->current = bar->item.menu ? bar->item.menu() : Menu{};
            fillMenu(bar->menu, bar->current);
            gtk_widget_show_all(bar->menu);
        }
    }

    void showMenuBar()
    {
        if (menuBar_) gtk_widget_set_visible(menuBar_, menuBarWanted_ && !fullscreen_);
    }

    GtkWidget* menuBar_ = nullptr;
    std::vector<std::unique_ptr<BarMenu>> bars_;
    bool menuBarWanted_ = true;

public:
    void addRoleButton(GtkWidget* button, DialogRole role)
    {
        g_object_set_data(G_OBJECT(button), "anyui-role", GINT_TO_POINTER(int(role)));
        connect(
            button, "clicked",
            +[](GtkButton* b, gpointer p)
            {
                auto role =
                    DialogRole(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "anyui-role")));
                static_cast<WWindow*>(p)->finish(role);
            },
            this);
        if (role == DialogRole::Ok || role == DialogRole::Yes)
        {
            gtk_style_context_add_class(gtk_widget_get_style_context(button), "suggested-action");
            gtk_widget_set_can_default(button, true);
            gtk_window_set_default(GTK_WINDOW(win_), button);
        }
    }

    GtkWidget* gtkWindow() const { return win_; }

    std::vector<std::unique_ptr<Part>> parts;

private:
    // Only X11 lets clients place their windows; Wayland's compositor does.
    // (By type name: <gdk/gdkx.h> would bring Xlib's macros, e.g. None.)
    bool placeable() const
    {
        GdkDisplay* d = gtk_widget_get_display(win_);
        return std::strcmp(G_OBJECT_TYPE_NAME(d), "GdkX11Display") == 0;
    }

    void closeRequested()
    {
        if (onCloseRequest_ && !onCloseRequest_()) return;
        if (dialog_)
            finish(DialogRole::Cancel);
        else
            close();
    }

    // The callback may delete this window, so it runs last.
    void finish(DialogRole role)
    {
        close();
        if (finished_) return;
        finished_ = true;
        if (auto f = onFinished_) f(role);
    }

    GtkWidget* win_;
    bool dialog_;
    bool finished_ = false;
    std::function<void(DialogRole)> onFinished_;
    std::function<bool()> onCloseRequest_;
    std::function<void(bool)> onFullscreen_;
    std::function<bool(const KeyEvent&)> onKey_;
    bool fullscreen_ = false;
};

// ---------------------------------------------------------------- builder

void setMargins(GtkWidget* w, int m)
{
    gtk_widget_set_margin_start(w, m);
    gtk_widget_set_margin_end(w, m);
    gtk_widget_set_margin_top(w, m);
    gtk_widget_set_margin_bottom(w, m);
}

class Builder
{
public:
    Builder(WWindow& win, BuildSink& sink)
        : win_(win)
        , sink_(sink)
    {
    }

    GtkWidget* build(const Node& n, Orientation parentDir)
    {
        GtkWidget* w = std::visit([&](const auto& s) { return make(n, s, parentDir); }, n.spec);
        // Containers have no interface; one with a handle gets a plain Widget.
        const bool container = std::holds_alternative<spec::Box>(n.spec) ||
                               std::holds_alternative<spec::Form>(n.spec) ||
                               std::holds_alternative<spec::Group>(n.spec) ||
                               std::holds_alternative<spec::Scroll>(n.spec) ||
                               std::holds_alternative<spec::Toolbar>(n.spec);
        if (container && n.common.handle) add<WPlain>(n, w);
        int width = px(n.common.width), height = px(n.common.height);
        if (width > 0 || height > 0) gtk_widget_set_size_request(w, width, height);
        if (n.common.stretch > 0)
        {
            if (parentDir == Orientation::Vertical)
                gtk_widget_set_vexpand(w, true);
            else
                gtk_widget_set_hexpand(w, true);
        }
        return w;
    }

private:
    static bool isLineControl(const Spec& s)
    {
        return std::holds_alternative<spec::Label>(s) || std::holds_alternative<spec::Button>(s) ||
               std::holds_alternative<spec::Check>(s) || std::holds_alternative<spec::Entry>(s) ||
               std::holds_alternative<spec::Select>(s) || std::holds_alternative<spec::Combo>(s) ||
               std::holds_alternative<spec::MenuButton>(s) ||
               std::holds_alternative<spec::ProgressBar>(s) ||
               std::holds_alternative<spec::DatePicker>(s);
    }

    static bool fillsFormRow(const Spec& s)
    {
        return std::holds_alternative<spec::Entry>(s) || std::holds_alternative<spec::Combo>(s) ||
               std::holds_alternative<spec::TextView>(s) || std::holds_alternative<spec::List>(s) ||
               std::holds_alternative<spec::Canvas>(s) || std::holds_alternative<spec::Box>(s) ||
               std::holds_alternative<spec::Form>(s) || std::holds_alternative<spec::Splitter>(s) ||
               std::holds_alternative<spec::Group>(s) || std::holds_alternative<spec::Scroll>(s) ||
               std::holds_alternative<spec::Tabs>(s);
    }

    template <class T, class... Args>
    T& add(const Node& n, Args&&... args)
    {
        auto* w = new T(std::forward<Args>(args)...);
        win_.parts.emplace_back(w);
        sink_.created(n, *w);
        return *w;
    }

    GtkWidget* make(const Node&, const spec::Box& s, Orientation)
    {
        GtkWidget* box = gtk_box_new(gtkOrientation(s.dir), 6);
        for (auto& c : s.items)
        {
            GtkWidget* w = build(c, s.dir);
            // Single-line controls keep their natural height when a taller
            // sibling (a stack of checkboxes) sets the row height.
            if (s.dir == Orientation::Horizontal && isLineControl(c.spec))
                gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
            gtk_container_add(GTK_CONTAINER(box), w);
        }
        if (s.margins)
        {
            // A bar along a window edge: the full margin at its ends, half along its sides.
            const bool row = s.dir == Orientation::Horizontal;
            gtk_widget_set_margin_start(box, row ? 6 : 3);
            gtk_widget_set_margin_end(box, row ? 6 : 3);
            gtk_widget_set_margin_top(box, row ? 3 : 6);
            gtk_widget_set_margin_bottom(box, row ? 3 : 6);
        }
        return box;
    }

    GtkWidget* make(const Node&, const spec::Form& s, Orientation)
    {
        GtkWidget* grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
        for (size_t i = 0; i < s.fields.size(); ++i)
        {
            GtkWidget* field = build(s.fields[i], Orientation::Horizontal);
            // Text fields and containers fill the row; spins, selects, colour
            // buttons and the like keep their natural width (as in Windows
            // dialogs) unless the node asks to stretch.
            if (fillsFormRow(s.fields[i].spec))
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
            gtk_grid_attach(GTK_GRID(grid), label, 0, int(i), 1, 1);
            gtk_grid_attach(GTK_GRID(grid), field, 1, int(i), 1, 1);
        }
        return grid;
    }

    GtkWidget* make(const Node& n, const spec::Splitter& s, Orientation)
    {
        if (s.panes.empty()) return gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget* paned = panes(s, 0);
        if (GTK_IS_PANED(paned))
        {
            add<WSplitter>(n, paned);
            initialSplit(s, paned);
        }
        return paned;
    }

    // GtkPaned's first split follows natural sizes, which mean little for
    // lists and text views. On the first allocation, panes without stretch
    // get their natural size and the rest is shared by stretch factor.
    static void initialSplit(const spec::Splitter& s, GtkWidget* top)
    {
        struct Split
        {
            bool vertical;
            std::vector<GtkPaned*> paneds;
            std::vector<GtkWidget*> panes;
            std::vector<int> stretch;
            bool scheduled = false;
        };
        auto* split = new Split{s.dir == Orientation::Vertical, {}, {}, {}};
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
            G_OBJECT(top), "anyui-initial-split", split,
            +[](gpointer p) { delete static_cast<Split*>(p); });

        g_signal_connect(top, "size-allocate",
                         G_CALLBACK(+[](GtkWidget* top, GdkRectangle* alloc, gpointer p)
                                    {
                                        auto* split = static_cast<Split*>(p);
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
                                                auto* split = static_cast<Split*>(g_object_get_data(
                                                    G_OBJECT(top), "anyui-initial-split"));
                                                if (split && !g_object_get_data(G_OBJECT(top),
                                                                                "anyui-user-sized"))
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

        int fixed = 0, stretchSum = 0;
        std::vector<int> natural(split.panes.size());
        for (size_t i = 0; i < split.panes.size(); ++i)
        {
            if (split.stretch[i] > 0)
            {
                stretchSum += split.stretch[i];
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
        if (avail <= 0 || stretchSum == 0) return;
        for (size_t i = 0; i < split.paneds.size(); ++i)
        {
            int size = split.stretch[i] > 0 ? avail * split.stretch[i] / stretchSum : natural[i];
            gtk_paned_set_position(split.paneds[i], size);
        }
    }

    // GtkPaned holds two children; more panes nest to the right/bottom.
    GtkWidget* panes(const spec::Splitter& s, size_t from)
    {
        GtkWidget* first = build(s.panes[from], s.dir);
        if (from + 1 == s.panes.size()) return first;

        bool anyStretch =
            std::ranges::any_of(s.panes, [](auto& p) { return p.common.stretch > 0; });
        GtkWidget* paned = gtk_paned_new(gtkOrientation(s.dir));
        auto* p = GTK_PANED(paned);
        gtk_paned_set_wide_handle(p, true);
        g_object_set_data(G_OBJECT(paned), "anyui-split-chain", GINT_TO_POINTER(1));
        gtk_paned_pack1(p, first, !anyStretch || s.panes[from].common.stretch > 0, false);
        gtk_paned_pack2(p, panes(s, from + 1), true, false);
        return paned;
    }

    GtkWidget* make(const Node&, const spec::Scroll& s, Orientation)
    {
        GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                       s.horizontal ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller), true);
        gtk_widget_set_vexpand(scroller, true);
        if (!s.content.empty())
            gtk_container_add(GTK_CONTAINER(scroller),
                              build(s.content.front(), Orientation::Vertical));
        return scroller;
    }

    GtkWidget* make(const Node&, const spec::Group& s, Orientation)
    {
        GtkWidget* frame = gtk_frame_new(s.title.c_str());
        if (!s.content.empty())
        {
            GtkWidget* child = build(s.content.front(), Orientation::Vertical);
            setMargins(child, 6);
            gtk_container_add(GTK_CONTAINER(frame), child);
        }
        return frame;
    }

    GtkWidget* make(const Node& n, const spec::Tabs& s, Orientation)
    {
        if (s.sidebar) return makeSidebar(n, s);
        GtkWidget* nb = gtk_notebook_new();
        std::vector<GtkWidget*> pages;
        for (size_t i = 0; i < s.pages.size(); ++i)
        {
            const bool lazy = i < s.lazy.size() && s.lazy[i];
            GtkWidget* page = lazy ? gtk_box_new(GTK_ORIENTATION_VERTICAL, 0) : build(s.pages[i], Orientation::Vertical);
            setMargins(page, 6);
            gtk_notebook_append_page(GTK_NOTEBOOK(nb), page,
                                     gtk_label_new_with_mnemonic(s.titles[i].c_str()));
            pages.push_back(page);
        }
        auto& tabs = add<WTabs>(n, nb);
        if (!s.lazy.empty())
            tabs.setLazy(s.lazy, std::move(pages),
                         [&win = win_, &sink = sink_](const Node& page) { return Builder(win, sink).build(page, Orientation::Vertical); });
        return nb;
    }

    // Tabs as a list of page names down the left, the page in a stack beside it.
    GtkWidget* makeSidebar(const Node& n, const spec::Tabs& s)
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
            GtkWidget* page = lazy ? gtk_box_new(GTK_ORIENTATION_VERTICAL, 0) : build(s.pages[i], Orientation::Vertical);
            setMargins(page, 6);
            gtk_stack_add_named(GTK_STACK(stack), page, std::to_string(i).c_str());
            pages.push_back(page);
        }
        gtk_box_pack_start(GTK_BOX(box), list, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), stack, TRUE, TRUE, 0);
        auto& tabs = add<WSideTabs>(n, box, list, stack);
        tabs.setCurrent(0);
        if (!s.lazy.empty())
            tabs.setLazy(s.lazy, std::move(pages),
                         [&win = win_, &sink = sink_](const Node& page) { return Builder(win, sink).build(page, Orientation::Vertical); });
        return box;
    }

    GtkWidget* make(const Node&, const spec::Spacer&, Orientation)
    {
        return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    }

    GtkWidget* make(const Node&, const spec::Separator&, Orientation parentDir)
    {
        return gtk_separator_new(parentDir == Orientation::Vertical ? GTK_ORIENTATION_HORIZONTAL
                                                                    : GTK_ORIENTATION_VERTICAL);
    }

    GtkWidget* make(const Node& n, const spec::Label& s, Orientation)
    {
        return add<WLabel>(n, s).outer();
    }

    GtkWidget* make(const Node& n, const spec::MenuButton& s, Orientation)
    {
        return add<WMenuButton>(n, s).outer();
    }

    GtkWidget* make(const Node& n, const spec::ProgressBar&, Orientation)
    {
        return add<WProgressBar>(n).outer();
    }

    GtkWidget* make(const Node& n, const spec::DatePicker& s, Orientation)
    {
        return add<WDatePicker>(n, s).outer();
    }

    GtkWidget* make(const Node& n, const spec::Button& s, Orientation)
    {
        auto& b = add<WButton>(n, s.iconOnly ? std::string() : s.text);
        if (s.icon) b.setIcon(s.icon);
        if (s.iconOnly && n.common.tooltip.empty()) b.setTooltip(s.text);
        if (s.flat) gtk_button_set_relief(GTK_BUTTON(b.outer()), GTK_RELIEF_NONE);
        return b.outer();
    }

    GtkWidget* make(const Node& n, const spec::ToolButton& s, Orientation)
    {
        return add<WToolButton>(n, s).outer();
    }

    // Items: ToolButtons, Separator, Spacer (pushes later items to the end),
    // or any other widget, held in a GtkToolItem.
    GtkWidget* make(const Node&, const spec::Toolbar& s, Orientation)
    {
        GtkWidget* bar = gtk_toolbar_new();
        gtk_toolbar_set_style(GTK_TOOLBAR(bar), GTK_TOOLBAR_ICONS);
        for (const Node& item : s.items)
        {
            GtkToolItem* ti = nullptr;
            if (std::holds_alternative<spec::Separator>(item.spec))
            {
                ti = gtk_separator_tool_item_new();
            }
            else if (std::holds_alternative<spec::Spacer>(item.spec))
            {
                ti = gtk_separator_tool_item_new();
                gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(ti), false);
                gtk_tool_item_set_expand(ti, true);
            }
            else if (std::holds_alternative<spec::ToolButton>(item.spec))
            {
                ti = GTK_TOOL_ITEM(build(item, Orientation::Horizontal));
            }
            else
            {
                ti = gtk_tool_item_new();
                GtkWidget* w = build(item, Orientation::Horizontal);
                gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
                gtk_container_add(GTK_CONTAINER(ti), w);
                if (item.common.stretch > 0) gtk_tool_item_set_expand(ti, true);
            }
            gtk_toolbar_insert(GTK_TOOLBAR(bar), ti, -1);
        }
        return bar;
    }

    GtkWidget* make(const Node& n, const spec::Slider& s, Orientation)
    {
        return add<WSlider>(n, s).outer();
    }

    GtkWidget* make(const Node& n, const spec::Spin& s, Orientation)
    {
        return add<WSpin>(n, s).outer();
    }

    GtkWidget* make(const Node& n, const spec::ColorButton& s, Orientation)
    {
        return add<WColorButton>(n, s).outer();
    }

    GtkWidget* make(const Node& n, const spec::Check& s, Orientation)
    {
        return add<WCheck>(n, s).outer();
    }

    GtkWidget* make(const Node& n, const spec::Entry& s, Orientation)
    {
        return add<WEntry>(n, s).outer();
    }

    GtkWidget* make(const Node& n, const spec::Select& s, Orientation)
    {
        return add<WSelect>(n, s).outer();
    }

    GtkWidget* make(const Node& n, const spec::Combo& s, Orientation)
    {
        return add<WCombo>(n, s).outer();
    }

    GtkWidget* make(const Node& n, const spec::TextView& s, Orientation)
    {
        return add<WTextView>(n, s).outer();
    }

    GtkWidget* make(const Node& n, const spec::List& s, Orientation)
    {
        return add<WList>(n, s).outer();
    }

    GtkWidget* make(const Node& n, const spec::Canvas& s, Orientation)
    {
        return add<WCanvas>(n, s).outer();
    }

    GtkWidget* make(const Node&, const spec::Buttons& s, Orientation)
    {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_top(box, 6);
        auto ordered = s.buttons;
        std::ranges::stable_sort(ordered, {}, [](auto& b) { return gnomeOrder(b.role); });
        bool spaced = false;
        for (auto& b : ordered)
        {
            // Help sits at the far start, the rest at the end.
            if (!spaced && b.role != DialogRole::Help)
            {
                GtkWidget* gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
                gtk_widget_set_hexpand(gap, true);
                gtk_container_add(GTK_CONTAINER(box), gap);
                spaced = true;
            }
            auto* w = new WButton(b.text.empty() ? stockLabel(b.role) : b.text);
            win_.parts.emplace_back(w);
            if (!b.onClick) win_.addRoleButton(w->outer(), b.role);
            sink_.created(b, *w);
            gtk_container_add(GTK_CONTAINER(box), w->outer());
        }
        return box;
    }

    WWindow& win_;
    BuildSink& sink_;
};

// ---------------------------------------------------------------- backend

class Gtk3Backend : public Backend
{
public:
    static Backend* create(int* argc, char** argv)
    {
        if (!gtk_init_check(argc, &argv)) return nullptr;
        initMetrics();
        return new Gtk3Backend;
    }

    std::string_view name() const override { return "gtk3"; }

    std::unique_ptr<anyui::Window> createWindow(const WindowSpec& spec, anyui::Window* parent,
                                                BuildSink& sink) override
    {
        auto win = std::make_unique<WWindow>(spec, static_cast<WWindow*>(parent));
        win->setContent(Builder(*win, sink).build(spec.content, Orientation::Vertical),
                        spec.margins, spec.menuBar);
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

    std::unique_ptr<anyui::Image> createImage(int w, int h, PixelFormat format,
                                              const uint8_t* pixels, size_t stride) override
    {
        return std::make_unique<CairoImage>(w, h, format, pixels, stride);
    }

    Subscription timer(int ms, bool repeat, std::function<void()> fn) override
    {
        struct State
        {
            std::function<void()> fn;
            bool repeat;
            guint id = 0;
        };
        auto state = std::make_shared<State>(State{std::move(fn), repeat});
        state->id = g_timeout_add_full(
            G_PRIORITY_DEFAULT, guint(std::max(ms, 0)),
            +[](gpointer p) -> gboolean
            {
                auto state =
                    *static_cast<std::shared_ptr<State>*>(p);  // fn may drop the subscription
                const bool again = state->repeat;
                if (!again) state->id = 0;
                state->fn();
                return again && state->id ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
            },
            new std::shared_ptr<State>(state),
            +[](gpointer p) { delete static_cast<std::shared_ptr<State>*>(p); });
        return Subscription(
            [state]
            {
                if (state->id) g_source_remove(std::exchange(state->id, 0));
            });
    }

    void message(anyui::Window* parent, const MessageSpec& m,
                 std::function<void(DialogRole)> done) override
    {
        GtkMessageType type = GTK_MESSAGE_INFO;
        switch (m.icon)
        {
        case MessageSpec::Icon::Info: type = GTK_MESSAGE_INFO; break;
        case MessageSpec::Icon::Warning: type = GTK_MESSAGE_WARNING; break;
        case MessageSpec::Icon::Error: type = GTK_MESSAGE_ERROR; break;
        case MessageSpec::Icon::Question: type = GTK_MESSAGE_QUESTION; break;
        }
        GtkWidget* dlg = gtk_message_dialog_new(parentWindow(parent), GTK_DIALOG_MODAL, type,
                                                GTK_BUTTONS_NONE, "%s", m.text.c_str());
        if (!m.title.empty()) gtk_window_set_title(GTK_WINDOW(dlg), m.title.c_str());
        if (!m.detail.empty())
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s",
                                                     m.detail.c_str());

        struct Ctx
        {
            std::vector<DialogRole> roles;
            std::function<void(DialogRole)> done;
        };
        auto* ctx = new Ctx{m.buttons, std::move(done)};
        if (ctx->roles.empty()) ctx->roles = {DialogRole::Ok};
        auto ordered = ctx->roles;
        std::ranges::stable_sort(ordered, {}, gnomeOrder);
        for (auto r : ordered)
        {
            auto i = std::ranges::find(ctx->roles, r) - ctx->roles.begin();
            gtk_dialog_add_button(GTK_DIALOG(dlg), stockLabel(r), int(i));
        }

        g_signal_connect_data(
            dlg, "response",
            G_CALLBACK(+[](GtkDialog* d, int resp, gpointer p)
                       {
                           auto* ctx = static_cast<Ctx*>(p);
                           DialogRole role = resp >= 0 && size_t(resp) < ctx->roles.size()
                                                 ? ctx->roles[size_t(resp)]
                                                 : DialogRole::Cancel;
                           auto done = std::move(ctx->done);
                           gtk_widget_destroy(GTK_WIDGET(d));  // frees ctx
                           if (done) done(role);
                       }),
            ctx, +[](gpointer p, GClosure*) { delete static_cast<Ctx*>(p); }, GConnectFlags(0));
        gtk_widget_show(dlg);
    }

    void chooseFiles(anyui::Window* parent, const FileDialogSpec& f,
                     std::function<void(std::vector<std::string>)> done) override
    {
        GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_OPEN;
        if (f.mode == FileDialogSpec::Mode::Save) action = GTK_FILE_CHOOSER_ACTION_SAVE;
        if (f.mode == FileDialogSpec::Mode::Folder) action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;

        // GtkFileChooserNative goes through the portal when available.
        GtkFileChooserNative* dlg =
            gtk_file_chooser_native_new(f.title.empty() ? nullptr : f.title.c_str(),
                                        parentWindow(parent), action, nullptr, nullptr);
        auto* chooser = GTK_FILE_CHOOSER(dlg);
        gtk_file_chooser_set_select_multiple(chooser, f.mode == FileDialogSpec::Mode::OpenMany);
        if (action == GTK_FILE_CHOOSER_ACTION_SAVE)
            gtk_file_chooser_set_do_overwrite_confirmation(chooser, true);
        for (auto& flt : f.filters)
        {
            GtkFileFilter* ff = gtk_file_filter_new();
            gtk_file_filter_set_name(ff, flt.name.c_str());
            gtk_file_filter_add_pattern(ff, flt.pattern.c_str());
            gtk_file_chooser_add_filter(chooser, ff);
        }
        if (!f.initialPath.empty())
        {
            if (g_file_test(f.initialPath.c_str(), G_FILE_TEST_IS_DIR))
                gtk_file_chooser_set_current_folder(chooser, f.initialPath.c_str());
            else
                gtk_file_chooser_set_filename(chooser, f.initialPath.c_str());
        }

        using Done = std::function<void(std::vector<std::string>)>;
        g_signal_connect_data(
            dlg, "response",
            G_CALLBACK(+[](GtkNativeDialog* d, int resp, gpointer p)
                       {
                           std::vector<std::string> out;
                           if (resp == GTK_RESPONSE_ACCEPT)
                           {
                               GSList* names = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(d));
                               for (GSList* l = names; l; l = l->next)
                                   out.push_back(static_cast<char*>(l->data));
                               g_slist_free_full(names, g_free);
                           }
                           Done done = std::move(*static_cast<Done*>(p));
                           g_object_unref(d);  // frees p
                           if (done) done(std::move(out));
                       }),
            new Done(std::move(done)), +[](gpointer p, GClosure*) { delete static_cast<Done*>(p); },
            GConnectFlags(0));
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
    }

    void setClipboard(const std::string& s) override
    {
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), s.c_str(),
                               int(s.size()));
    }

    void setClipboardImage(int w, int h, PixelFormat format, const uint8_t* pixels,
                           size_t stride) override
    {
        GdkPixbuf* pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, w, h);
        if (!pb) return;
        uint8_t* dst = gdk_pixbuf_get_pixels(pb);
        const int dstStride = gdk_pixbuf_get_rowstride(pb);
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* src = pixels + size_t(y) * stride;
            uint8_t* row = dst + size_t(y) * dstStride;
            if (format == PixelFormat::Rgba8)
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
    static GtkWindow* parentWindow(anyui::Window* w)
    {
        return w ? GTK_WINDOW(static_cast<WWindow*>(w)->gtkWindow()) : nullptr;
    }

    bool quit_ = false;
    bool running_ = false;
    int code_ = 0;
};

}  // namespace
}  // namespace anyui::gtk3

ANYUI_BACKEND_ENTRY(gtk3, anyui::gtk3::Gtk3Backend)
