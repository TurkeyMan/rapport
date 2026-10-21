// GTK 4 backend (package anyui-gtk4).
//
// Lists use GtkColumnView over a lazy GListModel (AnyuiRows) whose items
// are bare GObjects tagged with their row index; cells pull text from the
// anyui::ListModel at bind time. Painted columns are GtkDrawingAreas.
#include <gtk/gtk.h>

#include "gtk_common.h"

#include <memory>

// ---------------------------------------------------------------- AnyuiRows

G_BEGIN_DECLS
G_DECLARE_FINAL_TYPE(AnyuiRows, anyui_rows, ANYUI, ROWS, GObject)
G_END_DECLS

struct _AnyuiRows
{
    GObject parent_instance;
    guint count;
};

static GType anyui_rows_get_item_type(GListModel*)
{
    return G_TYPE_OBJECT;
}
static guint anyui_rows_get_n_items(GListModel* m)
{
    return ANYUI_ROWS(m)->count;
}

static gpointer anyui_rows_get_item(GListModel* m, guint pos)
{
    if (pos >= ANYUI_ROWS(m)->count) return nullptr;
    auto* item = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
    g_object_set_data(item, "anyui-row", GUINT_TO_POINTER(pos));
    return item;
}

static void anyui_rows_iface_init(GListModelInterface* iface)
{
    iface->get_item_type = anyui_rows_get_item_type;
    iface->get_n_items = anyui_rows_get_n_items;
    iface->get_item = anyui_rows_get_item;
}

G_DEFINE_TYPE_WITH_CODE(AnyuiRows, anyui_rows, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, anyui_rows_iface_init))

static void anyui_rows_class_init(AnyuiRowsClass*) {}
static void anyui_rows_init(AnyuiRows* self)
{
    self->count = 0;
}

namespace anyui::gtk4
{
namespace
{

using namespace gtkcommon;

// TODO: take accent and contrast from the settings portal.
}  // namespace
}  // namespace anyui::gtk4

anyui::Color anyui::gtkcommon::themeColor(GtkWidget* w, ThemeColor role)
{
    GdkRGBA fg;
    gtk_widget_get_color(w, &fg);
    return fallbackColor(role, toColor(fg), isDarkText(fg));
}

namespace anyui::gtk4
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
        gtk_check_button_set_active(GTK_CHECK_BUTTON(w_), s.checked);
        connect(
            w_, "toggled",
            +[](GtkCheckButton* b, gpointer p)
            {
                auto* self = static_cast<WCheck*>(p);
                if (self->toggled_) self->toggled_(gtk_check_button_get_active(b));
            },
            this);
    }

    bool checked() const override { return gtk_check_button_get_active(GTK_CHECK_BUTTON(w_)); }
    void setChecked(bool v) override { gtk_check_button_set_active(GTK_CHECK_BUTTON(w_), v); }
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
        gtk_editable_set_text(GTK_EDITABLE(w_), s.text.c_str());
        if (!s.placeholder.empty()) setPlaceholder(s.placeholder);
        connect(
            w_, "changed",
            +[](GtkEditable* e, gpointer p)
            {
                auto* self = static_cast<WEntry*>(p);
                if (self->changed_) self->changed_(gtk_editable_get_text(e));
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

    std::string text() const override { return gtk_editable_get_text(GTK_EDITABLE(w_)); }
    void setText(const std::string& t) override
    {
        gtk_editable_set_text(GTK_EDITABLE(w_), t.c_str());
    }
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
        : W(gtk_drop_down_new(nullptr, nullptr))
    {
        setItems(s.items);
        setCurrent(s.current);
        connect(
            w_, "notify::selected",
            +[](GObject*, GParamSpec*, gpointer p)
            {
                auto* self = static_cast<WSelect*>(p);
                if (self->changed_) self->changed_(self->current());
            },
            this);
    }

    void setItems(std::vector<std::string> items) override
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
    void setCurrent(int i) override
    {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(w_), i < 0 ? GTK_INVALID_LIST_POSITION : guint(i));
    }
    void onChanged(std::function<void(int)> f) override { changed_ = std::move(f); }

private:
    std::function<void(int)> changed_;
};

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

