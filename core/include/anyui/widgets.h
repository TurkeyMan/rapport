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
#include <utility>
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
    float size = 0;  // points; 0 = the toolkit's normal size
};

enum class LineCap
{
    Butt,   // ends exactly at the end points
    Round,  // a half-disc past each end
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

// ---------------------------------------------------------------- subscriptions

// Keeps a callback registered (a timer, a per-frame callback) for as long as
// it lives; destroying or reset()ting it cancels. Move-only.
class Subscription
{
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> cancel)
        : cancel_(std::move(cancel))
    {
    }
    Subscription(Subscription&& o) noexcept
        : cancel_(std::exchange(o.cancel_, nullptr))
    {
    }
    Subscription& operator=(Subscription&& o) noexcept
    {
        if (this != &o)
        {
            reset();
            cancel_ = std::exchange(o.cancel_, nullptr);
        }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    ~Subscription() { reset(); }

    void reset()
    {
        if (auto c = std::exchange(cancel_, nullptr)) c();
    }
    explicit operator bool() const { return bool(cancel_); }

private:
    std::function<void()> cancel_;
};

// ---------------------------------------------------------------- images

// Pixel layouts an Image can be created from. Bgra8Premultiplied (bytes
// B,G,R,A; colour premultiplied by alpha) is what Cairo, Direct2D, GDK and
// Qt (ARGB32_Premultiplied) draw natively, so it uploads without conversion.
enum class PixelFormat
{
    Bgra8Premultiplied,
    Rgba8,  // straight alpha; converted on upload
};

enum class Filter
{
    Nearest,  // hard pixel edges (zoomed-in inspection)
    Linear,   // smooth
};

// Pixels uploaded to the backend once and drawn many times with
// Painter::image (a cairo surface, GdkTexture, QImage, D2D bitmap).
// Created by Ui::createImage; usable from the UI thread only.
class Image
{
public:
    virtual ~Image() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    // Replace all pixels (same size and format as created).
    virtual void update(const uint8_t* pixels, size_t stride) = 0;
};

// ---------------------------------------------------------------- painting

// Tiny drawing API for custom-painted list cells (commit graph, ref pills)
// and Canvas widgets. Origin is the cell's / canvas's top-left corner, in
// logical pixels. Maps to QPainter / Cairo / GtkSnapshot / Direct2D.
class Painter
{
public:
    virtual ~Painter() = default;

    virtual Color resolve(const Paint&) const = 0;

    virtual void line(PointF a, PointF b, const Paint&, float width = 1,
                      LineCap cap = LineCap::Butt) = 0;
    virtual void curve(PointF a, PointF c1, PointF c2, PointF b, const Paint&,
                       float width = 1) = 0;  // cubic bezier
    virtual void circle(PointF centre, float radius, std::optional<Paint> fill,
                        std::optional<Paint> stroke) = 0;
    virtual void rect(RectF, float cornerRadius, std::optional<Paint> fill,
                      std::optional<Paint> stroke) = 0;
    // Closed polygon (a play triangle, an arrow head).
    virtual void polygon(const std::vector<PointF>& points, std::optional<Paint> fill,
                         std::optional<Paint> stroke, float width = 1) = 0;
    // Circular arc, angles in radians clockwise from 3 o'clock.
    virtual void arc(PointF centre, float radius, float start, float end, const Paint&,
                     float width = 1, LineCap cap = LineCap::Butt) = 0;
    // Draws `src` (in image pixels) into `dst` (logical pixels), scaled with
    // `filter`. For 1:1 device pixels, make dst = src size / deviceScale().
    virtual void image(const Image&, RectF src, RectF dst, Filter = Filter::Linear,
                       float alpha = 1) = 0;
    // Device pixels per logical pixel (2 on a 200% display).
    virtual float deviceScale() const = 0;
    // Restrict drawing to `r` until the matching pop (nestable).
    virtual void pushClip(RectF r) = 0;
    virtual void popClip() = 0;
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

// ---------------------------------------------------------------- icons

// An icon for a button or toolbar item: a named theme icon, an app-drawn
// one, or both (the drawing is the fallback where the name isn't found,
// and is always used where there is no icon theme, e.g. Win32).
struct Icon
{
    // freedesktop icon name, e.g. "go-up", "document-save".
    std::string name;
    // Draws the icon into `size` (logical px) on a Painter with the right
    // deviceScale. Draw with theme colours (ThemeColor::WindowText) so it
    // follows dark mode; backends redraw it when the theme or scale changes.
    std::function<void(Painter&, SizeF)> draw;

