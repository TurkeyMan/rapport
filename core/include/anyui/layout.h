// Declarative layout description.
//
// Apps write a tree of builders; it becomes a tree of plain Nodes that the
// backend turns into native widgets and native layouts. No pixel positions:
// boxes, forms and splitters, with semantic sizes and stretch.
//
//     using namespace anyui::layout;
//     auto dlg = ui.create(Dialog{"Rebase", {
//         VBox{
//             Form{
//                 {"Branch",   Label{}.bind(branchName)},
//                 {"Upstream", Combo{}.bind(upstream)},
//             },
//             Splitter{Vertical,
//                 List{columns, &model}.bind(commitList).stretch(),
//                 TextView{}.readOnly().monospace().bind(message),
//             }.stretch(),
//             HBox{
//                 Check{"Preserve merges"}.bind(preserveMerges),
//                 Spacer{},
//                 Button{"Up"}.onClick(moveUp),
//                 Button{"Down"}.onClick(moveDown),
//             },
//             Buttons{Ok("Start rebase"), Cancel},
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

namespace anyui
{

struct Node;
using Nodes = std::vector<Node>;

struct Common
{
    int stretch = 0;  // share of spare space along the parent box/splitter
    Extent width, height;
    bool enabled = true;
    std::string tooltip;
    // Filled with the node's widget, for hiding/disabling any node, boxes
    // and toolbars included (they have no interface of their own).
    Widget** handle = nullptr;
};

namespace spec
{

struct Box
{
    Orientation dir;
    Nodes items;
};
// Label/field pairs; the backend applies its own label alignment rules.
// An empty label makes the field span both columns.
struct Form
{
    std::vector<std::string> labels;
    Nodes fields;
};
struct Splitter
{
    Orientation dir;  // direction the panes are laid out in
    Nodes panes;
    anyui::Splitter** bind = nullptr;
};
struct Group
{
    std::string title;
    Nodes content;  // exactly one
};
// Scrolls its content when it doesn't fit (a long settings page).
struct Scroll
{
    Nodes content;            // exactly one
    bool horizontal = false;  // also scroll sideways; otherwise the content's width is kept
};
struct Tabs
{
    std::vector<std::string> titles;
    Nodes pages;
    std::function<void(int)> onChanged;
    anyui::Tabs** bind = nullptr;
};
struct Spacer
{
};
struct Separator
{
};  // line across the parent box's direction

struct Label
{
    std::string text;
    bool selectable = false;
    // Cut long text with "..." instead of widening the window to fit it
    // (status lines).
    bool ellipsize = false;
    anyui::Label** bind = nullptr;
};
struct Button
{
    std::string text;
    bool flat = false;  // no border until hovered (a link-like button)
    Icon icon;
    bool iconOnly = false;  // show the icon; the text becomes the tooltip/accessible name
    std::function<void()> onClick;
    anyui::Button** bind = nullptr;
};
// Row of tool items: ToolButton nodes, Separator and Spacer.
struct Toolbar
{
    Nodes items;
};
struct ToolButton
{
    Icon icon;
    std::string
        label;  // shown beside/under the icon only if the toolkit does; always the accessible name
    bool toggle = false;
    bool checked = false;
    std::function<void()> onClick;
    std::function<void(bool)> onToggled;
    anyui::ToolButton** bind = nullptr;
};
struct Slider
{
    double min = 0, max = 1, value = 0;
    double step = 0.01, page = 0.1;  // arrow keys / page keys and wheel
    int digits = 2;                  // shown value's decimals (if shown)
    bool showValue = false;
    std::vector<std::pair<double, std::string>> marks;  // ticks, optional labels
    std::function<void(double)> onChanged;
    anyui::Slider** bind = nullptr;
};
struct Spin
{
    double min = 0, max = 100, value = 0, step = 1;
    int digits = 0;
    std::function<void(double)> onChanged;
    anyui::Spin** bind = nullptr;
};
struct ColorButton
{
    Color color;
    bool alpha = false;  // let the chooser edit alpha
    std::string title;   // chooser dialog title
    std::function<void(Color)> onChanged;
    anyui::ColorButton** bind = nullptr;
};
struct Check
{
    std::string text;
    CheckState state = CheckState::Unchecked;
    std::function<void(bool)> onToggled;
    anyui::Check** bind = nullptr;
};
struct Entry
{
    std::string text, placeholder;
    std::function<void(const std::string&)> onChanged;
    std::function<void()> onActivate;
    anyui::Entry** bind = nullptr;
};
struct Select
{
    std::vector<std::string> items;
    int current = 0;
    std::function<void(int)> onChanged;
    anyui::Select** bind = nullptr;
};
struct Combo
{
    std::vector<std::string> items;
    std::string text;
    std::function<void(const std::string&)> onChanged;
    std::function<void()> onActivate;
    anyui::Combo** bind = nullptr;
};
struct TextView
{
    std::string text;
    bool readOnly = false;
    bool monospace = false;
    bool wrap = true;
    bool compact = false;  // the same denser font as compact lists
    std::function<void()> onChanged;
    anyui::TextView** bind = nullptr;
};
struct List
{
    std::vector<Column> columns;
    ListModel* model = nullptr;
    SelectionMode selection = SelectionMode::Multi;
    bool headers = true;
    // Denser rows: a slightly smaller font and less cell padding, closer
    // to Windows list views than the toolkit default.
    bool compact = false;
    // Selected rows in a light tint of the accent colour with normal text
    // (as Windows list views do) instead of the toolkit's solid highlight.
    // Painted cells see the same colours through ThemeColor::Highlight and
    // ThemeColor::HighlightText.
    bool softSelection = false;
    std::function<void()> onSelectionChanged;
    std::function<void(size_t)> onActivate;
    std::function<void(size_t, SortOrder)> onSortRequested;
    std::function<Menu(std::optional<size_t>)> onContextMenu;
    std::function<Menu(size_t)> onHeaderMenu;
    anyui::List::ReorderFn onReorder;
    anyui::List** bind = nullptr;
};
struct MenuButton
{
    std::string text;
    Icon icon;
    bool pushed = false;
    std::function<Menu()> onMenu;
    anyui::MenuButton** bind = nullptr;
};
struct ProgressBar
{
    anyui::ProgressBar** bind = nullptr;
};
struct DatePicker
{
    Date date;
    std::function<void(Date)> onChanged;
    anyui::DatePicker** bind = nullptr;
};

// App-drawn surface; see anyui::Canvas.
struct Canvas
{
    bool focusable = true;  // takes keyboard focus on click (needed for onKey)
    std::function<void(Painter&, SizeF)> onPaint;
    std::function<void(SizeF)> onResize;
    std::function<bool(const PointerEvent&)> onPointer;
    std::function<bool(const ScrollEvent&)> onScroll;
    std::function<bool(const KeyEvent&)> onKey;
    anyui::Canvas** bind = nullptr;
};

// Without onClick, a dialog button ends the dialog with its role.
struct DialogButton
{
    DialogRole role = DialogRole::None;
    std::string text;  // empty = the toolkit's stock label for the role
    std::function<void()> onClick;
    anyui::Button** bind = nullptr;
};
// Row of dialog buttons; the backend orders them by platform convention.
struct Buttons
{
    std::vector<DialogButton> buttons;
};

}  // namespace spec

using Spec =
    std::variant<spec::Box, spec::Form, spec::Splitter, spec::Group, spec::Scroll, spec::Tabs,
                 spec::Spacer, spec::Separator, spec::Label, spec::Button, spec::Check, spec::Entry,
                 spec::Select, spec::Combo, spec::TextView, spec::List, spec::Canvas,
                 spec::MenuButton, spec::ProgressBar, spec::DatePicker, spec::Toolbar,
                 spec::ToolButton, spec::Slider, spec::Spin, spec::ColorButton, spec::Buttons>;

struct Node
{
    Spec spec;
    Common common;
};

struct WindowSpec
{
    std::string title;
    bool dialog = false;  // modal to its parent, has dialog buttons
    Node content;
    Extent width, height;
    // The usual space between the window edge and its content; off for
    // edge-to-edge content (an image viewer, a canvas-only window).
    bool margins = true;
};

// ---------------------------------------------------------------- builders

namespace layout
{

using anyui::Orientation;
inline constexpr Orientation Horizontal = Orientation::Horizontal;
inline constexpr Orientation Vertical = Orientation::Vertical;
using namespace anyui::literals;

template <class Self, class S>
class Builder
{
public:
    Self& stretch(int factor = 1)
    {
        node_.common.stretch = factor;
        return self();
    }
    Self& width(Extent w)
    {
        node_.common.width = w;
        return self();
    }
    Self& height(Extent h)
    {
        node_.common.height = h;
        return self();
    }
    Self& enabled(bool e)
    {
        node_.common.enabled = e;
        return self();
    }
    Self& tooltip(std::string t)
    {
        node_.common.tooltip = std::move(t);
        return self();
    }
    // The node as a plain Widget (setVisible, setEnabled...), whatever it is.
    Self& handle(Widget*& h)
    {
        node_.common.handle = &h;
        return self();
    }