// GTK 4 has no non-deprecated editable combo; GtkComboBoxText still works.
class WCombo : public W<anyui::Combo>
{
public:
    explicit WCombo(const spec::Combo& s)
        : W(gtk_combo_box_text_new_with_entry())
    {
        entry_ = gtk_combo_box_get_child(GTK_COMBO_BOX(w_));
        setItems(s.items);
        setText(s.text);
        connect(
            entry_, "changed",
            +[](GtkEditable* e, gpointer p)
            {
                auto* self = static_cast<WCombo*>(p);
                if (self->changed_) self->changed_(gtk_editable_get_text(e));
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
    std::string text() const override { return gtk_editable_get_text(GTK_EDITABLE(entry_)); }
    void setText(const std::string& t) override
    {
        gtk_editable_set_text(GTK_EDITABLE(entry_), t.c_str());
    }
    void onChanged(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void onActivate(std::function<void()> f) override { activate_ = std::move(f); }

private:
    GtkWidget* entry_;
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

G_GNUC_END_IGNORE_DEPRECATIONS

class WTextView : public W<anyui::TextView>
{
public:
    explicit WTextView(const spec::TextView& s)
        : W(gtk_text_view_new(), gtk_scrolled_window_new())
    {
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer_), w_);
        gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(outer_), true);
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
        auto* p = GTK_PANED(w_);
        bool vertical =
            gtk_orientable_get_orientation(GTK_ORIENTABLE(w_)) == GTK_ORIENTATION_VERTICAL;
        float total = float(vertical ? gtk_widget_get_height(w_) : gtk_widget_get_width(w_));
        float pos = float(gtk_paned_get_position(p));
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
        : W(nullptr, gtk_scrolled_window_new())
    {
        rows_ = ANYUI_ROWS(g_object_new(anyui_rows_get_type(), nullptr));
        auto* rows = G_LIST_MODEL(rows_);  // ownership passes to the selection model
        switch (s.selection)
        {
        case SelectionMode::None: sel_ = GTK_SELECTION_MODEL(gtk_no_selection_new(rows)); break;
        case SelectionMode::Single:
        {
            auto* single = gtk_single_selection_new(rows);
            gtk_single_selection_set_autoselect(single, false);
            gtk_single_selection_set_can_unselect(single, true);
            sel_ = GTK_SELECTION_MODEL(single);
            break;
        }
        case SelectionMode::Multi: sel_ = GTK_SELECTION_MODEL(gtk_multi_selection_new(rows)); break;
        }

        w_ = gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(sel_)));
        auto* view = GTK_COLUMN_VIEW(w_);
        gtk_column_view_set_show_column_separators(view, true);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer_), w_);
        gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(outer_), true);

        for (size_t i = 0; i < s.columns.size(); ++i) addColumn(s.columns[i], i);

        actions_ = g_simple_action_group_new();
        gtk_widget_insert_action_group(w_, "ctx", G_ACTION_GROUP(actions_));

        connect(
            sel_, "selection-changed",
            +[](GtkSelectionModel*, guint, guint, gpointer p)
            {
                auto* self = static_cast<WList*>(p);
                if (self->selChanged_) self->selChanged_();
            },
            this);
        connect(
            w_, "activate",
            +[](GtkColumnView*, guint row, gpointer p)
            {
                auto* self = static_cast<WList*>(p);
                if (self->activate_) self->activate_(row);
            },
            this);
        connect(
            gtk_column_view_get_sorter(view), "changed",
            +[](GtkSorter* sorter, GtkSorterChange, gpointer p)
            { static_cast<WList*>(p)->sortChanged(GTK_COLUMN_VIEW_SORTER(sorter)); },
            this);
    }

    ~WList() override
    {
        if (model_) model_->removeObserver(this);
        g_object_unref(sel_);
        g_object_unref(actions_);
    }

    void setModel(ListModel* m) override
    {
        if (model_) model_->removeObserver(this);
        model_ = m;
        if (model_) model_->addObserver(this);
        modelReset();
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

    void setCurrent(size_t row) override
    {
        gtk_column_view_scroll_to(
            GTK_COLUMN_VIEW(w_), guint(row), nullptr,
            GtkListScrollFlags(GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT), nullptr);
    }

    void scrollTo(size_t row) override
    {
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(w_), guint(row), nullptr, GTK_LIST_SCROLL_NONE,
                                  nullptr);
    }

    void setSortIndicator(std::optional<size_t> column, SortOrder order) override
    {
        suppressSort_ = true;
        GtkColumnViewColumn* col = column && *column < cols_.size() ? cols_[*column] : nullptr;
        gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(w_), col,
                                       order == SortOrder::Ascending ? GTK_SORT_ASCENDING
                                                                     : GTK_SORT_DESCENDING);
        suppressSort_ = false;
    }

    std::vector<float> columnWidths() const override
    {
        std::vector<float> out;
        for (auto* c : cols_) out.push_back(float(gtk_column_view_column_get_fixed_width(c)));
        return out;
    }

    void setColumnWidths(const std::vector<float>& widths) override
    {
        for (size_t i = 0; i < widths.size() && i < cols_.size(); ++i)
            if (widths[i] > 0) gtk_column_view_column_set_fixed_width(cols_[i], int(widths[i]));
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

    // ListObserver
    void modelReset() override
    {
        guint old = rows_->count;
        rows_->count = model_ ? guint(model_->rowCount()) : 0;
        g_list_model_items_changed(G_LIST_MODEL(rows_), 0, old, rows_->count);
    }
    void rowsChanged(size_t first, size_t n) override
    {
        g_list_model_items_changed(G_LIST_MODEL(rows_), guint(first), guint(n), guint(n));
    }
    void rowsInserted(size_t first, size_t n) override
    {
        rows_->count += guint(n);
        g_list_model_items_changed(G_LIST_MODEL(rows_), guint(first), 0, guint(n));
    }
    void rowsRemoved(size_t first, size_t n) override
    {
        rows_->count -= guint(n);
        g_list_model_items_changed(G_LIST_MODEL(rows_), guint(first), guint(n), 0);
    }

protected:
    void onDisconnect() override
    {
        alive_ = false;
        if (popover_) gtk_widget_unparent(popover_);
        popover_ = nullptr;
    }

private:
    struct ColumnCtx
    {
        WList* list;
        size_t column;
        bool painted;
    };

    static size_t rowOf(gpointer obj)
    {
        return GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(obj), "anyui-row"));
    }

    void addColumn(const Column& c, size_t index)
    {
        auto& ctx = ctxs_.emplace_back(new ColumnCtx{this, index, c.painted});
        GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
        connect(
            factory, "setup",
            +[](GtkSignalListItemFactory*, GtkListItem* li, gpointer p)
            { static_cast<ColumnCtx*>(p)->list->setupCell(li, *static_cast<ColumnCtx*>(p)); },
            ctx.get());
        connect(
            factory, "bind",
            +[](GtkSignalListItemFactory*, GtkListItem* li, gpointer p)
            { static_cast<ColumnCtx*>(p)->list->bindCell(li, *static_cast<ColumnCtx*>(p)); },
            ctx.get());

        GtkColumnViewColumn* col = gtk_column_view_column_new(c.title.c_str(), factory);
        gtk_column_view_column_set_resizable(col, true);
        gtk_column_view_column_set_expand(col, c.stretch);
        int width = px(c.width);
        if (width < 0 && c.painted) width = int(8 * g_metrics.charWidth);
        if (width > 0) gtk_column_view_column_set_fixed_width(col, width);
        if (c.sortable)
        {
            // Never compares anything: it only makes the header clickable.
            // Sorting happens in the app's model.
            GtkCustomSorter* sorter = gtk_custom_sorter_new(nullptr, nullptr, nullptr);
            gtk_column_view_column_set_sorter(col, GTK_SORTER(sorter));
            g_object_unref(sorter);
        }
        gtk_column_view_append_column(GTK_COLUMN_VIEW(w_), col);
        cols_.push_back(col);
        g_object_unref(col);
    }

    void setupCell(GtkListItem* li, ColumnCtx& ctx)
    {
        GtkWidget* child;
        if (ctx.painted)
        {
            child = gtk_drawing_area_new();
            gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(child),
                                                int(g_metrics.lineHeight + 0.5f));
            gtk_widget_set_hexpand(child, true);
            gtk_drawing_area_set_draw_func(
                GTK_DRAWING_AREA(child),
                +[](GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer p)
                {
                    static_cast<ColumnCtx*>(p)->list->drawCell(area, cr, w, h,
                                                               *static_cast<ColumnCtx*>(p));
                },
                &ctx, nullptr);
        }
        else
        {
            child = gtk_label_new(nullptr);
            gtk_label_set_xalign(GTK_LABEL(child), 0);
            gtk_label_set_ellipsize(GTK_LABEL(child), PANGO_ELLIPSIZE_END);
            gtk_label_set_single_line_mode(GTK_LABEL(child), true);
        }

        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
        g_signal_connect(click, "pressed",
                         G_CALLBACK(+[](GtkGestureClick* g, int, double x, double y, gpointer p)
                                    {
                                        auto* cell = gtk_event_controller_get_widget(
                                            GTK_EVENT_CONTROLLER(g));
                                        static_cast<ColumnCtx*>(p)->list->contextMenuAt(cell, x, y);
                                    }),
                         &ctx);
        gtk_widget_add_controller(child, GTK_EVENT_CONTROLLER(click));

        gtk_list_item_set_child(li, child);
    }

    void bindCell(GtkListItem* li, ColumnCtx& ctx)
    {
        if (!model_) return;
        size_t row = rowOf(gtk_list_item_get_item(li));
        GtkWidget* child = gtk_list_item_get_child(li);
        // Stored +1 so 0 means "not bound".
        g_object_set_data(G_OBJECT(child), "anyui-row", GSIZE_TO_POINTER(row + 1));
        if (ctx.painted)
        {
            gtk_widget_queue_draw(child);
            return;
        }
        gtk_label_set_text(GTK_LABEL(child), model_->text(row, ctx.column).c_str());
        PangoAttrList* attrs = pangoAttrs(child, model_->style(row, ctx.column));
        gtk_label_set_attributes(GTK_LABEL(child), attrs);
        if (attrs) pango_attr_list_unref(attrs);
    }

    void drawCell(GtkDrawingArea* area, cairo_t* cr, int w, int h, ColumnCtx& ctx)
    {
        size_t tagged = rowOf(area);
        if (!alive_ || !model_ || tagged == 0) return;
        size_t row = tagged - 1;
        if (row >= model_->rowCount()) return;
        CairoPainter painter(cr, GTK_WIDGET(area));
        CellInfo info{{float(w), float(h)},
                      bool(gtk_selection_model_is_selected(sel_, guint(row))),
                      gtk_widget_has_focus(GTK_WIDGET(area))};
        model_->paint(row, ctx.column, painter, info);
    }

    void sortChanged(GtkColumnViewSorter* sorter)
    {
        if (suppressSort_ || !sort_) return;
        GtkColumnViewColumn* col = gtk_column_view_sorter_get_primary_sort_column(sorter);
        auto it = std::ranges::find(cols_, col);
        if (it == cols_.end()) return;
        bool asc = gtk_column_view_sorter_get_primary_sort_order(sorter) == GTK_SORT_ASCENDING;
        sort_(size_t(it - cols_.begin()), asc ? SortOrder::Ascending : SortOrder::Descending);
    }

    void contextMenuAt(GtkWidget* cell, double x, double y)
    {
        if (!alive_) return;
        size_t tagged = rowOf(cell);
        std::optional<size_t> row;
        if (tagged) row = tagged - 1;
        if (row && !gtk_selection_model_is_selected(sel_, guint(*row)))
            gtk_selection_model_select_item(sel_, guint(*row), true);
        if (!contextMenu_) return;

        menu_ = contextMenu_(row);
        if (menu_.empty()) return;

        for (auto& name : actionNames_)
            g_action_map_remove_action(G_ACTION_MAP(actions_), name.c_str());
        actionNames_.clear();
        GMenu* model = buildMenu(menu_);

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

    // Separators become GMenu sections.
    GMenu* buildMenu(const Menu& items)
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

        for (const MenuItem& item : items)
        {
            if (item.isSeparator())
            {
                flush();
                continue;
            }
            if (!item.children.empty())
            {
                GMenu* sub = buildMenu(item.children);
                g_menu_append_submenu(section, item.label.c_str(), G_MENU_MODEL(sub));
                g_object_unref(sub);
                continue;
            }

            std::string name = "i" + std::to_string(actionNames_.size());
            actionNames_.push_back(name);
            GSimpleAction* action =
                item.checked ? g_simple_action_new_stateful(name.c_str(), nullptr,
                                                            g_variant_new_boolean(*item.checked))
                             : g_simple_action_new(name.c_str(), nullptr);
            g_simple_action_set_enabled(action, item.enabled && item.action);
            g_signal_connect(action, "activate",
                             G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer p)
                                        {
                                            auto* mi = static_cast<const MenuItem*>(p);
                                            if (mi->action) mi->action();
                                        }),
                             const_cast<MenuItem*>(&item));
            g_action_map_add_action(G_ACTION_MAP(actions_), G_ACTION(action));
            g_object_unref(action);

            GMenuItem* mi = g_menu_item_new(item.label.c_str(), ("ctx." + name).c_str());
            if (!item.shortcut.empty())
                g_menu_item_set_attribute(mi, "accel", "s", gtkAccel(item.shortcut).c_str());
            g_menu_append_item(section, mi);
            g_object_unref(mi);
        }
        flush();
        g_object_unref(section);
        return top;
    }

    AnyuiRows* rows_ = nullptr;  // owned by sel_
    GtkSelectionModel* sel_ = nullptr;
    GSimpleActionGroup* actions_ = nullptr;
    GtkWidget* popover_ = nullptr;
    ListModel* model_ = nullptr;
    std::vector<GtkColumnViewColumn*> cols_;
    std::vector<std::unique_ptr<ColumnCtx>> ctxs_;
    std::vector<std::string> actionNames_;
    Menu menu_;  // owns the callbacks the popover's actions point at
    bool suppressSort_ = false;
    bool alive_ = true;

    std::function<void()> selChanged_;
    std::function<void(size_t)> activate_;
    std::function<void(size_t, SortOrder)> sort_;
    std::function<Menu(std::optional<size_t>)> contextMenu_;
};