    static Icon named(std::string n) { return {std::move(n), {}}; }
    static Icon drawn(std::function<void(Painter&, SizeF)> f) { return {{}, std::move(f)}; }
    explicit operator bool() const { return !name.empty() || bool(draw); }
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
    bool visible = true;  // hidden columns can be shown from a header menu
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
    virtual void setIcon(const Icon&) = 0;
    virtual void onClick(std::function<void()>) = 0;
};

// Toolbar item: icon (and/or label) button, optionally a toggle.
class ToolButton : public Widget
{
public:
    virtual void setIcon(const Icon&) = 0;
    virtual bool checked() const = 0;   // toggles only
    virtual void setChecked(bool) = 0;  // doesn't fire onToggled
    virtual void onClick(std::function<void()>) = 0;
    virtual void onToggled(std::function<void(bool)>) = 0;
};

// A value on a continuous range: horizontal track with a knob.
class Slider : public Widget
{
public:
    virtual double value() const = 0;
    virtual void setValue(double) = 0;  // doesn't fire onChanged
    virtual void setRange(double min, double max) = 0;
    virtual void onChanged(std::function<void(double)>) = 0;
};

// A number with up/down steppers.
class Spin : public Widget
{
public:
    virtual double value() const = 0;
    virtual void setValue(double) = 0;  // doesn't fire onChanged
    virtual void setRange(double min, double max) = 0;
    virtual void onChanged(std::function<void(double)>) = 0;
};

// Shows a colour; clicking opens the platform's colour chooser.
class ColorButton : public Widget
{
public:
    virtual Color color() const = 0;
    virtual void setColor(Color) = 0;  // doesn't fire onChanged
    virtual void onChanged(std::function<void(Color)>) = 0;
};

enum class CheckState
{
    Unchecked,
    Checked,
    Indeterminate,  // the "mixed" mark; set by the app, clicks only toggle on/off
};

class Check : public Widget
{
public:
    virtual bool checked() const = 0;  // false while Indeterminate
    virtual void setChecked(bool) = 0;
    virtual CheckState state() const = 0;
    virtual void setState(CheckState) = 0;
    virtual void onToggled(std::function<void(bool)>) = 0;  // user clicks only
};

// Button that drops down a menu below itself (TortoiseGit's "Walk
// Behavior" and "View"), with a drop-down arrow.
class MenuButton : public Widget
{
public:
    virtual void setText(const std::string&) = 0;
    virtual void setIcon(const Icon&) = 0;  // before the text (a magnifier, an arrow)
    // Drawn pressed while true, e.g. while any of its options is active.
    virtual void setPushed(bool) = 0;
    // Called each time the menu opens, so check marks are current.
    virtual void onMenu(std::function<Menu()>) = 0;
};

class ProgressBar : public Widget
{
public:
    virtual void setFraction(float) = 0;  // 0..1
    virtual void pulse() = 0;             // activity of unknown length
};

struct Date
{
    int year = 1970, month = 1, day = 1;  // month and day are 1-based
    auto operator<=>(const Date&) const = default;
};

class DatePicker : public Widget
{
public:
    virtual Date date() const = 0;
    virtual void setDate(Date) = 0;
    virtual void onChanged(std::function<void(Date)>) = 0;  // user changes only
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
    virtual bool columnVisible(size_t column) const = 0;
    virtual void setColumnVisible(size_t column, bool) = 0;