    operator Node() const& { return node_; }
    operator Node() && { return std::move(node_); }

protected:
    Builder()
        : node_{S{}, {}}
    {
    }
    S& s() { return std::get<S>(node_.spec); }
    Self& self() { return static_cast<Self&>(*this); }

private:
    Node node_;
};

class VBox : public Builder<VBox, spec::Box>
{
public:
    VBox(std::initializer_list<Node> items) { s() = {Vertical, items}; }
};

class HBox : public Builder<HBox, spec::Box>
{
public:
    HBox(std::initializer_list<Node> items) { s() = {Horizontal, items}; }
};

struct FormRow
{
    std::string label;
    Node field;
};

class Form : public Builder<Form, spec::Form>
{
public:
    Form(std::initializer_list<FormRow> rows)
    {
        for (auto& r : rows)
        {
            s().labels.push_back(r.label);
            s().fields.push_back(r.field);
        }
    }
};

class Splitter : public Builder<Splitter, spec::Splitter>
{
public:
    template <class... Panes>
    Splitter(Orientation dir, Panes&&... panes)
    {
        s().dir = dir;
        (s().panes.push_back(Node(std::forward<Panes>(panes))), ...);
    }
    Splitter& bind(anyui::Splitter*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class Group : public Builder<Group, spec::Group>
{
public:
    Group(std::string title, Node content)
    {
        s().title = std::move(title);
        s().content.push_back(std::move(content));
    }
};

class Scroll : public Builder<Scroll, spec::Scroll>
{
public:
    explicit Scroll(Node content) { s().content.push_back(std::move(content)); }
    Scroll& horizontal(bool v = true)
    {
        s().horizontal = v;
        return *this;
    }
};

struct Page
{
    std::string title;
    Node content;
};

class Tabs : public Builder<Tabs, spec::Tabs>
{
public:
    Tabs(std::initializer_list<Page> pages)
    {
        for (auto& p : pages)
        {
            s().titles.push_back(p.title);
            s().pages.push_back(p.content);
        }
    }
    Tabs& onChanged(std::function<void(int)> f)
    {
        s().onChanged = std::move(f);
        return *this;
    }
    Tabs& bind(anyui::Tabs*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class Spacer : public Builder<Spacer, spec::Spacer>
{
public:
    Spacer() { stretch(); }
};

class Separator : public Builder<Separator, spec::Separator>
{
public:
    Separator() = default;
};

class Label : public Builder<Label, spec::Label>
{
public:
    explicit Label(std::string text = {}) { s().text = std::move(text); }
    Label& selectable(bool v = true)
    {
        s().selectable = v;
        return *this;
    }
    Label& ellipsize(bool v = true)
    {
        s().ellipsize = v;
        return *this;
    }
    Label& bind(anyui::Label*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class Button : public Builder<Button, spec::Button>
{
public:
    explicit Button(std::string text) { s().text = std::move(text); }
    Button& flat(bool v = true)
    {
        s().flat = v;
        return *this;
    }
    Button& icon(Icon i)
    {
        s().icon = std::move(i);
        return *this;
    }
    // Icon only; the text stays as tooltip and accessible name.
    Button& iconOnly(bool v = true)
    {
        s().iconOnly = v;
        return *this;
    }
    Button& onClick(std::function<void()> f)
    {
        s().onClick = std::move(f);
        return *this;
    }
    Button& bind(anyui::Button*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class Check : public Builder<Check, spec::Check>
{
public:
    explicit Check(std::string text, bool checked = false)
    {
        s().text = std::move(text);
        s().state = checked ? CheckState::Checked : CheckState::Unchecked;
    }
    Check(std::string text, CheckState state)
    {
        s().text = std::move(text);
        s().state = state;
    }
    Check& onToggled(std::function<void(bool)> f)
    {
        s().onToggled = std::move(f);
        return *this;
    }
    Check& bind(anyui::Check*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class Entry : public Builder<Entry, spec::Entry>
{
public:
    explicit Entry(std::string text = {}) { s().text = std::move(text); }
    Entry& placeholder(std::string p)
    {
        s().placeholder = std::move(p);
        return *this;
    }
    Entry& onChanged(std::function<void(const std::string&)> f)
    {
        s().onChanged = std::move(f);
        return *this;
    }
    Entry& onActivate(std::function<void()> f)
    {
        s().onActivate = std::move(f);
        return *this;
    }
    Entry& bind(anyui::Entry*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class Select : public Builder<Select, spec::Select>
{
public:
    explicit Select(std::vector<std::string> items = {}, int current = 0)
    {
        s().items = std::move(items);
        s().current = current;
    }
    Select& onChanged(std::function<void(int)> f)
    {
        s().onChanged = std::move(f);
        return *this;
    }
    Select& bind(anyui::Select*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class Combo : public Builder<Combo, spec::Combo>
{
public:
    explicit Combo(std::vector<std::string> items = {}, std::string text = {})
    {
        s().items = std::move(items);
        s().text = std::move(text);
    }
    Combo& onChanged(std::function<void(const std::string&)> f)
    {
        s().onChanged = std::move(f);
        return *this;
    }
    Combo& onActivate(std::function<void()> f)
    {
        s().onActivate = std::move(f);
        return *this;
    }
    Combo& bind(anyui::Combo*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class TextView : public Builder<TextView, spec::TextView>
{
public:
    explicit TextView(std::string text = {}) { s().text = std::move(text); }
    TextView& readOnly(bool v = true)
    {
        s().readOnly = v;
        return *this;
    }
    TextView& compact(bool v = true)
    {
        s().compact = v;
        return *this;
    }
    TextView& monospace(bool v = true)
    {
        s().monospace = v;
        return *this;
    }
    TextView& wrap(bool v)
    {
        s().wrap = v;
        return *this;
    }
    TextView& onChanged(std::function<void()> f)
    {
        s().onChanged = std::move(f);
        return *this;
    }
    TextView& bind(anyui::TextView*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class List : public Builder<List, spec::List>
{
public:
    List(std::vector<Column> columns, ListModel* model)
    {
        s().columns = std::move(columns);
        s().model = model;
    }
    List& selection(SelectionMode m)
    {
        s().selection = m;
        return *this;
    }
    List& softSelection(bool v = true)
    {
        s().softSelection = v;
        return *this;
    }
    List& compact(bool v = true)
    {
        s().compact = v;
        return *this;
    }
    List& headers(bool v)
    {
        s().headers = v;
        return *this;
    }
    List& onSelectionChanged(std::function<void()> f)
    {
        s().onSelectionChanged = std::move(f);
        return *this;
    }
    List& onActivate(std::function<void(size_t)> f)
    {
        s().onActivate = std::move(f);
        return *this;
    }
    List& onSortRequested(std::function<void(size_t, SortOrder)> f)
    {
        s().onSortRequested = std::move(f);
        return *this;
    }
    List& onContextMenu(std::function<Menu(std::optional<size_t>)> f)
    {
        s().onContextMenu = std::move(f);
        return *this;
    }
    List& onReorder(anyui::List::ReorderFn f)
    {
        s().onReorder = std::move(f);
        return *this;
    }
    List& onHeaderMenu(std::function<Menu(size_t)> f)
    {
        s().onHeaderMenu = std::move(f);
        return *this;
    }
    List& bind(anyui::List*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class MenuButton : public Builder<MenuButton, spec::MenuButton>
{
public:
    explicit MenuButton(std::string text) { s().text = std::move(text); }
    MenuButton& icon(Icon i)
    {
        s().icon = std::move(i);
        return *this;
    }
    MenuButton& pushed(bool v = true)
    {
        s().pushed = v;
        return *this;
    }
    MenuButton& onMenu(std::function<Menu()> f)
    {
        s().onMenu = std::move(f);
        return *this;
    }
    MenuButton& bind(anyui::MenuButton*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class ProgressBar : public Builder<ProgressBar, spec::ProgressBar>
{
public:
    ProgressBar() = default;
    ProgressBar& bind(anyui::ProgressBar*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class DatePicker : public Builder<DatePicker, spec::DatePicker>
{
public:
    explicit DatePicker(Date date = {}) { s().date = date; }
    DatePicker& onChanged(std::function<void(Date)> f)
    {
        s().onChanged = std::move(f);
        return *this;
    }
    DatePicker& bind(anyui::DatePicker*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class Canvas : public Builder<Canvas, spec::Canvas>
{
public:
    Canvas() = default;
    Canvas& focusable(bool v)
    {
        s().focusable = v;
        return *this;
    }
    Canvas& onPaint(std::function<void(Painter&, SizeF)> f)
    {
        s().onPaint = std::move(f);
        return *this;
    }
    Canvas& onResize(std::function<void(SizeF)> f)
    {
        s().onResize = std::move(f);
        return *this;
    }
    Canvas& onPointer(std::function<bool(const PointerEvent&)> f)
    {
        s().onPointer = std::move(f);
        return *this;
    }
    Canvas& onScroll(std::function<bool(const ScrollEvent&)> f)
    {
        s().onScroll = std::move(f);
        return *this;
    }
    Canvas& onKey(std::function<bool(const KeyEvent&)> f)
    {
        s().onKey = std::move(f);
        return *this;
    }
    Canvas& bind(anyui::Canvas*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class Toolbar : public Builder<Toolbar, spec::Toolbar>
{
public:
    Toolbar(std::initializer_list<Node> items) { s().items = items; }
};

class ToolButton : public Builder<ToolButton, spec::ToolButton>
{
public:
    ToolButton(Icon icon, std::string label)
    {
        s().icon = std::move(icon);
        s().label = std::move(label);
    }
    // A toggle: stays pressed while checked.
    ToolButton& toggle(bool checked = false)
    {
        s().toggle = true;
        s().checked = checked;
        return *this;
    }
    ToolButton& onClick(std::function<void()> f)
    {
        s().onClick = std::move(f);
        return *this;
    }
    ToolButton& onToggled(std::function<void(bool)> f)
    {
        s().onToggled = std::move(f);
        return *this;
    }
    ToolButton& bind(anyui::ToolButton*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class Slider : public Builder<Slider, spec::Slider>
{
public:
    Slider(double min, double max, double value)
    {
        s().min = min;
        s().max = max;
        s().value = value;
    }
    Slider& step(double step, double page)
    {
        s().step = step;
        s().page = page;
        return *this;
    }
    Slider& showValue(int digits)
    {
        s().showValue = true;
        s().digits = digits;
        return *this;
    }
    Slider& mark(double at, std::string label = {})
    {
        s().marks.emplace_back(at, std::move(label));
        return *this;
    }
    Slider& onChanged(std::function<void(double)> f)
    {
        s().onChanged = std::move(f);
        return *this;
    }
    Slider& bind(anyui::Slider*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class Spin : public Builder<Spin, spec::Spin>
{
public:
    Spin(double min, double max, double value, double step = 1, int digits = 0)
    {
        s().min = min;
        s().max = max;
        s().value = value;
        s().step = step;
        s().digits = digits;
    }
    Spin& onChanged(std::function<void(double)> f)
    {
        s().onChanged = std::move(f);
        return *this;
    }
    Spin& bind(anyui::Spin*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class ColorButton : public Builder<ColorButton, spec::ColorButton>
{
public:
    explicit ColorButton(Color c, std::string title = {})
    {
        s().color = c;
        s().title = std::move(title);
    }
    ColorButton& alpha(bool v = true)
    {
        s().alpha = v;
        return *this;
    }
    ColorButton& onChanged(std::function<void(Color)> f)
    {
        s().onChanged = std::move(f);
        return *this;
    }
    ColorButton& bind(anyui::ColorButton*& h)
    {
        s().bind = &h;
        return *this;
    }
};

class DialogButton
{
public:
    explicit DialogButton(DialogRole role, std::string text = {})
    {
        b_.role = role;
        b_.text = std::move(text);
    }
    DialogButton& onClick(std::function<void()> f)
    {
        b_.onClick = std::move(f);
        return *this;
    }
    DialogButton& bind(anyui::Button*& h)
    {
        b_.bind = &h;
        return *this;
    }
    operator spec::DialogButton() const { return b_; }

private:
    spec::DialogButton b_;
};

// `Ok` on its own, or `Ok("Start rebase")` for a custom label.
struct RoleTag
{
    DialogRole role;
    DialogButton operator()(std::string text = {}) const
    {
        return DialogButton{role, std::move(text)};
    }
    operator spec::DialogButton() const { return DialogButton{role}; }
};
inline constexpr RoleTag Ok{DialogRole::Ok}, Cancel{DialogRole::Cancel}, Apply{DialogRole::Apply},
    Close{DialogRole::Close}, Yes{DialogRole::Yes}, No{DialogRole::No}, Help{DialogRole::Help},
    Reset{DialogRole::Reset};

class Buttons : public Builder<Buttons, spec::Buttons>
{
public:
    Buttons(std::initializer_list<spec::DialogButton> buttons) { s().buttons = buttons; }
};

namespace detail
{
inline Node single(std::initializer_list<Node> content)
{
    if (content.size() == 1) return *content.begin();
    return VBox(content);
}
}  // namespace detail

class Window
{
public:
    Window(std::string title, std::initializer_list<Node> content)
        : w_{std::move(title), false, detail::single(content), {}, {}}
    {
    }
    Window& size(Extent w, Extent h)
    {
        w_.width = w;
        w_.height = h;
        return *this;
    }
    Window& margins(bool on)
    {
        w_.margins = on;
        return *this;
    }
    operator WindowSpec() const& { return w_; }
    operator WindowSpec() && { return std::move(w_); }

protected:
    WindowSpec w_;
};

class Dialog : public Window
{
public:
    Dialog(std::string title, std::initializer_list<Node> content)
        : Window(std::move(title), content)
    {
        w_.dialog = true;
    }
    Dialog& size(Extent w, Extent h)
    {
        Window::size(w, h);
        return *this;
    }
};

}  // namespace layout
}  // namespace anyui