// ---------------------------------------------------------------- window

// ---------------------------------------------------------------- canvas

// Paint, resize, cursor and frame callbacks. Written without a GTK 4 build
// to check it against.
// TODO(pengit): input (GtkGestureClick, GtkEventControllerMotion /Scroll
// /Key) and focus, mirroring the gtk3 WCanvas.
class WCanvas : public W<anyui::Canvas>
{
public:
    explicit WCanvas(const spec::Canvas& s)
        : W(gtk_drawing_area_new())
    {
        gtk_widget_set_focusable(w_, s.focusable);
        gtk_drawing_area_set_draw_func(
            GTK_DRAWING_AREA(w_),
            +[](GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer p)
            {
                auto* self = static_cast<WCanvas*>(p);
                if (!self->paint_) return;
                CairoPainter painter(cr, GTK_WIDGET(area));
                self->paint_(painter, {float(w), float(h)});
            },
            this, nullptr);
        connect(
            w_, "resize",
            +[](GtkDrawingArea*, int w, int h, gpointer p)
            {
                auto* self = static_cast<WCanvas*>(p);
                if (self->resize_) self->resize_({float(w), float(h)});
            },
            this);
    }

    ~WCanvas() override { onDisconnect(); }

    void invalidate() override { gtk_widget_queue_draw(w_); }
    SizeF size() const override
    {
        return {float(gtk_widget_get_width(w_)), float(gtk_widget_get_height(w_))};
    }
    float deviceScale() const override { return float(gtk_widget_get_scale_factor(w_)); }
    void setCursor(Cursor c) override
    {
        static const char* names[] = {"default", "pointer",  "text",    "crosshair",
                                      "grab",    "grabbing", "zoom-in", "zoom-out",
                                      "move",    "wait",     "none"};
        gtk_widget_set_cursor_from_name(w_, c == Cursor::Default ? nullptr : names[int(c)]);
    }

