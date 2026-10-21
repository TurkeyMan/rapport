// Toolkit-agnostic widget interfaces.
//
// Backends (anyui_qt.so, anyui_gtk.so, ...) implement these with native
// widgets. Apps only ever see these abstract types. All calls happen on the
// UI thread; worker threads hand results back with Backend::post().
//
// Strings are UTF-8. Coordinates and sizes are logical pixels; backends
// handle device scale.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace anyui
{

// ---------------------------------------------------------------- basics

enum class Orientation
{
    Horizontal,
    Vertical
};
enum class Align
{
    Start,
    Center,
    End
};

struct PointF
{
    float x = 0, y = 0;
};
struct SizeF
{
    float w = 0, h = 0;
};
struct RectF
{
    float x = 0, y = 0, w = 0, h = 0;
};

struct Color
{
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

// Colours taken from the desktop theme (portal accent, dark mode, ...).
enum class ThemeColor
{
    Window,
    WindowText,
    Base,
    AlternateBase,
    Text,
    PlaceholderText,
    Highlight,
    HighlightText,
    Mid,
    Accent,
    Link,
    // diff add / remove / context, status pills
    Positive,
    Negative,
    Neutral,
};

// Either a fixed colour or a theme role resolved at paint time.
using Paint = std::variant<ThemeColor, Color>;

struct TextStyle
{
    std::optional<Paint> fg, bg;
    bool bold = false;
    bool italic = false;
    bool monospace = false;
};

// Semantic size: converted by the backend with real font metrics.
struct Extent
{
    enum class Unit
    {
        Auto,
        Chars,
        Lines,
        Px
    };
    float value = 0;
    Unit unit = Unit::Auto;
};

inline namespace literals
{
constexpr Extent operator""_ch(unsigned long long v)
{
    return {float(v), Extent::Unit::Chars};
}
constexpr Extent operator""_lines(unsigned long long v)
{
    return {float(v), Extent::Unit::Lines};
}
constexpr Extent operator""_px(unsigned long long v)
{
    return {float(v), Extent::Unit::Px};
}
}  // namespace literals

// ---------------------------------------------------------------- menus

struct MenuItem
{
    std::string label;  // empty label + no children = separator
    std::function<void()> action;
    std::vector<MenuItem> children;  // non-empty = submenu
    std::string shortcut;            // display hint, e.g. "Ctrl+C"
    bool enabled = true;
    std::optional<bool> checked;  // set = checkable item

    static MenuItem separator() { return {}; }
    bool isSeparator() const { return label.empty() && children.empty(); }
};
using Menu = std::vector<MenuItem>;

// ---------------------------------------------------------------- painting

// Tiny vector API for custom-painted list cells (commit graph, ref pills).
// Origin is the cell's top-left corner. Maps to QPainter / Cairo / GtkSnapshot.
class Painter
{
public:
    virtual ~Painter() = default;

    virtual Color resolve(const Paint&) const = 0;

    virtual void line(PointF a, PointF b, const Paint&, float width = 1) = 0;
    virtual void curve(PointF a, PointF c1, PointF c2, PointF b, const Paint&,
                       float width = 1) = 0;  // cubic bezier
    virtual void circle(PointF centre, float radius, std::optional<Paint> fill,
                        std::optional<Paint> stroke) = 0;
    virtual void rect(RectF, float cornerRadius, std::optional<Paint> fill,
                      std::optional<Paint> stroke) = 0;
    // Draws text with its top-left at `at`, clipped to the cell.
    virtual void text(PointF at, const std::string&, const TextStyle&) = 0;
    virtual SizeF measure(const std::string&, const TextStyle&) const = 0;
};

struct CellInfo
{
    SizeF size;
    bool selected = false;
    bool focused = false;
};

// ---------------------------------------------------------------- list model

class ListObserver
{
public:
    virtual ~ListObserver() = default;
    virtual void modelReset() = 0;
    virtual void rowsChanged(size_t first, size_t count) = 0;
    virtual void rowsInserted(size_t first, size_t count) = 0;
    virtual void rowsRemoved(size_t first, size_t count) = 0;
};

// Virtual list model: the view pulls only what is visible.
// Rows have uniform height so backends can use fixed-height fast paths.
class ListModel
{
public:
    virtual ~ListModel() = default;

    virtual size_t rowCount() const = 0;
    virtual std::string text(size_t row, size_t column) const = 0;
    virtual TextStyle style(size_t /*row*/, size_t /*column*/) const { return {}; }
    // Called for columns declared `painted`; text() is still used for
    // accessibility, type-ahead and copy.
    virtual void paint(size_t /*row*/, size_t /*column*/, Painter&, const CellInfo&) const {}

    void addObserver(ListObserver* o) { observers_.push_back(o); }
    void removeObserver(ListObserver* o) { std::erase(observers_, o); }

protected:
    void notifyReset()
    {
        for (auto* o : observers_) o->modelReset();
    }
    void notifyChanged(size_t first, size_t n)
    {
        for (auto* o : observers_) o->rowsChanged(first, n);
    }
    void notifyInserted(size_t first, size_t n)
    {
        for (auto* o : observers_) o->rowsInserted(first, n);
    }
    void notifyRemoved(size_t first, size_t n)
    {
        for (auto* o : observers_) o->rowsRemoved(first, n);
    }

private:
    std::vector<ListObserver*> observers_;
};

struct Column
{
    std::string title;
    Extent width;          // initial width; Auto = fit header
    bool stretch = false;  // takes spare width
    bool painted = false;  // drawn through ListModel::paint
    bool sortable = false;
    Align align = Align::Start;
};

enum class SelectionMode
{
    None,
    Single,
    Multi
};
enum class SortOrder
{
    Ascending,
    Descending
};

// ---------------------------------------------------------------- widgets

class Widget
{
public:
    virtual ~Widget() = default;
    virtual void setEnabled(bool) = 0;
    virtual void setVisible(bool) = 0;
    virtual void setTooltip(const std::string&) = 0;
    virtual void focus() = 0;
};

class Label : public Widget
{
public:
    virtual void setText(const std::string&) = 0;
};

class Button : public Widget
{
public:
    virtual void setText(const std::string&) = 0;
    virtual void onClick(std::function<void()>) = 0;
};

class Check : public Widget
{
public:
    virtual bool checked() const = 0;
    virtual void setChecked(bool) = 0;
    virtual void onToggled(std::function<void(bool)>) = 0;
};

// Single-line text entry.
class Entry : public Widget
{
public:
    virtual std::string text() const = 0;
    virtual void setText(const std::string&) = 0;
    virtual void setPlaceholder(const std::string&) = 0;
    virtual void onChanged(std::function<void(const std::string&)>) = 0;
    virtual void onActivate(std::function<void()>) = 0;  // Enter pressed
};

// Drop-down with a fixed set of choices (QComboBox / GtkDropDown).
class Select : public Widget
{
public:
    virtual void setItems(std::vector<std::string>) = 0;
    virtual int current() const = 0;  // -1 = none
    virtual void setCurrent(int) = 0;
    virtual void onChanged(std::function<void(int)>) = 0;
};

// Editable drop-down: free text plus suggestions.
class Combo : public Widget
{
public:
    virtual void setItems(std::vector<std::string>) = 0;
    virtual std::string text() const = 0;
    virtual void setText(const std::string&) = 0;
    virtual void onChanged(std::function<void(const std::string&)>) = 0;
    virtual void onActivate(std::function<void()>) = 0;
};

// Multi-line text: read-only with styled spans (diffs, details) or editable.
class TextView : public Widget
{
public:
    struct Span
    {
        size_t begin, end;  // byte offsets into the text
        TextStyle style;
    };
    virtual std::string text() const = 0;
    virtual void setText(const std::string&, std::vector<Span> spans = {}) = 0;
    virtual void append(const std::string&, const TextStyle& = {}) = 0;
    virtual void setReadOnly(bool) = 0;
    virtual void onChanged(std::function<void()>) = 0;
};

class List : public Widget
{
public:
    virtual void setModel(ListModel*) = 0;              // not owned; must outlive the list
    virtual std::vector<size_t> selection() const = 0;  // ascending
    virtual std::optional<size_t> current() const = 0;
    virtual void select(const std::vector<size_t>& rows) = 0;
    virtual void setCurrent(size_t row) = 0;
    virtual void scrollTo(size_t row) = 0;

    // The model is virtual, so sorting is the app's job: the list reports
    // header clicks and shows the indicator it is told to.
    virtual void setSortIndicator(std::optional<size_t> column, SortOrder) = 0;

    virtual std::vector<float> columnWidths() const = 0;  // for saving state
    virtual void setColumnWidths(const std::vector<float>&) = 0;

    virtual void onSelectionChanged(std::function<void()>) = 0;
    virtual void onActivate(std::function<void(size_t row)>) = 0;  // double-click / Enter
    virtual void onSortRequested(std::function<void(size_t column, SortOrder)>) = 0;
    // Right-click / menu key; return an empty menu for none.
    virtual void onContextMenu(std::function<Menu(std::optional<size_t> row)>) = 0;
};

// Draggable divider between panes.
class Splitter : public Widget
{
public:
    virtual std::vector<float> sizes() const = 0;  // for saving state
    virtual void setSizes(const std::vector<float>&) = 0;
};

class Tabs : public Widget
{
public:
    virtual int current() const = 0;
    virtual void setCurrent(int) = 0;
    virtual void onChanged(std::function<void(int)>) = 0;
};

// ---------------------------------------------------------------- windows

enum class DialogRole
{
    None,
    Ok,
    Cancel,
    Apply,
    Close,
    Yes,
    No,
    Help,
    Reset
};

class Window
{
public:
    virtual ~Window() = default;  // destroys all widgets in the window
    virtual void setTitle(const std::string&) = 0;
    virtual void show() = 0;
    virtual void close() = 0;
    // Dialogs: fired once with the role of the button that ended it
    // (Cancel for Esc / close box). There is no blocking exec(): GTK4
    // has no nested main loops, so everything is callback-driven.
    virtual void onFinished(std::function<void(DialogRole)>) = 0;
    // Return false to veto closing (e.g. unsaved commit message).
    virtual void onCloseRequest(std::function<bool()>) = 0;
};

}  // namespace anyui
