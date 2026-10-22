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
    // TODO(pengit): drawn icons (Icon::draw); named ones only for now.
    void setIcon(const Icon& icon) override
    {
        if (!icon.name.empty()) gtk_button_set_icon_name(GTK_BUTTON(w_), icon.name.c_str());
    }
    void onClick(std::function<void()> f) override { click_ = std::move(f); }

private:
    std::function<void()> click_;
};

// GTK 4 has no GtkToolbar: a flat button in a box. Written without a GTK 4
// build. TODO(pengit): toggles' pressed look, drawn icons.
class WToolButton : public W<anyui::ToolButton>
{
public:
    explicit WToolButton(const spec::ToolButton& s)
        : W(s.toggle ? gtk_toggle_button_new() : gtk_button_new())
        , toggle_(s.toggle)
    {
        gtk_button_set_has_frame(GTK_BUTTON(w_), false);
        gtk_widget_set_tooltip_text(w_, s.label.c_str());
        setIcon(s.icon);
        if (toggle_) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w_), s.checked);
        connect(
            w_, "clicked",
            +[](GtkButton*, gpointer p)
            {
                auto* self = static_cast<WToolButton*>(p);
                if (self->quiet_) return;
                if (self->click_) self->click_();
                if (self->toggle_ && self->toggled_) self->toggled_(self->checked());
            },
            this);
    }
    void setIcon(const Icon& icon) override
    {
        if (!icon.name.empty()) gtk_button_set_icon_name(GTK_BUTTON(w_), icon.name.c_str());
    }
    bool checked() const override
    {
        return toggle_ && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w_));
    }
    void setChecked(bool c) override
    {
        if (!toggle_) return;
        quiet_ = true;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w_), c);
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

// GtkScale and GtkSpinButton keep their GTK 3 API in GTK 4.
template <class Base>
class WValue : public W<Base>
{
public:
    explicit WValue(GtkWidget* w)
        : W<Base>(w)
    {
        this->connect(
            w, "value-changed",
            +[](GtkWidget*, gpointer p)
            {
                auto* self = static_cast<WValue*>(p);
                if (!self->quiet_ && self->changed_) self->changed_(self->value());
            },
            this);
    }
    double value() const override { return gtk_range_or_spin_value(this->w_); }
    void setValue(double v) override
    {
        quiet_ = true;
        if (GTK_IS_SPIN_BUTTON(this->w_))
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(this->w_), v);
        else
            gtk_range_set_value(GTK_RANGE(this->w_), v);
        quiet_ = false;
    }
    void setRange(double min, double max) override
    {
        quiet_ = true;
        if (GTK_IS_SPIN_BUTTON(this->w_))
            gtk_spin_button_set_range(GTK_SPIN_BUTTON(this->w_), min, max);
        else
            gtk_range_set_range(GTK_RANGE(this->w_), min, max);
        quiet_ = false;
    }
    void onChanged(std::function<void(double)> f) override { changed_ = std::move(f); }

private:
    static double gtk_range_or_spin_value(GtkWidget* w)
    {
        return GTK_IS_SPIN_BUTTON(w) ? gtk_spin_button_get_value(GTK_SPIN_BUTTON(w))
                                     : gtk_range_get_value(GTK_RANGE(w));
    }
    bool quiet_ = false;
    std::function<void(double)> changed_;
};

// TODO(pengit): GtkColorButton is deprecated since GTK 4.10 (GtkColorDialogButton).
class WColorButton : public W<anyui::ColorButton>
{
public:
    explicit WColorButton(const spec::ColorButton& s)
        : W(gtk_color_button_new())
    {
        setColor(s.color);
        gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(w_), s.alpha);
        connect(
            w_, "color-set",
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
        const GdkRGBA rgba{c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
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
            +[](GtkCheckButton* b, gpointer p)
            {
                auto* self = static_cast<WCheck*>(p);
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

    bool checked() const override { return state() == CheckState::Checked; }
    void setChecked(bool v) override { setState(v ? CheckState::Checked : CheckState::Unchecked); }

    CheckState state() const override
    {
        auto* b = GTK_CHECK_BUTTON(w_);
        if (gtk_check_button_get_inconsistent(b)) return CheckState::Indeterminate;
        return gtk_check_button_get_active(b) ? CheckState::Checked : CheckState::Unchecked;
    }

    void setState(CheckState s) override
    {
        auto* b = GTK_CHECK_BUTTON(w_);
        updating_ = true;
        gtk_check_button_set_inconsistent(b, s == CheckState::Indeterminate);
        gtk_check_button_set_active(b, s == CheckState::Checked);
        updating_ = false;
    }

    void onToggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }

private:
    std::function<void(bool)> toggled_;
    bool updating_ = false;
};

// TODO(pengit): build the menu as a GtkPopoverMenu (like WList's context
// menu) and show the pushed state; untested without GTK 4 headers.
// An anyui::Menu as a GMenu model plus the actions its items name, for
// popover menus. Built afresh each time a menu opens (so it shows the app's
// state then); the actions call into the Menu kept here, which lives until
// the next build. Separators become GMenu sections.
class MenuActions
{
public:
    MenuActions(GtkWidget* owner, std::string prefix)
        : prefix_(std::move(prefix))
        , group_(g_simple_action_group_new())
    {
        gtk_widget_insert_action_group(owner, prefix_.c_str(), G_ACTION_GROUP(group_));
    }
    ~MenuActions() { g_object_unref(group_); }
    MenuActions(const MenuActions&) = delete;
    MenuActions& operator=(const MenuActions&) = delete;

    // A new model for `menu`; the caller owns the reference.
    GMenu* build(Menu menu)
    {
        for (auto& name : names_) g_action_map_remove_action(G_ACTION_MAP(group_), name.c_str());
        names_.clear();
        menu_ = std::move(menu);
        return model(menu_);
    }

private:
    GMenu* model(const Menu& items)
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
                                            auto* mi = static_cast<const MenuItem*>(p);
                                            if (mi->action) mi->action();
                                        }),
                             const_cast<MenuItem*>(&item));
            g_action_map_add_action(G_ACTION_MAP(group_), G_ACTION(action));
            g_object_unref(action);
            GMenuItem* mi = g_menu_item_new(item.label.c_str(), (prefix_ + "." + name).c_str());
            if (!item.shortcut.empty()) g_menu_item_set_attribute(mi, "accel", "s", gtkAccel(item.shortcut).c_str());
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
    Menu menu_;
};