    void onPaint(std::function<void(Painter&, SizeF)> f) override { paint_ = std::move(f); }
    void onResize(std::function<void(SizeF)> f) override { resize_ = std::move(f); }
    void onPointer(std::function<bool(const PointerEvent&)>) override {}
    void onScroll(std::function<bool(const ScrollEvent&)>) override {}
    void onKey(std::function<bool(const KeyEvent&)>) override {}

    Subscription onFrame(std::function<void(double)> f) override
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
        return Subscription(
            [this, alive = alive_, id]
            {
                if (!*alive) return;
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
    std::function<void(Painter&, SizeF)> paint_;
    std::function<void(SizeF)> resize_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::vector<guint> ticks_;
};

class WWindow : public anyui::Window, public Part
{
public:
    WWindow(const WindowSpec& spec, WWindow* parent)
        : win_(gtk_window_new())
        , dialog_(spec.dialog)
    {
        auto* win = GTK_WINDOW(win_);
        gtk_window_set_title(win, spec.title.c_str());
        gtk_window_set_default_size(win, px(spec.width), px(spec.height));
        if (parent) gtk_window_set_transient_for(win, GTK_WINDOW(parent->win_));
        if (dialog_)
        {
            gtk_window_set_modal(win, parent != nullptr);
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
                static_cast<WWindow*>(p)->closeRequested();
                return true;  // we own the window; never let GTK destroy it
            },
            this);
    }

    ~WWindow() override
    {
        for (auto& part : parts) part->disconnect();
        disconnect();
        gtk_window_destroy(GTK_WINDOW(win_));
    }

    void setTitle(const std::string& t) override
    {
        gtk_window_set_title(GTK_WINDOW(win_), t.c_str());
    }
    void show() override
    {
        finished_ = false;
        gtk_window_present(GTK_WINDOW(win_));
    }
    void close() override { gtk_widget_set_visible(win_, false); }
    void onFinished(std::function<void(DialogRole)> f) override { onFinished_ = std::move(f); }
    void onCloseRequest(std::function<bool()> f) override { onCloseRequest_ = std::move(f); }

    void setContent(GtkWidget* content)
    {
        int margin = dialog_ ? 12 : 6;
        gtk_widget_set_margin_start(content, margin);
        gtk_widget_set_margin_end(content, margin);
        gtk_widget_set_margin_top(content, margin);
        gtk_widget_set_margin_bottom(content, margin);
        gtk_window_set_child(GTK_WINDOW(win_), content);
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
            gtk_widget_add_css_class(button, "suggested-action");
            gtk_window_set_default_widget(GTK_WINDOW(win_), button);
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
        for (auto& c : s.items) gtk_box_append(GTK_BOX(box), build(c, s.dir));
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
            gtk_widget_add_css_class(label, "dim-label");
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
        gtk_paned_set_start_child(p, first);
        gtk_paned_set_end_child(p, panes(s, from + 1));
        gtk_paned_set_resize_start_child(p, !anyStretch || s.panes[from].common.stretch > 0);
        gtk_paned_set_shrink_start_child(p, false);
        gtk_paned_set_shrink_end_child(p, false);
        return paned;
    }

    GtkWidget* make(const Node&, const spec::Group& s, Orientation)
    {
        GtkWidget* frame = gtk_frame_new(s.title.c_str());
        if (!s.content.empty())
        {
            GtkWidget* child = build(s.content.front(), Orientation::Vertical);
            gtk_widget_set_margin_start(child, 6);
            gtk_widget_set_margin_end(child, 6);
            gtk_widget_set_margin_top(child, 6);
            gtk_widget_set_margin_bottom(child, 6);
            gtk_frame_set_child(GTK_FRAME(frame), child);
        }
        return frame;
    }

    GtkWidget* make(const Node& n, const spec::Tabs& s, Orientation)
    {
        GtkWidget* nb = gtk_notebook_new();
        for (size_t i = 0; i < s.pages.size(); ++i)
        {
            GtkWidget* page = build(s.pages[i], Orientation::Vertical);
            gtk_widget_set_margin_start(page, 6);
            gtk_widget_set_margin_end(page, 6);
            gtk_widget_set_margin_top(page, 6);
            gtk_widget_set_margin_bottom(page, 6);
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
                gtk_box_append(GTK_BOX(box), gap);
                spaced = true;
            }
            auto* w = new WButton(b.text.empty() ? stockLabel(b.role) : b.text);
            win_.parts.emplace_back(w);
            if (!b.onClick) win_.addRoleButton(w->outer(), b.role);
            sink_.created(b, *w);
            gtk_box_append(GTK_BOX(box), w->outer());
        }
        return box;
    }

