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

class WLabel : public W<anyui::Label>
{
public:
    explicit WLabel(const spec::Label& s)
        : W(gtk_label_new(s.text.c_str()))
    {
        gtk_label_set_xalign(GTK_LABEL(w_), 0);
        gtk_label_set_selectable(GTK_LABEL(w_), s.selectable);
    }

    void setText(const std::string& t) override { gtk_label_set_text(GTK_LABEL(w_), t.c_str()); }
};

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
    void onClick(std::function<void()> f) override { click_ = std::move(f); }

private:
    std::function<void()> click_;
};

class WCheck : public W<anyui::Check>
{
public:
    explicit WCheck(const spec::Check& s)
        : W(gtk_check_button_new_with_mnemonic(s.text.c_str()))
    {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w_), s.checked);
        connect(
            w_, "toggled",
            +[](GtkToggleButton* b, gpointer p)
            {
                auto* self = static_cast<WCheck*>(p);
                if (self->toggled_) self->toggled_(gtk_toggle_button_get_active(b));
            },
            this);
    }

    bool checked() const override { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w_)); }
    void setChecked(bool v) override { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w_), v); }
    void onToggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }

private:
    std::function<void(bool)> toggled_;
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

private:
    GtkTextBuffer* buffer_;
    TagCache tags_;
    std::function<void()> changed_;
};

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
        float total = float(vertical ? gtk_widget_get_allocated_height(w_)
                                     : gtk_widget_get_allocated_width(w_));
        float pos = float(gtk_paned_get_position(GTK_PANED(w_)));
        return {pos, total - pos};
    }

    void setSizes(const std::vector<float>& s) override
    {
        if (!s.empty()) gtk_paned_set_position(GTK_PANED(w_), int(s[0]));
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
                if (self->changed_) self->changed_(int(page));
            },
            this);
    }

    int current() const override { return gtk_notebook_get_current_page(GTK_NOTEBOOK(w_)); }
    void setCurrent(int i) override { gtk_notebook_set_current_page(GTK_NOTEBOOK(w_), i); }
    void onChanged(std::function<void(int)> f) override { changed_ = std::move(f); }