// A GtkMenuButton whose menu is built by `fn` each time it opens.
void attachMenu(GtkMenuButton* button, MenuActions* actions, std::function<Menu()>* fn)
{
    struct Ctx
    {
        MenuActions* actions;
        std::function<Menu()>* fn;
    };
    gtk_menu_button_set_create_popup_func(
        button,
        +[](GtkMenuButton* b, gpointer p)
        {
            auto* c = static_cast<Ctx*>(p);
            GMenu* model = c->actions->build(*c->fn ? (*c->fn)() : Menu{});
            gtk_menu_button_set_menu_model(b, G_MENU_MODEL(model));
            g_object_unref(model);
        },
        new Ctx{actions, fn}, +[](gpointer p) { delete static_cast<Ctx*>(p); });
}

class WMenuButton : public W<anyui::MenuButton>
{
public:
    explicit WMenuButton(const spec::MenuButton& s)
        : W(gtk_menu_button_new())
        , actions_(w_, "mb")
    {
        gtk_menu_button_set_use_underline(GTK_MENU_BUTTON(w_), true);  // "_View": V is the access key
        setText(s.text);
        attachMenu(GTK_MENU_BUTTON(w_), &actions_, &menuFn_);
    }

    void setText(const std::string& t) override
    {
        gtk_menu_button_set_label(GTK_MENU_BUTTON(w_), t.c_str());
    }
    void setIcon(const Icon&) override {}  // TODO(pengit)
    void setPushed(bool) override {}
    void onMenu(std::function<Menu()> f) override { menuFn_ = std::move(f); }

private:
    std::function<Menu()> menuFn_;
    MenuActions actions_;
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

// A menu button showing the date, with a calendar in its popover.
class WDatePicker : public W<anyui::DatePicker>
{
public:
    explicit WDatePicker(const spec::DatePicker& s)
        : W(gtk_menu_button_new())
    {
        calendar_ = gtk_calendar_new();
        gtk_calendar_set_show_week_numbers(GTK_CALENDAR(calendar_), true);
        GtkWidget* popover = gtk_popover_new();
        gtk_popover_set_child(GTK_POPOVER(popover), calendar_);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(w_), popover);
        setDate(s.date);

        connect(
            calendar_, "day-selected",
            +[](GtkCalendar* c, gpointer p)
            {
                auto* self = static_cast<WDatePicker*>(p);
                if (self->updating_) return;
                GDateTime* dt = gtk_calendar_get_date(c);
                self->date_ = {g_date_time_get_year(dt), g_date_time_get_month(dt),
                               g_date_time_get_day_of_month(dt)};
                g_date_time_unref(dt);
                self->updateLabel();
            },
            this);
        connect(
            popover, "show",
            +[](GtkWidget*, gpointer p)
            {
                auto* self = static_cast<WDatePicker*>(p);
                self->shown_ = self->date_;
            },
            this);
        connect(
            popover, "closed",
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
        updating_ = true;
        GDateTime* dt = g_date_time_new_local(d.year, d.month, d.day, 0, 0, 0);
        if (dt)
        {
            gtk_calendar_select_day(GTK_CALENDAR(calendar_), dt);
            g_date_time_unref(dt);
        }
        updating_ = false;
        updateLabel();
    }
    void onChanged(std::function<void(Date)> f) override { changed_ = std::move(f); }

private:
    void updateLabel()
    {
        GDate* d =
            g_date_new_dmy(GDateDay(date_.day), GDateMonth(date_.month), GDateYear(date_.year));
        char buf[64] = "";
        if (g_date_valid(d)) g_date_strftime(buf, sizeof buf, "%x", d);
        g_date_free(d);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(w_), buf);
    }

