// Declarative layout description.
//
// Apps write a tree of builders; it becomes a tree of plain Nodes that the
// backend turns into native widgets and native layouts. No pixel positions:
// boxes, forms and splitters, with semantic sizes and stretch.
//
//     using namespace rap::layout;
//     auto dlg = ui.create(Dialog{"Rebase", {
//         VBox{
//             Form{
//                 {"Branch",   Label{}.bind(branch_name)},
//                 {"Upstream", Combo{}.bind(upstream)},
//             },
//             Splitter{Vertical,
//                 List{columns, &model}.bind(commit_list).stretch(),
//                 text_view{}.read_only().monospace().bind(message),
//             }.stretch(),
//             HBox{
//                 Check{"Preserve merges"}.bind(preserve_merges),
//                 Spacer{},
//                 Button{"Up"}.on_click(moveUp),
//                 Button{"Down"}.on_click(moveDown),
//             },
//             Buttons{ok("Start rebase"), Cancel},
//         }
//     }});
//
// Responsibilities when a window is created:
//   backend - builds every node with its content (text, items, columns,
//             flags), layout (stretch, width/height) and dialog-button roles;
//   core    - afterwards applies enabled/tooltip, the list model, every
//             callback, and fills bind() handles through the widget
//             interfaces. Backends never need to look at callbacks or binds.
#pragma once

#include "widgets.h"