    WWindow& win_;
    BuildSink& sink_;
};

// ---------------------------------------------------------------- backend

class Gtk4Backend : public Backend
{
public:
    static Backend* create(int*, char**)
    {
        if (!gtk_init_check()) return nullptr;
        initMetrics();
        return new Gtk4Backend;
    }

    std::string_view name() const override { return "gtk4"; }

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
        loop_ = g_main_loop_new(nullptr, false);
        g_main_loop_run(loop_);
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

    void message(anyui::Window* parent, const MessageSpec& m,
                 std::function<void(DialogRole)> done) override
    {
        GtkAlertDialog* dlg = gtk_alert_dialog_new("%s", m.text.c_str());
        if (!m.detail.empty()) gtk_alert_dialog_set_detail(dlg, m.detail.c_str());

        struct Ctx
        {
            std::vector<DialogRole> roles;
            std::function<void(DialogRole)> done;
        };
        auto* ctx = new Ctx{m.buttons, std::move(done)};
        if (ctx->roles.empty()) ctx->roles = {DialogRole::Ok};

        std::vector<std::string> labels;
        for (auto r : ctx->roles)
        {
            std::string l = stockLabel(r);
            std::erase(l, '_');
            labels.push_back(l);
        }
        std::vector<const char*> ptrs;
        for (auto& l : labels) ptrs.push_back(l.c_str());
        ptrs.push_back(nullptr);
        gtk_alert_dialog_set_buttons(dlg, ptrs.data());
        for (size_t i = 0; i < ctx->roles.size(); ++i)
            if (ctx->roles[i] == DialogRole::Cancel || ctx->roles[i] == DialogRole::No)
                gtk_alert_dialog_set_cancel_button(dlg, int(i));

        gtk_alert_dialog_choose(
            dlg, parentWindow(parent), nullptr,
            +[](GObject* src, GAsyncResult* res, gpointer p)
            {
                std::unique_ptr<Ctx> ctx(static_cast<Ctx*>(p));
                int i = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
                DialogRole role = i >= 0 && size_t(i) < ctx->roles.size() ? ctx->roles[size_t(i)]
                                                                          : DialogRole::Cancel;
                if (ctx->done) ctx->done(role);
            },
            ctx);
        g_object_unref(dlg);
    }