    GtkWidget* calendar_;
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
        // TODO(pengit): relies on a compact list having installed the
        // display-wide anyui-compact rule; untested without GTK 4 headers.
        if (s.compact) gtk_widget_add_css_class(w_, "anyui-compact");
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
        std::vector<float> out;
        GtkWidget* w = w_;
        for (;;)
        {
            auto* p = GTK_PANED(w);
            out.push_back(float(gtk_paned_get_position(p)));
            GtkWidget* next = gtk_paned_get_end_child(p);
            if (!next || !g_object_get_data(G_OBJECT(next), "anyui-split-chain"))
            {
                out.push_back(next ? float(vertical ? gtk_widget_get_height(next)
                                                    : gtk_widget_get_width(next))
                                   : 0);
                return out;
            }
            w = next;
        }
    }

    void setSizes(const std::vector<float>& s) override
    {
        GtkWidget* w = w_;
        for (size_t i = 0; i + 1 < s.size() && w; ++i)
        {
            auto* p = GTK_PANED(w);
            if (s[i] > 0) gtk_paned_set_position(p, int(s[i]));
            GtkWidget* next = gtk_paned_get_end_child(p);
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
        gtk_box_append(GTK_BOX(pages_[size_t(i)]), w);
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
        gtk_box_append(GTK_BOX(pages_[size_t(i)]), w);
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
        if (s.compact)
        {
            // TODO(pengit): untested without GTK 4 headers. One display-wide
            // rule, matching only lists marked compact.
            static bool installed = false;
            if (!installed)
            {
                std::string px = std::to_string(compactFont(w_).px) + "px";
                std::string rule =
                    "columnview.anyui-compact, textview.anyui-compact { font-size: " + px +
                    "; } columnview.anyui-compact row cell { padding-top: 0; "
                    "padding-bottom: 0; }";
                GtkCssProvider* css = gtk_css_provider_new();
                gtk_css_provider_load_from_string(css, rule.c_str());
                gtk_style_context_add_provider_for_display(gtk_widget_get_display(w_),
                                                           GTK_STYLE_PROVIDER(css),
                                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
                g_object_unref(css);
                installed = true;
            }
            gtk_widget_add_css_class(w_, "anyui-compact");
        }
        if (s.softSelection)
        {
            // The colours computed as gtk3 does, written into the rule:
            // @accent_bg_color isn't defined by every GTK 4 theme, and an
            // unresolved colour left selected rows with no background.
            soft_ = softSelectionColors(w_);
            static bool installed = false;
            if (!installed)
            {
                auto rgb = [](Color c)
                {
                    return "rgb(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," +
                           std::to_string(c.b) + ")";
                };
                // The row under the pointer gets a light tint, as on gtk3 and
                // win32; GTK 4 already gives rows :hover.
                Color base = themeColor(w_, ThemeColor::Base), accent = themeColor(w_, ThemeColor::Accent);
                const bool dark = base.r * 30 + base.g * 59 + base.b * 11 < 12800;
                const Color hover = mixColors(base, accent, dark ? 0.16f : 0.09f);
                std::string rule = "columnview.anyui-soft row:hover { background-color: " + rgb(hover) +
                                   "; } columnview.anyui-soft row:selected, columnview.anyui-soft row:selected:focus, "
                                   "columnview.anyui-soft row:selected:hover { background-color: " +
                                   rgb(soft_->first) + "; color: " + rgb(soft_->second) +
                                   "; } columnview row.anyui-drop { box-shadow: inset 0 0 0 2px " +
                                   rgb(themeColor(w_, ThemeColor::Highlight)) + "; }";
                GtkCssProvider* css = gtk_css_provider_new();
                gtk_css_provider_load_from_string(css, rule.c_str());
                gtk_style_context_add_provider_for_display(gtk_widget_get_display(w_),
                                                           GTK_STYLE_PROVIDER(css),
                                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
                g_object_unref(css);
                installed = true;
            }
            gtk_widget_add_css_class(w_, "anyui-soft");
        }
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer_), w_);
        gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(outer_), true);

        for (size_t i = 0; i < s.columns.size(); ++i) addColumn(s.columns[i], i);

        ctxMenu_ = std::make_unique<MenuActions>(w_, "ctx");
        if (s.autoScroll) enableAutoScroll();

        connect(
            sel_, "selection-changed",
            +[](GtkSelectionModel*, guint, guint, gpointer p)
            {
                auto* self = static_cast<WList*>(p);
                // Painted cells draw with the selection's colours.
                for (auto [li, ctx] : self->bound_)
                    if (ctx->painted) gtk_widget_queue_draw(gtk_list_item_get_child(li));
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
    // TODO(pengit): header right-click menu (gesture on the header row).
    void onHeaderMenu(std::function<Menu(size_t)> f) override { headerMenu_ = std::move(f); }
    void onCellContextMenu(std::function<Menu(size_t, size_t, PointF)> f) override { cellMenu_ = std::move(f); }
    // TODO(pengit): drag-reordering (GtkDragSource on the rows).
    void onReorder(ReorderFn) override {}
    void setReorderable(bool) override {}
    // Autoscroll (spec.autoScroll): with the middle button held, a tick
    // callback scrolls by the pointer's offset from the anchor. (The anchor
    // isn't drawn on gtk4 yet: the cursor shows the mode.)
    void enableAutoScroll()
    {
        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_MIDDLE);
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
        g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int, double, double y, gpointer p)
                                                       {
                                                           auto* self = static_cast<WList*>(p);
                                                           gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
                                                           self->startAutoScroll(y);
                                                       }),
                         this);
        g_signal_connect(click, "released", G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer p)
                                                        { static_cast<WList*>(p)->stopAutoScroll(); }),
                         this);
        g_signal_connect(click, "cancel", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer p)
                                                      { static_cast<WList*>(p)->stopAutoScroll(); }),
                         this);
        gtk_widget_add_controller(w_, GTK_EVENT_CONTROLLER(click));
        GtkEventController* motion = gtk_event_controller_motion_new();
        g_signal_connect(motion, "motion", G_CALLBACK(+[](GtkEventControllerMotion*, double, double y, gpointer p)
                                                       { static_cast<WList*>(p)->autoY_ = y; }),
                         this);
        gtk_widget_add_controller(w_, motion);
    }
    void startAutoScroll(double y)
    {
        stopAutoScroll();
        autoY0_ = autoY_ = y;
        autoLast_ = 0;
        autoTick_ = gtk_widget_add_tick_callback(
            w_,
            +[](GtkWidget*, GdkFrameClock* clock, gpointer p) -> gboolean
            {
                auto* self = static_cast<WList*>(p);
                const gint64 now = gdk_frame_clock_get_frame_time(clock);
                const double dt = self->autoLast_ ? (now - self->autoLast_) / 1e6 : 0;
                self->autoLast_ = now;
                GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->outer_));
                const double v = gtk_adjustment_get_value(adj) + autoScrollSpeed(float(self->autoY_ - self->autoY0_)) * dt;
                gtk_adjustment_set_value(adj, std::clamp(v, gtk_adjustment_get_lower(adj),
                                                         gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj)));
                return G_SOURCE_CONTINUE;
            },
            this, nullptr);
        gtk_widget_set_cursor_from_name(w_, "all-scroll");
    }
    void stopAutoScroll()
    {
        if (!autoTick_) return;
        gtk_widget_remove_tick_callback(w_, autoTick_);
        autoTick_ = 0;
        gtk_widget_set_cursor(w_, nullptr);
    }
    guint autoTick_ = 0;
    double autoY0_ = 0, autoY_ = 0;
    gint64 autoLast_ = 0;

    // CellDrag: a drag gesture on the column view. The cell under a point
    // is found by picking the widget there; the target row is framed with
    // a CSS class. (No floating label on gtk4 yet: the cursor and the frame
    // show the drag.)
    void onCellDrag(CellDrag d) override
    {
        if (!cellDrag_.begin)
        {
            GtkGesture* drag = gtk_gesture_drag_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
            g_signal_connect(drag, "drag-begin", G_CALLBACK(+[](GtkGestureDrag* g, double x, double y, gpointer p)
                                                            { static_cast<WList*>(p)->cellDragBegin(g, x, y); }),
                             this);
            g_signal_connect(drag, "drag-update", G_CALLBACK(+[](GtkGestureDrag* g, double dx, double dy, gpointer p)
                                                             { static_cast<WList*>(p)->cellDragUpdate(g, dx, dy); }),
                             this);
            g_signal_connect(drag, "drag-end", G_CALLBACK(+[](GtkGestureDrag*, double, double, gpointer p)
                                                          { static_cast<WList*>(p)->cellDragEnd(true); }),
                             this);
            gtk_widget_add_controller(w_, GTK_EVENT_CONTROLLER(drag));
            GtkEventController* keys = gtk_event_controller_key_new();
            g_signal_connect(keys, "key-pressed",
                             G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType, gpointer p) -> gboolean
                                        {
                                            auto* self = static_cast<WList*>(p);
                                            if (key != GDK_KEY_Escape || !self->cell_.dragging) return false;
                                            self->cellDragEnd(false);
                                            return true;
                                        }),
                             this);
            gtk_widget_add_controller(w_, keys);
        }
        cellDrag_ = std::move(d);
    }

    bool columnVisible(size_t column) const override
    {
        return column < cols_.size() && gtk_column_view_column_get_visible(cols_[column]);
    }
    void setColumnVisible(size_t column, bool v) override
    {
        if (column < cols_.size()) gtk_column_view_column_set_visible(cols_[column], v);
    }

    // ListObserver
    void modelReset() override
    {
        guint old = rows_->count;
        rows_->count = model_ ? guint(model_->rowCount()) : 0;
        g_list_model_items_changed(G_LIST_MODEL(rows_), 0, old, rows_->count);
    }
    // Changed rows are refreshed in the cells showing them. Reporting them
    // to GTK as removed and re-added (items-changed) dropped their
    // selection and rebuilt their widgets, on every label or icon update.
    void rowsChanged(size_t first, size_t n) override
    {
        for (auto [li, ctx] : bound_)
        {
            const size_t row = rowOf(gtk_list_item_get_item(li));
            if (row >= first && row < first + n) bindCell(li, *ctx);
        }
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

    // The cell widget at a point of the column view, its row and column.
    struct CellHit
    {
        GtkWidget* cell = nullptr;
        size_t row = 0, column = 0;
    };
    std::optional<CellHit> cellAt(double x, double y) const
    {
        for (GtkWidget* w = gtk_widget_pick(w_, x, y, GTK_PICK_DEFAULT); w && w != w_; w = gtk_widget_get_parent(w))
        {
            const size_t tagged = rowOf(w);
            auto* ctx = static_cast<ColumnCtx*>(g_object_get_data(G_OBJECT(w), "anyui-ctx"));
            if (tagged && ctx) return CellHit{w, tagged - 1, ctx->column};
        }
        return std::nullopt;
    }
    static GtkWidget* rowWidget(GtkWidget* cell)
    {
        for (GtkWidget* w = cell; w; w = gtk_widget_get_parent(w))
            if (std::string_view(gtk_widget_get_css_name(w)) == "row") return w;
        return nullptr;
    }

    struct CellDragState
    {
        bool pressed = false, dragging = false;
        double x0 = 0, y0 = 0;
        size_t from = 0;
        CellDragStart start;
        std::optional<size_t> target;
        GtkWidget* targetRow = nullptr;  // framed
    };
    void cellDragBegin(GtkGestureDrag* g, double x, double y)
    {
        cell_ = {};
        auto hit = cellAt(x, y);
        std::optional<CellDragStart> start;
        if (hit && model_)
        {
            graphene_point_t in = GRAPHENE_POINT_INIT(float(x), float(y)), out;
            if (!gtk_widget_compute_point(w_, hit->cell, &in, &out)) out = in;
            start = cellDrag_.begin(hit->row, hit->column, PointF{out.x, out.y});
        }
        if (!start)
        {
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);  // an ordinary click
            return;
        }
        cell_ = {true, false, x, y, hit->row, std::move(*start), std::nullopt, nullptr};
    }
    void cellDragUpdate(GtkGestureDrag* g, double dx, double dy)
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
        if (cell_.targetRow) gtk_widget_remove_css_class(cell_.targetRow, "anyui-drop");
        cell_.target.reset();
        cell_.targetRow = nullptr;
        if (auto hit = cellAt(cell_.x0 + dx, cell_.y0 + dy);
            hit && cellDrag_.canDrop && cellDrag_.canDrop(cell_.from, hit->row, cell_.start.data))
        {
            cell_.target = hit->row;
            if ((cell_.targetRow = rowWidget(hit->cell))) gtk_widget_add_css_class(cell_.targetRow, "anyui-drop");
        }
        gtk_widget_set_cursor_from_name(w_, cell_.target ? "grabbing" : "not-allowed");
    }
    void cellDragEnd(bool drop)
    {
        if (!cell_.pressed) return;
        CellDragState c = std::move(cell_);
        cell_ = {};
        if (c.targetRow) gtk_widget_remove_css_class(c.targetRow, "anyui-drop");
        gtk_widget_set_cursor(w_, nullptr);
        if (drop && c.dragging && c.target && cellDrag_.drop) cellDrag_.drop(c.from, *c.target, c.start.data);
    }
    CellDrag cellDrag_;
    CellDragState cell_;

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
        connect(
            factory, "unbind",
            +[](GtkSignalListItemFactory*, GtkListItem* li, gpointer p)
            { static_cast<ColumnCtx*>(p)->list->bound_.erase(li); },
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
        gtk_column_view_column_set_visible(col, c.visible);
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

        g_object_set_data(G_OBJECT(child), "anyui-ctx", &ctx);
        gtk_list_item_set_child(li, child);
    }

    void bindCell(GtkListItem* li, ColumnCtx& ctx)
    {
        if (!model_) return;
        bound_[li] = &ctx;
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
        auto* ctx = static_cast<ColumnCtx*>(g_object_get_data(G_OBJECT(cell), "anyui-ctx"));
        const bool atCell = cellMenu_ && row && ctx;
        if (!contextMenu_ && !atCell) return;

        Menu menu = atCell ? cellMenu_(*row, ctx->column, PointF{float(x), float(y)}) : contextMenu_(row);
        if (menu.empty()) return;

        GMenu* model = ctxMenu_->build(std::move(menu));

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

    AnyuiRows* rows_ = nullptr;  // owned by sel_
    GtkSelectionModel* sel_ = nullptr;
    std::unordered_map<GtkListItem*, ColumnCtx*> bound_;  // cells showing a row now
    std::optional<std::pair<Color, Color>> soft_;  // selection background, text
    std::unique_ptr<MenuActions> ctxMenu_;
    GtkWidget* popover_ = nullptr;
    ListModel* model_ = nullptr;
    std::vector<GtkColumnViewColumn*> cols_;
    std::vector<std::unique_ptr<ColumnCtx>> ctxs_;
    bool suppressSort_ = false;
    bool alive_ = true;

    std::function<void()> selChanged_;
    std::function<void(size_t)> activate_;
    std::function<void(size_t, SortOrder)> sort_;
    std::function<Menu(std::optional<size_t>)> contextMenu_;
    std::function<Menu(size_t)> headerMenu_;
    std::function<Menu(size_t, size_t, PointF)> cellMenu_;
};