#include <initializer_list>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rap
{

struct node;
using nodes = std::vector<node>;

struct common
{
    int stretch = 0;  // share of spare space along the parent box/splitter
    extent width, height;
    bool enabled = true;
    bool visible = true;  // false: created hidden (shown later with set_visible)
    std::string tooltip;
    // Filled with the node's widget, for hiding/disabling any node, boxes
    // and toolbars included (they have no interface of their own).
    widget** handle = nullptr;
};

namespace spec
{

struct box
{
    orientation dir;
    nodes items;
    // The usual window margin around the items, for a bar along the edge of
    // a window that has margins off (a status line): the full margin at its
    // ends and half of it along its sides.
    bool margins = false;
    // Set by layout::deferred: builds the box's one item later (the backend
    // decides when), in place of `items`.
    std::function<node()> deferred;
};
// Label/field pairs; the backend applies its own label alignment rules.
// An empty label makes the field span both columns.
struct form
{
    std::vector<std::string> labels;
    nodes fields;
};
struct splitter
{
    orientation dir;  // direction the panes are laid out in
    nodes panes;
    rap::splitter** bind = nullptr;
};
struct group
{
    std::string title;
    nodes content;  // exactly one
};
// Scrolls its content when it doesn't fit (a long settings page).
struct scroll
{
    nodes content;            // exactly one
    bool horizontal = false;  // also scroll sideways; otherwise the content's width is kept
};
struct tabs
{
    std::vector<std::string> titles;
    nodes pages;  // a lazy page's entry is an empty box
    // Per page: how to build it the first time it's shown (layout::lazy),
    // or empty for one built with the window. Empty if no page is lazy.
    std::vector<std::function<node()>> lazy;
    // The page names as a list down the left with the page beside it (a
    // settings dialog) rather than tabs along the top.
    bool sidebar = false;
    std::function<void(int)> on_changed;
    rap::tabs** bind = nullptr;
};
struct spacer
{
};
struct separator
{
};  // line across the parent box's direction

struct label
{
    std::string text;
    bool selectable = false;
    // Cut long text with "..." instead of widening the window to fit it
    // (status lines).
    bool ellipsize = false;
    // A hyperlink that runs on_click (TortoiseGit's "Check: All None ..."
    // links, a branch name that opens Switch): each toolkit's own link.
    bool link = false;
    std::function<void()> on_click;
    // Drawn before the text; on a link it's part of the link (its click and
    // pointer), so an icon and its words read as one.
    rap::icon icon;
    rap::label** bind = nullptr;
};
struct button
{
    std::string text;
    bool flat = false;  // no border until hovered (a link-like button)
    rap::icon icon;
    bool icon_only = false;  // show the icon; the text becomes the tooltip/accessible name
    std::function<void()> on_click;
    rap::button** bind = nullptr;
};
// Row of tool items: tool_button nodes, Separator and Spacer.
struct toolbar
{
    nodes items;
};
struct tool_button
{
    rap::icon icon;
    std::string
        label;  // shown beside/under the icon only if the toolkit does; always the accessible name
    bool toggle = false;
    bool checked = false;
    std::function<void()> on_click;
    std::function<void(bool)> on_toggled;
    rap::tool_button** bind = nullptr;
};
struct slider
{
    double min = 0, max = 1, value = 0;
    double step = 0.01, page = 0.1;  // arrow keys / page keys and wheel
    int digits = 2;                  // shown value's decimals (if shown)
    bool show_value = false;
    std::vector<std::pair<double, std::string>> marks;  // ticks, optional labels
    std::function<void(double)> on_changed;
    rap::slider** bind = nullptr;
};
struct spin
{
    double min = 0, max = 100, value = 0, step = 1;
    int digits = 0;
    std::function<void(double)> on_changed;
    rap::spin** bind = nullptr;
};
struct color_button
{
    rap::color color;
    bool alpha = false;  // let the chooser edit alpha
    std::string title;   // chooser dialog title
    std::function<void(rap::color)> on_changed;
    rap::color_button** bind = nullptr;
};
struct check
{
    std::string text;
    check_state state = check_state::unchecked;
    std::function<void(bool)> on_toggled;
    rap::check** bind = nullptr;
};
struct entry
{
    std::string text, placeholder;
    bool read_only = false;
    bool password = false;  // shows dots for the characters typed
    bool frame = true;
    std::function<void(const std::string&)> on_changed;
    std::function<void()> on_activate;
    rap::entry** bind = nullptr;
};
struct drop_down
{
    std::vector<std::string> items;
    int current = 0;
    std::function<void(int)> on_changed;
    rap::drop_down** bind = nullptr;
};
struct combo
{
    std::vector<std::string> items;
    std::string text;
    std::function<void(const std::string&)> on_changed;
    std::function<void()> on_activate;
    rap::combo** bind = nullptr;
};
struct text_view
{
    std::string text;
    bool read_only = false;
    bool monospace = false;
    bool wrap = true;
    bool compact = false;  // the same denser font as compact lists
    std::function<void()> on_changed;
    rap::text_view** bind = nullptr;
};
struct list
{
    std::vector<column> columns;
    list_model* model = nullptr;
    selection_mode selection = selection_mode::multi;
    bool headers = true;
    // Denser rows: a slightly smaller font and less cell padding, closer
    // to Windows list views than the toolkit default.
    bool compact = false;
    // Selected rows in a light tint of the accent colour with normal text
    // (as Windows list views do) instead of the toolkit's solid highlight.
    // Painted cells see the same colours through theme_color::highlight and
    // theme_color::highlight_text.
    bool soft_selection = false;
    // Hold the middle button and move away from where it was pressed: the
    // list scrolls up or down at a speed that grows with the distance (a
    // small dead zone around the anchor, which is marked while held).
    // Release, or Esc, stops. Opt-in, for long lists (a history).
    bool auto_scroll = false;
    // A checkbox in each row's first cell: list_model::checked and
    // toggle_checked.
    bool checkboxes = false;
    std::function<void()> on_selection_changed;
    std::function<void(size_t)> on_activate;
    std::function<void(size_t, sort_order)> on_sort_requested;
    std::function<menu(std::optional<size_t>)> on_context_menu;
    std::function<menu(size_t)> on_header_menu;
    rap::list::reorder_fn on_reorder;
    rap::list::cell_drag on_cell_drag;
    std::function<menu(size_t, size_t, point_f)> on_cell_context_menu;
    rap::list** bind = nullptr;
};
struct menu_button
{
    std::string text;
    rap::icon icon;
    bool pushed = false;
    std::function<menu()> on_menu;
    rap::menu_button** bind = nullptr;
};
struct progress_bar
{
    rap::progress_bar** bind = nullptr;
};
struct date_picker
{
    rap::date date;
    std::function<void(rap::date)> on_changed;
    rap::date_picker** bind = nullptr;
};

// App-drawn surface; see rap::canvas.
struct canvas
{
    bool focusable = true;  // takes keyboard focus on click (needed for on_key)
    std::function<void(rap::painter&, rap::size_f)> on_paint;
    std::function<void(size_f)> on_resize;
    std::function<bool(const pointer_event&)> on_pointer;
    std::function<bool(const scroll_event&)> on_scroll;
    std::function<bool(const key_event&)> on_key;
    rap::canvas** bind = nullptr;
};

// Without on_click, a dialog button ends the dialog with its role.
struct dialog_button
{
    dialog_role role = dialog_role::none;
    std::string text;  // empty = the toolkit's stock label for the role
    std::function<void()> on_click;
    rap::button** bind = nullptr;
};
// Row of dialog buttons; the backend orders them by platform convention.
struct buttons
{
    std::vector<dialog_button> buttons;
};

}  // namespace spec

using node_spec =
    std::variant<spec::box, spec::form, spec::splitter, spec::group, spec::scroll, spec::tabs,
                 spec::spacer, spec::separator, spec::label, spec::button, spec::check, spec::entry,
                 spec::drop_down, spec::combo, spec::text_view, spec::list, spec::canvas,
                 spec::menu_button, spec::progress_bar, spec::date_picker, spec::toolbar,
                 spec::tool_button, spec::slider, spec::spin, spec::color_button, spec::buttons>;

struct node
{
    node_spec spec;
    rap::common common;
};

struct window_spec
{
    std::string title;
    bool dialog = false;  // has dialog buttons; modal to its parent unless `modal` is false
    // A dialog that leaves its parent usable (a find bar, a tool palette, a
    // save dialog previewing into the main view). It still stays above it.
    bool modal = true;
    // Off for a window whose content has one right size (a small dialog):
    // no resize border, no maximise.
    bool resizable = true;
    node content;
    extent width, height;
    // The usual space between the window edge and its content; off for
    // edge-to-edge content (an image viewer, a canvas-only window).
    bool margins = true;
    // Menus across the top, outside the margins. Item shortcuts are display
    // hints: the window's on_key handles the keys.
    std::vector<menu_bar_item> menu_bar;
};

// -- builders -----------------------------------------------------------------

namespace layout
{

using rap::orientation;
inline constexpr orientation horizontal = orientation::horizontal;
inline constexpr orientation vertical = orientation::vertical;
using namespace rap::literals;

template <class Self, class S>
class builder
{
public:
    Self& stretch(int factor = 1)
    {
        node_.common.stretch = factor;
        return self();
    }
    Self& width(extent w)
    {
        node_.common.width = w;
        return self();
    }
    Self& height(extent h)
    {
        node_.common.height = h;
        return self();
    }
    Self& enabled(bool e)
    {
        node_.common.enabled = e;
        return self();
    }
    // Created hidden: a part of a window shown only in some state (a field
    // for one choice of a drop-down), shown later with set_visible.
    Self& visible(bool v)
    {
        node_.common.visible = v;
        return self();
    }
    Self& tooltip(std::string t)
    {
        node_.common.tooltip = std::move(t);
        return self();
    }
    // The node as a plain Widget (set_visible, set_enabled...), whatever it is.
    Self& handle(widget*& h)
    {
        node_.common.handle = &h;
        return self();
    }

    operator node() const& { return node_; }
    operator node() && { return std::move(node_); }

protected:
    builder()
        : node_{S{}, {}}
    {
    }
    S& s() { return std::get<S>(node_.spec); }
    Self& self() { return static_cast<Self&>(*this); }

private:
    node node_;
};

class vbox : public builder<vbox, spec::box>
{
public:
    vbox(std::initializer_list<node> items) { s() = {vertical, items, false, {}}; }
    vbox& margins(bool on = true)
    {
        s().margins = on;
        return *this;
    }
};

class hbox : public builder<hbox, spec::box>
{
public:
    hbox(std::initializer_list<node> items) { s() = {horizontal, items, false, {}}; }
    hbox& margins(bool on = true)
    {
        s().margins = on;
        return *this;
    }
};

// A box whose content is built after the window first appears, when the UI
// is next idle, or at once if the box is shown before then: for a part of
// the window that starts hidden and is slow to make (a big list). Backends
// that make controls cheaply build it with the window. The contract:
// - Its widgets, and so its binds and callbacks, may not exist yet: code
//   that reads them checks. Content fed from a model needs no care, as it
//   reads the model when built.
// - Until built it measures nothing; give it a size if that matters.
// A vertical box; its handle (common.handle) shows and hides it as usual.
class deferred : public builder<deferred, spec::box>
{
public:
    explicit deferred(std::function<node()> build) { s() = {vertical, {}, false, std::move(build)}; }
};

struct form_row
{
    std::string label;
    node field;
};

class form : public builder<form, spec::form>
{
public:
    form(std::initializer_list<form_row> rows)
    {
        for (auto& r : rows)
        {
            s().labels.push_back(r.label);
            s().fields.push_back(r.field);
        }
    }
};

class splitter : public builder<splitter, spec::splitter>
{
public:
    template <class... Panes>
    splitter(orientation dir, Panes&&... panes)
    {
        s().dir = dir;
        (s().panes.push_back(node(std::forward<Panes>(panes))), ...);
    }
    splitter& bind(rap::splitter*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class group : public builder<group, spec::group>
{
public:
    group(std::string title, node content)
    {
        s().title = std::move(title);
        s().content.push_back(std::move(content));
    }
};

class scroll : public builder<scroll, spec::scroll>
{
public:
    explicit scroll(node content) { s().content.push_back(std::move(content)); }
    scroll& horizontal(bool v = true)
    {
        s().horizontal = v;
        return *this;
    }
};

// A tab page built the first time it's shown rather than with the window,
// for dialogs whose pages hold many controls. The contract:
// - Its widgets, and so its binds and callbacks, exist only from then on.
//   Code that reads them (an OK handler) checks whether the page was built.
// - It doesn't count toward the window's natural size: it gets the tab
//   area that the window's size and the other pages give it. Size the window
//   (or the Tabs), and put content that might not fit in a Scroll.
struct lazy
{
    std::function<node()> build;
};

struct page
{
    page(std::string title, node content)
        : title(std::move(title))
        , content(std::move(content))
    {
    }
    page(std::string title, lazy lazy)
        : title(std::move(title))
        , build(std::move(lazy.build))
    {
    }
    std::string title;
    node content;
    std::function<node()> build;
};

class tabs : public builder<tabs, spec::tabs>
{
public:
    tabs(std::initializer_list<page> pages)
    {
        bool any_lazy = false;
        for (auto& p : pages) any_lazy |= bool(p.build);
        for (auto& p : pages)
        {
            s().titles.push_back(p.title);
            s().pages.push_back(p.build ? node{spec::box{vertical, {}, false, {}}, {}} : p.content);
            if (any_lazy) s().lazy.push_back(p.build);
        }
    }
    tabs& on_changed(std::function<void(int)> f)
    {
        s().on_changed = std::move(f);
        return *this;
    }
    tabs& sidebar(bool on = true)
    {
        s().sidebar = on;
        return *this;
    }
    tabs& bind(rap::tabs*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class spacer : public builder<spacer, spec::spacer>
{
public:
    spacer() { stretch(); }
};

class separator : public builder<separator, spec::separator>
{
public:
    separator() = default;
};

class label : public builder<label, spec::label>
{
public:
    explicit label(std::string text = {}) { s().text = std::move(text); }
    label& selectable(bool v = true)
    {
        s().selectable = v;
        return *this;
    }
    label& ellipsize(bool v = true)
    {
        s().ellipsize = v;
        return *this;
    }
    label& link(std::function<void()> on_click)
    {
        s().link = true;
        s().on_click = std::move(on_click);
        return *this;
    }
    label& icon(rap::icon i)
    {
        s().icon = std::move(i);
        return *this;
    }
    label& bind(rap::label*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class button : public builder<button, spec::button>
{
public:
    explicit button(std::string text) { s().text = std::move(text); }
    button& flat(bool v = true)
    {
        s().flat = v;
        return *this;
    }
    button& icon(icon i)
    {
        s().icon = std::move(i);
        return *this;
    }
    // Icon only; the text stays as tooltip and accessible name.
    button& icon_only(bool v = true)
    {
        s().icon_only = v;
        return *this;
    }
    button& on_click(std::function<void()> f)
    {
        s().on_click = std::move(f);
        return *this;
    }
    button& bind(rap::button*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class check : public builder<check, spec::check>
{
public:
    explicit check(std::string text, bool checked = false)
    {
        s().text = std::move(text);
        s().state = checked ? check_state::checked : check_state::unchecked;
    }
    check(std::string text, check_state state)
    {
        s().text = std::move(text);
        s().state = state;
    }
    check& on_toggled(std::function<void(bool)> f)
    {
        s().on_toggled = std::move(f);
        return *this;
    }
    check& bind(rap::check*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class entry : public builder<entry, spec::entry>
{
public:
    explicit entry(std::string text = {}) { s().text = std::move(text); }
    entry& read_only(bool v = true)
    {
        s().read_only = v;
        return *this;
    }
    // Without the toolkit's frame: a read-only entry then reads as text that
    // can still be selected, as TortoiseGit shows the branch it commits to.
    entry& frame(bool v = true)
    {
        s().frame = v;
        return *this;
    }
    // Hides what's typed (a password, a passphrase): the toolkit's own
    // password field, which also keeps it from the clipboard.
    entry& password(bool v = true)
    {
        s().password = v;
        return *this;
    }
    entry& placeholder(std::string p)
    {
        s().placeholder = std::move(p);
        return *this;
    }
    entry& on_changed(std::function<void(const std::string&)> f)
    {
        s().on_changed = std::move(f);
        return *this;
    }
    entry& on_activate(std::function<void()> f)
    {
        s().on_activate = std::move(f);
        return *this;
    }
    entry& bind(rap::entry*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class drop_down : public builder<drop_down, spec::drop_down>
{
public:
    explicit drop_down(std::vector<std::string> items = {}, int current = 0)
    {
        s().items = std::move(items);
        s().current = current;
    }
    drop_down& on_changed(std::function<void(int)> f)
    {
        s().on_changed = std::move(f);
        return *this;
    }
    drop_down& bind(rap::drop_down*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class combo : public builder<combo, spec::combo>
{
public:
    explicit combo(std::vector<std::string> items = {}, std::string text = {})
    {
        s().items = std::move(items);
        s().text = std::move(text);
    }
    combo& on_changed(std::function<void(const std::string&)> f)
    {
        s().on_changed = std::move(f);
        return *this;
    }
    combo& on_activate(std::function<void()> f)
    {
        s().on_activate = std::move(f);
        return *this;
    }
    combo& bind(rap::combo*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class text_view : public builder<text_view, spec::text_view>
{
public:
    explicit text_view(std::string text = {}) { s().text = std::move(text); }
    text_view& read_only(bool v = true)
    {
        s().read_only = v;
        return *this;
    }
    text_view& compact(bool v = true)
    {
        s().compact = v;
        return *this;
    }
    text_view& monospace(bool v = true)
    {
        s().monospace = v;
        return *this;
    }
    text_view& wrap(bool v)
    {
        s().wrap = v;
        return *this;
    }
    text_view& on_changed(std::function<void()> f)
    {
        s().on_changed = std::move(f);
        return *this;
    }
    text_view& bind(rap::text_view*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class list : public builder<list, spec::list>
{
public:
    list(std::vector<column> columns, list_model* model)
    {
        s().columns = std::move(columns);
        s().model = model;
    }
    list& selection(selection_mode m)
    {
        s().selection = m;
        return *this;
    }
    list& checkboxes(bool v = true)
    {
        s().checkboxes = v;
        return *this;
    }
    list& soft_selection(bool v = true)
    {
        s().soft_selection = v;
        return *this;
    }
    list& auto_scroll(bool v = true)
    {
        s().auto_scroll = v;
        return *this;
    }
    list& compact(bool v = true)
    {
        s().compact = v;
        return *this;
    }
    list& headers(bool v)
    {
        s().headers = v;
        return *this;
    }
    list& on_selection_changed(std::function<void()> f)
    {
        s().on_selection_changed = std::move(f);
        return *this;
    }
    list& on_activate(std::function<void(size_t)> f)
    {
        s().on_activate = std::move(f);
        return *this;
    }
    list& on_sort_requested(std::function<void(size_t, sort_order)> f)
    {
        s().on_sort_requested = std::move(f);
        return *this;
    }
    list& on_context_menu(std::function<menu(std::optional<size_t>)> f)
    {
        s().on_context_menu = std::move(f);
        return *this;
    }
    list& on_reorder(rap::list::reorder_fn f)
    {
        s().on_reorder = std::move(f);
        return *this;
    }
    list& on_cell_drag(rap::list::cell_drag d)
    {
        s().on_cell_drag = std::move(d);
        return *this;
    }
    list& on_cell_context_menu(std::function<menu(size_t, size_t, point_f)> f)
    {
        s().on_cell_context_menu = std::move(f);
        return *this;
    }
    list& on_header_menu(std::function<menu(size_t)> f)
    {
        s().on_header_menu = std::move(f);
        return *this;
    }
    list& bind(rap::list*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class menu_button : public builder<menu_button, spec::menu_button>
{
public:
    explicit menu_button(std::string text) { s().text = std::move(text); }
    menu_button& icon(icon i)
    {
        s().icon = std::move(i);
        return *this;
    }
    menu_button& pushed(bool v = true)
    {
        s().pushed = v;
        return *this;
    }
    menu_button& on_menu(std::function<menu()> f)
    {
        s().on_menu = std::move(f);
        return *this;
    }
    menu_button& bind(rap::menu_button*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class progress_bar : public builder<progress_bar, spec::progress_bar>
{
public:
    progress_bar() = default;
    progress_bar& bind(rap::progress_bar*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class date_picker : public builder<date_picker, spec::date_picker>
{
public:
    explicit date_picker(date date = {}) { s().date = date; }
    date_picker& on_changed(std::function<void(date)> f)
    {
        s().on_changed = std::move(f);
        return *this;
    }
    date_picker& bind(rap::date_picker*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class canvas : public builder<canvas, spec::canvas>
{
public:
    canvas() = default;
    canvas& focusable(bool v)
    {
        s().focusable = v;
        return *this;
    }
    canvas& on_paint(std::function<void(rap::painter&, rap::size_f)> f)
    {
        s().on_paint = std::move(f);
        return *this;
    }
    canvas& on_resize(std::function<void(size_f)> f)
    {
        s().on_resize = std::move(f);
        return *this;
    }
    canvas& on_pointer(std::function<bool(const pointer_event&)> f)
    {
        s().on_pointer = std::move(f);
        return *this;
    }
    canvas& on_scroll(std::function<bool(const scroll_event&)> f)
    {
        s().on_scroll = std::move(f);
        return *this;
    }
    canvas& on_key(std::function<bool(const key_event&)> f)
    {
        s().on_key = std::move(f);
        return *this;
    }
    canvas& bind(rap::canvas*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class toolbar : public builder<toolbar, spec::toolbar>
{
public:
    toolbar(std::initializer_list<node> items) { s().items = items; }
};

class tool_button : public builder<tool_button, spec::tool_button>
{
public:
    tool_button(icon icon, std::string label)
    {
        s().icon = std::move(icon);
        s().label = std::move(label);
    }
    // A toggle: stays pressed while checked.
    tool_button& toggle(bool checked = false)
    {
        s().toggle = true;
        s().checked = checked;
        return *this;
    }
    tool_button& on_click(std::function<void()> f)
    {
        s().on_click = std::move(f);
        return *this;
    }
    tool_button& on_toggled(std::function<void(bool)> f)
    {
        s().on_toggled = std::move(f);
        return *this;
    }
    tool_button& bind(rap::tool_button*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class slider : public builder<slider, spec::slider>
{
public:
    slider(double min, double max, double value)
    {
        s().min = min;
        s().max = max;
        s().value = value;
    }
    slider& step(double step, double page)
    {
        s().step = step;
        s().page = page;
        return *this;
    }
    slider& show_value(int digits)
    {
        s().show_value = true;
        s().digits = digits;
        return *this;
    }
    slider& mark(double at, std::string label = {})
    {
        s().marks.emplace_back(at, std::move(label));
        return *this;
    }
    slider& on_changed(std::function<void(double)> f)
    {
        s().on_changed = std::move(f);
        return *this;
    }
    slider& bind(rap::slider*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class spin : public builder<spin, spec::spin>
{
public:
    spin(double min, double max, double value, double step = 1, int digits = 0)
    {
        s().min = min;
        s().max = max;
        s().value = value;
        s().step = step;
        s().digits = digits;
    }
    spin& on_changed(std::function<void(double)> f)
    {
        s().on_changed = std::move(f);
        return *this;
    }
    spin& bind(rap::spin*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class color_button : public builder<color_button, spec::color_button>
{
public:
    explicit color_button(color c, std::string title = {})
    {
        s().color = c;
        s().title = std::move(title);
    }
    color_button& alpha(bool v = true)
    {
        s().alpha = v;
        return *this;
    }
    color_button& on_changed(std::function<void(color)> f)
    {
        s().on_changed = std::move(f);
        return *this;
    }
    color_button& bind(rap::color_button*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class dialog_button
{
public:
    explicit dialog_button(dialog_role role, std::string text = {})
    {
        b_.role = role;
        b_.text = std::move(text);
    }
    dialog_button& on_click(std::function<void()> f)
    {
        b_.on_click = std::move(f);
        return *this;
    }
    dialog_button& bind(rap::button*& h)
    {
        b_.bind = &h;
        return *this;
    }
    operator spec::dialog_button() const { return b_; }

private:
    spec::dialog_button b_;
};

// `Ok` on its own, or `ok("Start rebase")` for a custom label.
struct role_tag
{
    dialog_role role;
    dialog_button operator()(std::string text = {}) const
    {
        return dialog_button{role, std::move(text)};
    }
    operator spec::dialog_button() const { return dialog_button{role}; }
};
inline constexpr role_tag ok{dialog_role::ok}, cancel{dialog_role::cancel}, apply{dialog_role::apply},
    close{dialog_role::close}, yes{dialog_role::yes}, no{dialog_role::no}, help{dialog_role::help},
    reset{dialog_role::reset};

class buttons : public builder<buttons, spec::buttons>
{
public:
    buttons(std::initializer_list<spec::dialog_button> buttons) { s().buttons = buttons; }
};

namespace detail
{
inline node single(std::initializer_list<node> content)
{
    if (content.size() == 1) return *content.begin();
    return vbox(content);
}
}  // namespace detail

class window
{
public:
    window(std::string title, std::initializer_list<node> content)
    {
        w_.title = std::move(title);
        w_.content = detail::single(content);
    }
    window& size(extent w, extent h)
    {
        w_.width = w;
        w_.height = h;
        return *this;
    }
    window& resizable(bool on)
    {
        w_.resizable = on;
        return *this;
    }
    window& margins(bool on)
    {
        w_.margins = on;
        return *this;
    }
    window& menu_bar(std::vector<menu_bar_item> menus)
    {
        w_.menu_bar = std::move(menus);
        return *this;
    }
    operator window_spec() const& { return w_; }
    operator window_spec() && { return std::move(w_); }

protected:
    window_spec w_;
};

class dialog : public window
{
public:
    dialog(std::string title, std::initializer_list<node> content)
        : window(std::move(title), content)
    {
        w_.dialog = true;
    }
    dialog& size(extent w, extent h)
    {
        window::size(w, h);
        return *this;
    }
    dialog& modeless()
    {
        w_.modal = false;
        return *this;
    }
    dialog& resizable(bool on)
    {
        window::resizable(on);
        return *this;
    }
};

}  // namespace layout
}  // namespace rap