    void chooseFiles(anyui::Window* parent, const FileDialogSpec& f,
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
                gtk_file_filter_add_pattern(ff, flt.pattern.c_str());
                g_list_store_append(store, ff);
                g_object_unref(ff);
            }
            gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(store));
            g_object_unref(store);
        }
        if (!f.initialPath.empty())
        {
            GFile* file = g_file_new_for_path(f.initialPath.c_str());
            if (g_file_test(f.initialPath.c_str(), G_FILE_TEST_IS_DIR))
                gtk_file_dialog_set_initial_folder(dlg, file);
            else
                gtk_file_dialog_set_initial_file(dlg, file);
            g_object_unref(file);
        }

        using Done = std::function<void(std::vector<std::string>)>;
        auto* ctx = new Done(std::move(done));
        auto pathOf = [](GFile* file)
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
            std::unique_ptr<Done> done(static_cast<Done*>(p));
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
        (void)pathOf;

        GtkWindow* pw = parentWindow(parent);
        switch (f.mode)
        {
        case FileDialogSpec::Mode::Open:
            gtk_file_dialog_open(
                dlg, pw, nullptr,
                +[](GObject* s, GAsyncResult* r, gpointer p)
                { single(gtk_file_dialog_open_finish(GTK_FILE_DIALOG(s), r, nullptr), p); },
                ctx);
            break;
        case FileDialogSpec::Mode::Save:
            gtk_file_dialog_save(
                dlg, pw, nullptr,
                +[](GObject* s, GAsyncResult* r, gpointer p)
                { single(gtk_file_dialog_save_finish(GTK_FILE_DIALOG(s), r, nullptr), p); },
                ctx);
            break;
        case FileDialogSpec::Mode::Folder:
            gtk_file_dialog_select_folder(
                dlg, pw, nullptr,
                +[](GObject* s, GAsyncResult* r, gpointer p)
                {
                    single(gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(s), r, nullptr), p);
                },
                ctx);
            break;
        case FileDialogSpec::Mode::OpenMany:
            gtk_file_dialog_open_multiple(
                dlg, pw, nullptr,
                +[](GObject* s, GAsyncResult* r, gpointer p)
                {
                    std::unique_ptr<Done> done(static_cast<Done*>(p));
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

    void setClipboard(const std::string& s) override
    {
        if (GdkDisplay* d = gdk_display_get_default())
            gdk_clipboard_set_text(gdk_display_get_clipboard(d), s.c_str());
    }

    // TODO(pengit): a GdkTexture-backed Image would avoid cairo for painting.
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
                auto state = *static_cast<std::shared_ptr<State>*>(p);
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

private:
    static GtkWindow* parentWindow(anyui::Window* w)
    {
        return w ? GTK_WINDOW(static_cast<WWindow*>(w)->gtkWindow()) : nullptr;
    }

    GMainLoop* loop_ = nullptr;
    bool quit_ = false;
    int code_ = 0;
};

}  // namespace
}  // namespace anyui::gtk4

ANYUI_BACKEND_ENTRY(gtk4, anyui::gtk4::Gtk4Backend)