// ---------------------------------------------------------------- window

// ---------------------------------------------------------------- canvas

// Paint, resize, cursor and frame callbacks. Written without a GTK 4 build
// to check it against.
unsigned modifiers(GdkModifierType state)
{
    unsigned m = 0;
    if (state & GDK_SHIFT_MASK) m |= ModShift;
    if (state & GDK_CONTROL_MASK) m |= ModCtrl;
    if (state & GDK_ALT_MASK) m |= ModAlt;
    if (state & (GDK_SUPER_MASK | GDK_META_MASK)) m |= ModMeta;
    return m;
}

// Input mirrors the gtk3 WCanvas: presses, releases and the wheel from a
// legacy controller (raw events, so a release after a drag still arrives),
// motion and crossing from a motion controller, keys from a key controller.
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
        focusable_ = s.focusable;

        GtkEventController* legacy = gtk_event_controller_legacy_new();
        connect(legacy, "event", +[](GtkEventControllerLegacy*, GdkEvent* e, gpointer p) -> gboolean
                { return static_cast<WCanvas*>(p)->rawEvent(e); }, this);
        gtk_widget_add_controller(w_, legacy);

        GtkEventController* motion = gtk_event_controller_motion_new();
        connect(motion, "motion", +[](GtkEventControllerMotion* c, double x, double y, gpointer p)
                { static_cast<WCanvas*>(p)->crossing(GTK_EVENT_CONTROLLER(c), PointerEvent::Type::Move, x, y); }, this);
        connect(motion, "enter", +[](GtkEventControllerMotion* c, double x, double y, gpointer p)
                { static_cast<WCanvas*>(p)->crossing(GTK_EVENT_CONTROLLER(c), PointerEvent::Type::Enter, x, y); }, this);
        connect(motion, "leave", +[](GtkEventControllerMotion* c, gpointer p)
                { static_cast<WCanvas*>(p)->crossing(GTK_EVENT_CONTROLLER(c), PointerEvent::Type::Leave, -1, -1); }, this);
        gtk_widget_add_controller(w_, motion);

        GtkEventController* keys = gtk_event_controller_key_new();
        connect(keys, "key-pressed", +[](GtkEventControllerKey*, guint keyval, guint code, GdkModifierType state, gpointer p) -> gboolean
                { return static_cast<WCanvas*>(p)->key(true, keyval, code, state); }, this);
        connect(keys, "key-released", +[](GtkEventControllerKey*, guint keyval, guint code, GdkModifierType state, gpointer p)
                { static_cast<WCanvas*>(p)->key(false, keyval, code, state); }, this);
        gtk_widget_add_controller(w_, keys);
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
    void onPointer(std::function<bool(const PointerEvent&)> f) override { pointer_ = std::move(f); }
    void onScroll(std::function<bool(const ScrollEvent&)> f) override { scroll_ = std::move(f); }
    void onKey(std::function<bool(const KeyEvent&)> f) override { key_ = std::move(f); }

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
    // A raw event's position, from its surface to this widget.
    PointF widgetPos(GdkEvent* e) const
    {
        double x = 0, y = 0;
        gdk_event_get_position(e, &x, &y);
        GtkNative* native = gtk_widget_get_native(w_);
        double sx = 0, sy = 0;
        if (native) gtk_native_get_surface_transform(native, &sx, &sy);
        graphene_point_t in = GRAPHENE_POINT_INIT(float(x - sx), float(y - sy)), out = in;
        if (native) gtk_widget_compute_point(GTK_WIDGET(native), w_, &in, &out);
        return {out.x, out.y};
    }

    bool rawEvent(GdkEvent* e)
    {
        const GdkEventType type = gdk_event_get_event_type(e);
        if (type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE)
        {
            PointerEvent ev{type == GDK_BUTTON_PRESS ? PointerEvent::Type::Press : PointerEvent::Type::Release, widgetPos(e),
                            int(gdk_button_event_get_button(e)), 1, modifiers(gdk_event_get_modifier_state(e))};
            if (ev.type == PointerEvent::Type::Press)
            {
                if (focusable_ && !gtk_widget_has_focus(w_)) gtk_widget_grab_focus(w_);
                // Count clicks ourselves, as gtk3 does, so each press arrives once.
                int time = 400, distance = 5;
                g_object_get(gtk_widget_get_settings(w_), "gtk-double-click-time", &time, "gtk-double-click-distance", &distance,
                             nullptr);
                const guint32 t = gdk_event_get_time(e);
                const bool again = ev.button == lastButton_ && t - lastTime_ <= guint32(time) &&
                                   std::abs(ev.pos.x - lastPos_.x) <= distance && std::abs(ev.pos.y - lastPos_.y) <= distance;
                clicks_ = again ? clicks_ + 1 : 1;
                ev.clicks = clicks_;
                lastButton_ = ev.button;
                lastTime_ = t;
                lastPos_ = ev.pos;
            }
            return pointer_ && pointer_(ev);
        }
        if (type == GDK_SCROLL && scroll_)
        {
            ScrollEvent ev{widgetPos(e), 0, 0, false, modifiers(gdk_event_get_modifier_state(e))};
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

    void crossing(GtkEventController* c, PointerEvent::Type type, double x, double y)
    {
        if (!pointer_) return;
        PointF pos{float(x), float(y)};
        if (type == PointerEvent::Type::Leave) pos = lastMove_;
        pointer_({type, pos, 0, 1, modifiers(gtk_event_controller_get_current_event_state(c))});
        if (type != PointerEvent::Type::Leave) lastMove_ = pos;
    }

    bool key(bool press, guint keyval, guint code, GdkModifierType state)
    {
        if (!key_) return false;
        KeyEvent ev;
        ev.press = press;
        ev.key = keyFor(keyval);
        if (ev.key == Key::Character) ev.codepoint = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
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

    std::function<void(Painter&, SizeF)> paint_;
    std::function<void(SizeF)> resize_;
    std::function<bool(const PointerEvent&)> pointer_;
    std::function<bool(const ScrollEvent&)> scroll_;
    std::function<bool(const KeyEvent&)> key_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::vector<guint> ticks_;
    std::vector<guint> held_;
    bool focusable_ = false;
    int clicks_ = 0, lastButton_ = 0;
    guint32 lastTime_ = 0;
    PointF lastPos_{}, lastMove_{};
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
        gtk_window_set_resizable(win, spec.resizable);
        // Window keys come first (capture phase), except while a text field
        // is being typed in: then only Escape and the F-keys, as on gtk3.
        GtkEventController* keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        connect(keys, "key-pressed", +[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer p) -> gboolean
                { return static_cast<WWindow*>(p)->windowKey(true, keyval, state); }, this);
        connect(keys, "key-released", +[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer p)
                { static_cast<WWindow*>(p)->windowKey(false, keyval, state); }, this);
        gtk_widget_add_controller(win_, keys);
        connect(win_, "notify::fullscreened",
                +[](GObject*, GParamSpec*, gpointer p)
                {
                    auto* self = static_cast<WWindow*>(p);
                    const bool fs = gtk_window_is_fullscreen(GTK_WINDOW(self->win_));
                    if (fs == self->fullscreen_) return;
                    self->fullscreen_ = fs;
                    if (self->menuBar_) self->setMenuBarVisible(self->menuBarWanted_);  // hidden in fullscreen
                    if (self->onFullscreen_) self->onFullscreen_(fs);
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
                static_cast<WWindow*>(p)->closeRequested();
                return true;  // we own the window; never let GTK destroy it
            },
            this);
    }

    ~WWindow() override
    {
        if (lastShown() == win_) lastShown() = nullptr;
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
        snapshotIfAsked();
    }

    // Development aid, as in gtk3: ANYUI_SNAPSHOT=file.png saves the window
    // shown last ANYUI_SNAPSHOT_DELAY ms (default 500) after it appears,
    // then quits the main loop.
    static GtkWidget*& lastShown()
    {
        static GtkWidget* w = nullptr;
        return w;
    }
    static GMainLoop*& mainLoop()
    {
        static GMainLoop* loop = nullptr;
        return loop;
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
                if (GtkWidget* win = lastShown())
                {
                    // The window as GTK last drew it, rendered to a texture.
                    GdkPaintable* paintable = gtk_widget_paintable_new(win);
                    GtkSnapshot* snap = gtk_snapshot_new();
                    gdk_paintable_snapshot(paintable, snap, gtk_widget_get_width(win),
                                           gtk_widget_get_height(win));
                    if (GskRenderNode* node = gtk_snapshot_free_to_node(snap))
                    {
                        GskRenderer* renderer = gtk_native_get_renderer(GTK_NATIVE(win));
                        GdkTexture* tex = gsk_renderer_render_texture(renderer, node, nullptr);
                        gdk_texture_save_to_png(tex, std::getenv("ANYUI_SNAPSHOT"));
                        g_object_unref(tex);
                        gsk_render_node_unref(node);
                    }
                    g_object_unref(paintable);
                }
                if (mainLoop()) g_main_loop_quit(mainLoop());
                return G_SOURCE_REMOVE;
            },
            nullptr);
    }
    void close() override { gtk_widget_set_visible(win_, false); }
    void onFinished(std::function<void(DialogRole)> f) override { onFinished_ = std::move(f); }
    void onCloseRequest(std::function<bool()> f) override { onCloseRequest_ = std::move(f); }

    // TODO(pengit): drawn window icons.
    SizeF size() const override
    {
        return {float(gtk_widget_get_width(win_)), float(gtk_widget_get_height(win_))};
    }
    void resize(SizeF s) override
    {
        // GTK 4 can't place windows, but at least the frame fits the work
        // area (its shadows are what the surface has beyond the window).
        int w = int(s.w), h = int(s.h);
        const RectF wa = workArea();
        if (GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(win_)); surface && wa.w > 0 && wa.h > 0)
        {
            const int extraW = std::max(0, gdk_surface_get_width(surface) - gtk_widget_get_width(win_));
            const int extraH = std::max(0, gdk_surface_get_height(surface) - gtk_widget_get_height(win_));
            w = std::min(w, int(wa.w) - extraW);
            h = std::min(h, int(wa.h) - extraH);
        }
        gtk_window_set_default_size(GTK_WINDOW(win_), std::max(1, w), std::max(1, h));
    }
    void setMinSize(SizeF s) override { gtk_widget_set_size_request(win_, int(s.w), int(s.h)); }
    std::optional<PointF> position() const override { return std::nullopt; }  // GTK 4: never
    void move(PointF) override {}
    RectF workArea() const override
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
        return withoutWindowsTaskbar({float(r.x), float(r.y), float(r.width), float(r.height)});
    }
    bool fullscreen() const override { return gtk_window_is_fullscreen(GTK_WINDOW(win_)); }
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
        if (!icon.name.empty()) gtk_window_set_icon_name(GTK_WINDOW(win_), icon.name.c_str());
    }
    void onKey(std::function<bool(const KeyEvent&)> f) override { onKey_ = std::move(f); }
    void setMenuBarVisible(bool on) override
    {
        menuBarWanted_ = on;
        if (menuBar_) gtk_widget_set_visible(menuBar_, on && !gtk_window_is_fullscreen(GTK_WINDOW(win_)));
    }

    // The menu bar: a row of flat menu buttons, each building its menu when
    // it opens. (GtkPopoverMenuBar wants its whole model up front.)
    GtkWidget* buildMenuBar(const std::vector<MenuBarItem>& menus)
    {
        menuBar_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(menuBar_, "menubar");
        for (const MenuBarItem& m : menus)
        {
            auto bar = std::make_unique<BarMenu>();
            bar->fn = m.menu;
            GtkWidget* button = gtk_menu_button_new();
            gtk_menu_button_set_label(GTK_MENU_BUTTON(button), m.title.c_str());
            gtk_menu_button_set_use_underline(GTK_MENU_BUTTON(button), true);
            gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(button), false);
            gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(button), false);
            bar->actions = std::make_unique<MenuActions>(button, "bar");
            attachMenu(GTK_MENU_BUTTON(button), bar->actions.get(), &bar->fn);
            gtk_box_append(GTK_BOX(menuBar_), button);
            bars_.push_back(std::move(bar));
        }
        return menuBar_;
    }

    void setContent(GtkWidget* content, bool margins, const std::vector<MenuBarItem>& menus)
    {
        int margin = !margins ? 0 : dialog_ ? 12 : 6;
        gtk_widget_set_margin_start(content, margin);
        gtk_widget_set_margin_end(content, margin);
        gtk_widget_set_margin_top(content, margin);
        gtk_widget_set_margin_bottom(content, margin);
        if (menus.empty()) return gtk_window_set_child(GTK_WINDOW(win_), content);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_append(GTK_BOX(box), buildMenuBar(menus));
        gtk_widget_set_vexpand(content, true);
        gtk_box_append(GTK_BOX(box), content);
        gtk_window_set_child(GTK_WINDOW(win_), box);
    }

    bool windowKey(bool press, guint keyval, GdkModifierType state)
    {
        if (!onKey_) return false;
        GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(win_));
        const bool typing = focus && (GTK_IS_EDITABLE(focus) || (GTK_IS_TEXT_VIEW(focus) && gtk_text_view_get_editable(GTK_TEXT_VIEW(focus))));
        const bool notText = keyval == GDK_KEY_Escape || (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12);
        if (typing && !notText) return false;
        KeyEvent ev;
        ev.press = press;
        ev.key = keyFor(keyval);
        if (ev.key == Key::Character) ev.codepoint = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
        ev.modifiers = modifiers(state);
        return onKey_(ev);
    }
    std::function<bool(const KeyEvent&)> onKey_;
    std::function<void(bool)> onFullscreen_;
    bool fullscreen_ = false;

    struct BarMenu
    {
        std::function<Menu()> fn;
        std::unique_ptr<MenuActions> actions;
    };
    GtkWidget* menuBar_ = nullptr;
    std::vector<std::unique_ptr<BarMenu>> bars_;
    bool menuBarWanted_ = true;

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
        for (auto& c : s.items) gtk_box_append(GTK_BOX(box), build(c, s.dir));
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
        // Spare space goes to stretching panes only (as on win32): the rest
        // keep their size when the window grows.
        const bool restStretch = std::any_of(s.panes.begin() + std::ptrdiff_t(from + 1), s.panes.end(),
                                             [](auto& p) { return p.common.stretch > 0; });
        GtkWidget* paned = gtk_paned_new(gtkOrientation(s.dir));
        auto* p = GTK_PANED(paned);
        gtk_paned_set_wide_handle(p, true);
        g_object_set_data(G_OBJECT(paned), "anyui-split-chain", GINT_TO_POINTER(1));
        gtk_paned_set_start_child(p, first);
        gtk_paned_set_end_child(p, panes(s, from + 1));
        gtk_paned_set_resize_start_child(p, !anyStretch || s.panes[from].common.stretch > 0);
        gtk_paned_set_resize_end_child(p, !anyStretch || restStretch);
        gtk_paned_set_shrink_start_child(p, false);
        gtk_paned_set_shrink_end_child(p, false);
        return paned;
    }

    GtkWidget* make(const Node&, const spec::Scroll& s, Orientation)
    {
        GtkWidget* scroller = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                       s.horizontal ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller), true);
        gtk_widget_set_vexpand(scroller, true);
        if (!s.content.empty())
            gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller),
                                          build(s.content.front(), Orientation::Vertical));
        return scroller;
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
        if (s.sidebar) return makeSidebar(n, s);
        GtkWidget* nb = gtk_notebook_new();
        std::vector<GtkWidget*> pages;
        for (size_t i = 0; i < s.pages.size(); ++i)
        {
            const bool lazy = i < s.lazy.size() && s.lazy[i];
            GtkWidget* page = lazy ? gtk_box_new(GTK_ORIENTATION_VERTICAL, 0) : build(s.pages[i], Orientation::Vertical);
            pages.push_back(page);
            gtk_widget_set_margin_start(page, 6);
            gtk_widget_set_margin_end(page, 6);
            gtk_widget_set_margin_top(page, 6);
            gtk_widget_set_margin_bottom(page, 6);
            gtk_notebook_append_page(GTK_NOTEBOOK(nb), page,
                                     gtk_label_new_with_mnemonic(s.titles[i].c_str()));
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
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
            gtk_list_box_append(GTK_LIST_BOX(list), row);
            const bool lazy = i < s.lazy.size() && s.lazy[i];
            GtkWidget* page = lazy ? gtk_box_new(GTK_ORIENTATION_VERTICAL, 0) : build(s.pages[i], Orientation::Vertical);
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

    GtkWidget* make(const Node& n, const spec::Button& s, Orientation)
    {
        auto& w = add<WButton>(n, s.iconOnly ? std::string() : s.text);
        if (s.icon) w.setIcon(s.icon);
        if (s.iconOnly && n.common.tooltip.empty()) w.setTooltip(s.text);
        GtkWidget* b = w.outer();
        if (s.flat) gtk_button_set_has_frame(GTK_BUTTON(b), false);
        return b;
    }

    GtkWidget* make(const Node& n, const spec::ToolButton& s, Orientation)
    {
        return add<WToolButton>(n, s).outer();
    }

    // No GtkToolbar in GTK 4: a box styled as a toolbar.
    GtkWidget* make(const Node&, const spec::Toolbar& s, Orientation)
    {
        GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(bar, "toolbar");
        for (const Node& item : s.items)
        {
            GtkWidget* w = build(item, Orientation::Horizontal);
            if (std::holds_alternative<spec::Spacer>(item.spec)) gtk_widget_set_hexpand(w, true);
            gtk_box_append(GTK_BOX(bar), w);
        }
        return bar;
    }

    GtkWidget* make(const Node& n, const spec::Slider& s, Orientation)
    {
        GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, s.min, s.max,
                                                    s.step > 0 ? s.step : 0.01);
        gtk_scale_set_digits(GTK_SCALE(scale), s.digits);
        gtk_scale_set_draw_value(GTK_SCALE(scale), s.showValue);
        gtk_range_set_value(GTK_RANGE(scale), s.value);
        for (auto& [at, label] : s.marks)
            gtk_scale_add_mark(GTK_SCALE(scale), at, GTK_POS_BOTTOM,
                               label.empty() ? nullptr : label.c_str());
        return add<WValue<anyui::Slider>>(n, scale).outer();
    }

    GtkWidget* make(const Node& n, const spec::Spin& s, Orientation)
    {
        GtkWidget* spin = gtk_spin_button_new_with_range(s.min, s.max, s.step > 0 ? s.step : 1);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), guint(std::max(s.digits, 0)));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), s.value);
        return add<WValue<anyui::Spin>>(n, spin).outer();
    }

    GtkWidget* make(const Node& n, const spec::ColorButton& s, Orientation)
    {
        return add<WColorButton>(n, s).outer();
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
        // Under WSLg, X11 draws faster than its Wayland (measured: 23 against
        // 20 fps for a panning canvas; gtk3 39 against 29).
        if (underWsl() && !std::getenv("GDK_BACKEND")) gdk_set_allowed_backends("x11,*");
        if (!gtk_init_check()) return nullptr;
        initMetrics();
        return new Gtk4Backend;
    }

    std::string_view name() const override { return "gtk4"; }

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
        loop_ = g_main_loop_new(nullptr, false);
        WWindow::mainLoop() = loop_;
        g_main_loop_run(loop_);
        WWindow::mainLoop() = nullptr;
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

    // A save in progress. When the backend added the extension and that
    // file exists, GTK's own overwrite check saw only the name as typed, so
    // replacing is asked here; Cancel reopens the dialog on the typed name.
    struct SaveState
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
                                     static_cast<SaveState*>(p)->saved(
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
            const std::string path = withExtension(typed, ext);
            if (path == typed || !g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) return finish({path});
            gchar* name = g_path_get_basename(path.c_str());
            GtkAlertDialog* alert =
                gtk_alert_dialog_new("A file named \u201c%s\u201d already exists. Replace it?", name);
            g_free(name);
            const char* buttons[] = {"_Cancel", "_Replace", nullptr};
            gtk_alert_dialog_set_buttons(alert, buttons);
            gtk_alert_dialog_set_cancel_button(alert, 0);
            gtk_alert_dialog_set_default_button(alert, 0);
            pending_ = {typed, path};
            gtk_alert_dialog_choose(alert, parent, nullptr,
                                    +[](GObject* a, GAsyncResult* r, gpointer p)
                                    {
                                        auto* self = static_cast<SaveState*>(p);
                                        const int b = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(a), r, nullptr);
                                        if (b == 1) return self->finish({self->pending_.second});
                                        GFile* again = g_file_new_for_path(self->pending_.first.c_str());
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
        std::pair<std::string, std::string> pending_;  // typed, with extension
    };

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
        GtkWindow* pw = parentWindow(parent);
        if (f.mode == FileDialogSpec::Mode::Save)
        {
            // GTK 4 doesn't say which filter the user picked: the first's
            // extension goes on a name typed without one.
            auto* save = new SaveState{dlg, pw, f.filters.empty() ? "" : defaultExtension(f.filters.front().pattern),
                                       std::move(done)};
            g_object_ref(dlg);
            save->run();
            g_object_unref(dlg);
            return;
        }
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

        switch (f.mode)
        {
        case FileDialogSpec::Mode::Open:
            gtk_file_dialog_open(
                dlg, pw, nullptr,
                +[](GObject* s, GAsyncResult* r, gpointer p)
                { single(gtk_file_dialog_open_finish(GTK_FILE_DIALOG(s), r, nullptr), p); },
                ctx);
            break;
        case FileDialogSpec::Mode::Save: break;  // above
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

    // TODO(pengit): a GdkTexture on the clipboard.
    void setClipboardImage(int, int, PixelFormat, const uint8_t*, size_t) override {}

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