    virtual void onSelectionChanged(std::function<void()>) = 0;
    virtual void onActivate(std::function<void(size_t row)>) = 0;  // double-click / Enter
    virtual void onSortRequested(std::function<void(size_t column, SortOrder)>) = 0;
    // Right-click / menu key; return an empty menu for none.
    virtual void onContextMenu(std::function<Menu(std::optional<size_t> row)>) = 0;
    // Right-click on a column header (e.g. a menu of columns to show).
    virtual void onHeaderMenu(std::function<Menu(size_t column)>) = 0;
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

// ---------------------------------------------------------------- canvas

enum Modifier : unsigned
{
    ModShift = 1,
    ModCtrl = 2,
    ModAlt = 4,
    ModMeta = 8,  // Super / Windows / Command
};

struct PointerEvent
{
    enum class Type
    {
        Press,
        Release,
        Move,
        Enter,
        Leave,
    };
    Type type = Type::Move;
    PointF pos;      // logical pixels, canvas-relative
    int button = 0;  // Press/Release: 1 left, 2 middle, 3 right, 4/5 back/forward
    int clicks = 1;  // Press: 2 for the second press of a double click
    unsigned modifiers = 0;
};

struct ScrollEvent
{
    PointF pos;
    // Wheel notches (+1 = one notch down/right), or pixel-precise deltas
    // from a touchpad when `precise` is set.
    float dx = 0, dy = 0;
    bool precise = false;
    unsigned modifiers = 0;
};

// Keys that have no text. Others arrive as Key::Character with `codepoint`.
enum class Key
{
    Character,
    Escape,
    Enter,
    Tab,
    Backspace,
    Delete,
    Insert,
    Space,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Other,
};

struct KeyEvent
{
    bool press = true;  // false: release
    Key key = Key::Other;
    char32_t codepoint = 0;  // Key::Character: lowercase/unshifted where it has one
    unsigned modifiers = 0;
    bool repeat = false;
};

enum class Cursor
{
    Default,
    Pointer,  // a link / clickable thing
    Text,
    Crosshair,
    Grab,
    Grabbing,
    ZoomIn,
    ZoomOut,
    Move,
    Wait,
    Hidden,
};

// A surface the app draws itself (an image view, a chart). Everything else
// in a window should stay a native widget.
class Canvas : public Widget
{
public:
    virtual void invalidate() = 0;  // repaint soon
    virtual SizeF size() const = 0;
    virtual float deviceScale() const = 0;
    virtual void setCursor(Cursor) = 0;

    // Paint handler: draws the whole canvas.
    virtual void onPaint(std::function<void(Painter&, SizeF)>) = 0;
    // Size changed, or the device scale did (moved to another monitor).
    virtual void onResize(std::function<void(SizeF)>) = 0;
    // Input handlers return true when they used the event.
    virtual void onPointer(std::function<bool(const PointerEvent&)>) = 0;
    virtual void onScroll(std::function<bool(const ScrollEvent&)>) = 0;
    virtual void onKey(std::function<bool(const KeyEvent&)>) = 0;

    // Called once per displayed frame (paced by the display) with the frame
    // time in seconds, until the subscription is dropped. For animation.
    [[nodiscard]] virtual Subscription onFrame(std::function<void(double)>) = 0;
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

    // Size of the window's content in logical px, and resizing it.
    virtual SizeF size() const = 0;
    virtual void resize(SizeF) = 0;
    virtual void setMinSize(SizeF) = 0;
    // Position in logical px of the frame's top-left. Where the platform
    // doesn't let apps place windows (Wayland), move() does nothing and
    // position() is unknown (nullopt).
    virtual std::optional<PointF> position() const = 0;
    virtual void move(PointF) = 0;
    // Usable area of the monitor the window is on (minus panels/taskbar).
    virtual RectF workArea() const = 0;

    virtual bool fullscreen() const = 0;
    virtual void setFullscreen(bool) = 0;
    virtual void onFullscreenChanged(std::function<void(bool)>) = 0;
    virtual void setIcon(const Icon&) = 0;

    // Keys reach this before the focused widget (except text entry while it
    // has focus); return true to consume. For app-wide keys (navigation,
    // fullscreen) that must work whatever has focus.
    virtual void onKey(std::function<bool(const KeyEvent&)>) = 0;
    // Dialogs: fired once with the role of the button that ended it
    // (Cancel for Esc / close box). There is no blocking exec(): GTK4
    // has no nested main loops, so everything is callback-driven.
    virtual void onFinished(std::function<void(DialogRole)>) = 0;
    // Return false to veto closing (e.g. unsaved commit message).
    virtual void onCloseRequest(std::function<bool()>) = 0;
};

}  // namespace anyui