private:
    std::function<void(int)> changed_;
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
        gtk_tree_view_set_rubber_banding(view, s.selection == SelectionMode::Multi);

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
            w_, "popup-menu",
            +[](GtkWidget*, gpointer p) -> gboolean
            {
                auto* self = static_cast<WList*>(p);
                return self->popup(self->cursorRow(), nullptr);
            },
            this);
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
            if (widths[i] > 0) gtk_tree_view_column_set_fixed_width(cols_[i], int(widths[i]));
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
        if (menu_) gtk_widget_destroy(menu_);
        menu_ = nullptr;
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
            pr->height = int(g_metrics.lineHeight + 0.5f);
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
        gtk_tree_view_append_column(GTK_TREE_VIEW(w_), col);
        cols_.push_back(col);
    }

    static void textCell(GtkTreeViewColumn*, GtkCellRenderer* r, GtkTreeModel*, GtkTreeIter* iter,
                         gpointer p)
    {
        auto* ctx = static_cast<ColumnCtx*>(p);
        ListModel* model = ctx->list->model_;
        if (!model || !ctx->list->alive_) return;
        size_t row = rowOf(iter);
        PangoAttrList* attrs = pangoAttrs(ctx->list->w_, model->style(row, ctx->column));
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
        CairoPainter painter(
            cr, widget, selected ? std::optional<Paint>{ThemeColor::HighlightText} : std::nullopt);
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

    gboolean buttonPressed(GdkEventButton* e)
    {
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

        if (menu_) gtk_widget_destroy(menu_);
        menuItems_ = std::move(items);
        menu_ = buildMenu(menuItems_);
        gtk_menu_attach_to_widget(GTK_MENU(menu_), w_, nullptr);
        gtk_widget_show_all(menu_);
        if (event)
            gtk_menu_popup_at_pointer(GTK_MENU(menu_), event);
        else
            gtk_menu_popup_at_widget(GTK_MENU(menu_), w_, GDK_GRAVITY_CENTER,
                                     GDK_GRAVITY_NORTH_WEST, nullptr);
        return true;
    }

    static GtkWidget* buildMenu(const Menu& items)
    {
        GtkWidget* menu = gtk_menu_new();
        for (const MenuItem& item : items)
        {
            GtkWidget* mi;
            if (item.isSeparator())
            {
                mi = gtk_separator_menu_item_new();
            }
            else if (!item.children.empty())
            {
                mi = gtk_menu_item_new_with_label(item.label.c_str());
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), buildMenu(item.children));
            }
            else
            {
                if (item.checked)
                {
                    mi = gtk_check_menu_item_new_with_label(item.label.c_str());
                    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), *item.checked);
                }
                else
                {
                    mi = gtk_menu_item_new_with_label(item.label.c_str());
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
        return menu;
    }

    AnyuiTreeRows* rows_ = nullptr;
    GtkTreeSelection* sel_ = nullptr;
    GtkWidget* menu_ = nullptr;
    ListModel* model_ = nullptr;
    std::vector<GtkTreeViewColumn*> cols_;
    std::vector<std::unique_ptr<ColumnCtx>> ctxs_;
    Menu menuItems_;
    bool alive_ = true;

    std::function<void()> selChanged_;
    std::function<void(size_t)> activate_;
    std::function<void(size_t, SortOrder)> sort_;
    std::function<Menu(std::optional<size_t>)> contextMenu_;
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
    // Development aid: ANYUI_SNAPSHOT=file.png saves the first window shown,
    // ANYUI_SNAPSHOT_DELAY ms (default 500) after it appears, then quits
    // the main loop. For checking layouts and canvases without a screen
    // grabber.
    void snapshotIfAsked()
    {
        static bool taken = false;
        const char* path = std::getenv("ANYUI_SNAPSHOT");
        if (taken || !path || !*path) return;
        taken = true;
        const char* delay = std::getenv("ANYUI_SNAPSHOT_DELAY");
        g_timeout_add(
            delay ? guint(std::atoi(delay)) : 500u,
            +[](gpointer p) -> gboolean
            {
                GtkWidget* win = static_cast<GtkWidget*>(p);
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
            win_);
    }

public:
    WWindow(const WindowSpec& spec, WWindow* parent)
        : win_(gtk_window_new(GTK_WINDOW_TOPLEVEL))
        , dialog_(spec.dialog)
    {
        auto* win = GTK_WINDOW(win_);
        gtk_window_set_title(win, spec.title.c_str());
        gtk_window_set_default_size(win, px(spec.width), px(spec.height));
        if (parent) gtk_window_set_transient_for(win, GTK_WINDOW(parent->win_));
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
    }

    ~WWindow() override
    {
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

    void setContent(GtkWidget* content)
    {
        int margin = dialog_ ? 12 : 6;
        gtk_widget_set_margin_start(content, margin);
        gtk_widget_set_margin_end(content, margin);
        gtk_widget_set_margin_top(content, margin);
        gtk_widget_set_margin_bottom(content, margin);
        gtk_container_add(GTK_CONTAINER(win_), content);
        // GTK 3 widgets start hidden; widgets the app hides later stay hidden.
        gtk_widget_show_all(content);
    }

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
        for (auto& c : s.items) gtk_container_add(GTK_CONTAINER(box), build(c, s.dir));
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
            gtk_widget_set_hexpand(field, true);
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
        if (GTK_IS_PANED(paned)) add<WSplitter>(n, paned);
        return paned;
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
        gtk_paned_pack1(p, first, !anyStretch || s.panes[from].common.stretch > 0, false);
        gtk_paned_pack2(p, panes(s, from + 1), true, false);
        return paned;
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
        GtkWidget* nb = gtk_notebook_new();
        for (size_t i = 0; i < s.pages.size(); ++i)
        {
            GtkWidget* page = build(s.pages[i], Orientation::Vertical);
            setMargins(page, 6);
            gtk_notebook_append_page(GTK_NOTEBOOK(nb), page,
                                     gtk_label_new_with_mnemonic(s.titles[i].c_str()));
        }
        add<WTabs>(n, nb);
        return nb;
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

    GtkWidget* make(const Node& n, const spec::Button& s, Orientation)
    {
        return add<WButton>(n, s.text).outer();
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
        win->setContent(Builder(*win, sink).build(spec.content, Orientation::Vertical));
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
