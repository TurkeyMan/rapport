// Toolkit-agnostic widget interfaces.
//
// Backends (rapport_qt.so, rapport_gtk.so, ...) implement these with native
// widgets. Apps only ever see these abstract types. All calls happen on the
// UI thread; worker threads hand results back with backend::post().
//
// Strings are UTF-8. Coordinates and sizes are logical pixels; backends
// handle device scale.
#pragma once

#include <algorithm>
#include <cmath>
#include "gpu.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rap
{

// -- basics -------------------------------------------------------------------

enum class orientation
{
    horizontal,
    vertical
};
enum class align
{
    start,
    center,
    end
};

struct point_f
{
    float x = 0, y = 0;
};
struct size_f
{
    float w = 0, h = 0;
};
struct rect_f
{
    float x = 0, y = 0, w = 0, h = 0;
};

struct color
{
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

// Colours taken from the desktop theme (portal accent, dark mode, ...).
enum class theme_color
{
    window,
    window_text,
    base,
    alternate_base,
    text,
    placeholder_text,
    highlight,
    highlight_text,
    mid,
    accent,
    link,
    // diff add / remove / context, status pills
    positive,
    negative,
    neutral,
};

// Either a fixed colour or a theme role resolved at paint time.
using paint = std::variant<theme_color, color>;

// The theme roles on paper: a light palette whatever the screen's theme, so
// a dark-mode window doesn't print white text on white. Every backend's
// painter resolves theme colours with this while printing.
inline color print_color(theme_color c)
{
    switch (c)
    {
    case theme_color::window:
    case theme_color::base: return {255, 255, 255};
    case theme_color::alternate_base: return {245, 245, 245};
    case theme_color::window_text:
    case theme_color::text:
    case theme_color::highlight_text: return {0, 0, 0};
    case theme_color::placeholder_text: return {128, 128, 128};
    case theme_color::highlight: return {215, 215, 215};
    case theme_color::mid: return {160, 160, 160};
    case theme_color::accent: return {0, 90, 180};
    case theme_color::link: return {0, 0, 200};
    case theme_color::positive: return {26, 127, 55};
    case theme_color::negative: return {207, 34, 46};
    case theme_color::neutral: return {101, 109, 118};
    }
    return {0, 0, 0};
}

struct text_style
{
    std::optional<paint> fg, bg;
    bool bold = false;
    bool italic = false;
    bool monospace = false;
    float size = 0;  // points; 0 = the toolkit's normal size
};

enum class line_cap
{
    butt,   // ends exactly at the end points
    round,  // a half-disc past each end
};

// Semantic size: converted by the backend with real font metrics.
struct extent
{
    enum class unit
    {
        auto_,
        chars,
        lines,
        px
    };
    float value = 0;
    unit unit = unit::auto_;
};

inline namespace literals
{
constexpr extent operator""_ch(unsigned long long v)
{
    return {float(v), extent::unit::chars};
}
constexpr extent operator""_lines(unsigned long long v)
{
    return {float(v), extent::unit::lines};
}
constexpr extent operator""_px(unsigned long long v)
{
    return {float(v), extent::unit::px};
}
}  // namespace literals

// -- menus --------------------------------------------------------------------

struct menu_item
{
    // "_" marks the access key ("_Copy"); write "__" for a literal underscore
    // (escape user text such as branch names). Empty label and no children
    // = separator.
    std::string label;
    std::function<void()> action;
    std::vector<menu_item> children;  // non-empty = submenu
    std::string shortcut;            // display hint, e.g. "Ctrl+C"
    bool enabled = true;
    std::optional<bool> checked;  // set = checkable item
    bool radio = false;           // a checkable item drawn as one of a group

    static menu_item separator() { return {}; }
    bool is_separator() const { return label.empty() && children.empty(); }
};
using menu = std::vector<menu_item>;

// One menu of a window's menu bar: "_File", and its items, built each time
// it opens so they can show the app's current state.
struct menu_bar_item
{
    std::string title;  // '_' marks the access key, as in menu_item::label
    std::function<rap::menu()> menu;
};

// -- subscriptions ------------------------------------------------------------

// Keeps a callback registered (a timer, a per-frame callback) for as long as
// it lives; destroying or reset()ting it cancels. Move-only.
class subscription
{
public:
    subscription() = default;
    explicit subscription(std::function<void()> cancel)
        : cancel_(std::move(cancel))
    {
    }
    subscription(subscription&& o) noexcept
        : cancel_(std::exchange(o.cancel_, nullptr))
    {
    }
    subscription& operator=(subscription&& o) noexcept
    {
        if (this != &o)
        {
            reset();
            cancel_ = std::exchange(o.cancel_, nullptr);
        }
        return *this;
    }
    subscription(const subscription&) = delete;
    subscription& operator=(const subscription&) = delete;
    ~subscription() { reset(); }

    void reset()
    {
        if (auto c = std::exchange(cancel_, nullptr)) c();
    }
    explicit operator bool() const { return bool(cancel_); }

private:
    std::function<void()> cancel_;
};

// -- images -------------------------------------------------------------------

// Pixel layouts an Image can be created from. Bgra8Premultiplied (bytes
// B,G,R,A; colour premultiplied by alpha) is what Cairo, Direct2D, GDK and
// Qt (ARGB32_Premultiplied) draw natively, so it uploads without conversion.
enum class pixel_format
{
    bgra8_premultiplied,
    rgba8,  // straight alpha; converted on upload
};

enum class filter
{
    nearest,  // hard pixel edges (zoomed-in inspection)
    linear,   // smooth
};

// Pixels uploaded to the backend once and drawn many times with
// painter::image (a cairo surface, GdkTexture, QImage, D2D bitmap).
// Created by ui::create_image; usable from the UI thread only.
class image
{
public:
    virtual ~image() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    // Replace all pixels (same size and format as created).
    virtual void update(const uint8_t* pixels, size_t stride) = 0;
};

// -- painting -----------------------------------------------------------------

// Tiny drawing API for custom-painted list cells (commit graph, ref pills)
// and Canvas widgets. Origin is the cell's / canvas's top-left corner, in
// logical pixels. Maps to QPainter / Cairo / GtkSnapshot / Direct2D.
class painter
{
public:
    virtual ~painter() = default;

    virtual color resolve(const paint&) const = 0;

    virtual void line(point_f a, point_f b, const paint&, float width = 1,
                      line_cap cap = line_cap::butt) = 0;
    virtual void curve(point_f a, point_f c1, point_f c2, point_f b, const paint&,
                       float width = 1) = 0;  // cubic bezier
    virtual void circle(point_f centre, float radius, std::optional<paint> fill,
                        std::optional<paint> stroke) = 0;
    virtual void rect(rect_f, float corner_radius, std::optional<paint> fill,
                      std::optional<paint> stroke) = 0;
    // Closed polygon (a play triangle, an arrow head).
    virtual void polygon(const std::vector<point_f>& points, std::optional<paint> fill,
                         std::optional<paint> stroke, float width = 1) = 0;
    // Circular arc, angles in radians clockwise from 3 o'clock.
    virtual void arc(point_f centre, float radius, float start, float end, const paint&,
                     float width = 1, line_cap cap = line_cap::butt) = 0;
    // Draws `src` (in image pixels) into `dst` (logical pixels), scaled with
    // `filter`. For 1:1 device pixels, make dst = src size / device_scale().
    virtual void image(const rap::image&, rect_f src, rect_f dst, filter = filter::linear,
                       float alpha = 1) = 0;
    // Device pixels per logical pixel as the display shows them (2 on a 200%
    // display, 1.5 at 150%): for sizes on screen and 1:1 images.
    virtual float device_scale() const = 0;
    // Pixels per logical pixel of the surface being drawn into, for snapping
    // to its grid (pixel_grid). The same as device_scale() except where a
    // toolkit draws at a whole scale the compositor then resizes: GTK 3 on
    // Wayland at 150% draws at 2, shown at 1.5, so its 1:1 images are the
    // right size but slightly soft (exact at whole scales; GTK 4 and Qt are
    // exact). See docs/gtk3-fractional-scale.md.
    virtual float pixel_scale() const { return device_scale(); }
    // Restrict drawing to `r` until the matching pop (nestable).
    virtual void push_clip(rect_f r) = 0;
    virtual void pop_clip() = 0;
    // Draws text with its top-left at `at`, clipped to the cell.
    virtual void text(point_f at, const std::string&, const text_style&) = 0;
    virtual size_f measure(const std::string&, const text_style&) const = 0;
};

// Device-pixel snapping for crisp painting at any scale: boxes and bars on
// whole device pixels, thin strokes centred so their edges fall on pixels.
// Everything stays in logical pixels; only the rounding uses the scale.
//     pixel_grid g(painter);
//     p.rect({g.snap(x), g.snap(y), g.size(w), g.size(h)}, ...);
//     float y = g.stroke(lineY, 1); p.line({x0, y}, {x1, y}, c, g.stroke_width(1));
struct pixel_grid
{
    float k;  // surface pixels per logical pixel (painter::pixel_scale)

    explicit pixel_grid(const painter& p)
        : k(std::max(1.f, p.pixel_scale()))
    {
    }

    float px() const { return 1 / k; }
    float snap(float v) const { return std::round(v * k) / k; }
    // A length of at least one device pixel.
    float size(float v) const { return std::max(1.f, std::round(v * k)) / k; }
    // Where to centre a stroke of `width` near `v` so its edges fall on pixels.
    float stroke(float v, float width) const
    {
        int w = std::max(1, int(std::round(width * k)));
        return (w % 2 ? std::floor(v * k) + 0.5f : std::round(v * k)) / k;
    }
    float stroke_width(float width) const { return std::max(1.f, std::round(width * k)) / k; }
};

struct cell_info
{
    size_f size;
    bool selected = false;
    bool focused = false;
};

// -- icons --------------------------------------------------------------------

// An icon for a button or toolbar item: a named theme icon, an app-drawn
// one, or both (the drawing is the fallback where the name isn't found,
// and is always used where there is no icon theme, e.g. Win32).
struct icon
{
    // freedesktop icon name, e.g. "go-up", "document-save".
    std::string name;
    // The desktop's icon for this kind of file, as its file manager shows
    // it (for_file). Only the name matters (its extension or MIME type); the
    // file needn't exist. A name ending in '/' is a folder.
    std::string file;
    // Draws the icon into `size` (logical px) on a Painter with the right
    // device_scale. Draw with theme colours (theme_color::window_text) so it
    // follows dark mode; backends redraw it when the theme or scale changes.
    std::function<void(rap::painter&, rap::size_f)> draw;

    static icon named(std::string n) { return {std::move(n), {}, {}}; }
    static icon for_file(std::string file_name) { return {{}, std::move(file_name), {}}; }
    static icon drawn(std::function<void(rap::painter&, rap::size_f)> f) { return {{}, {}, std::move(f)}; }
    explicit operator bool() const { return !name.empty() || !file.empty() || bool(draw); }
};

// -- list model ---------------------------------------------------------------

class list_observer
{
public:
    virtual ~list_observer() = default;
    virtual void model_reset() = 0;
    virtual void rows_changed(size_t first, size_t count) = 0;
    virtual void rows_inserted(size_t first, size_t count) = 0;
    virtual void rows_removed(size_t first, size_t count) = 0;
};

// Virtual list model: the view pulls only what is visible.
// Rows have uniform height so backends can use fixed-height fast paths.
enum class check_state
{
    unchecked,
    checked,
    indeterminate,  // the "mixed" mark; set by the app, clicks only toggle on/off
};

class list_model
{
public:
    virtual ~list_model() = default;

    virtual size_t row_count() const = 0;
    virtual std::string text(size_t row, size_t column) const = 0;
    // A background (bg) colours the whole cell, except on selected rows.
    virtual text_style style(size_t /*row*/, size_t /*column*/) const { return {}; }
    // A small icon before a text cell's text (a file's type); painted cells
    // ignore it.
    virtual rap::icon icon(size_t /*row*/, size_t /*column*/) const { return {}; }
    // Called for columns declared `painted`; text() is still used for
    // accessibility, type-ahead and copy.
    virtual void paint(size_t /*row*/, size_t /*column*/, painter&, const cell_info&) const {}
    // For a list made with checkboxes: each row's box, in the first column.
    virtual check_state checked(size_t /*row*/) const { return check_state::unchecked; }
    // The user clicked a row's box (or pressed Space on it). The model
    // decides what that means (TortoiseGit's lists apply it to the whole
    // selection when the row is selected) and reports what changed.
    virtual void toggle_checked(size_t /*row*/) {}

    void add_observer(list_observer* o) { observers_.push_back(o); }
    void remove_observer(list_observer* o) { std::erase(observers_, o); }

protected:
    void notify_reset()
    {
        for (auto* o : observers_) o->model_reset();
    }
    void notify_changed(size_t first, size_t n)
    {
        for (auto* o : observers_) o->rows_changed(first, n);
    }
    void notify_inserted(size_t first, size_t n)
    {
        for (auto* o : observers_) o->rows_inserted(first, n);
    }
    void notify_removed(size_t first, size_t n)
    {
        for (auto* o : observers_) o->rows_removed(first, n);
    }

private:
    std::vector<list_observer*> observers_;
};

struct column
{
    std::string title;
    extent width;          // initial width; auto_ = fit header
    bool stretch = false;  // takes spare width
    bool painted = false;  // drawn through list_model::paint
    bool sortable = false;
    bool visible = true;  // hidden columns can be shown from a header menu
    rap::align align = rap::align::start;
};

enum class selection_mode
{
    none,
    single,
    multi
};
enum class sort_order
{
    ascending,
    descending
};

// -- widgets ------------------------------------------------------------------

class widget
{
public:
    virtual ~widget() = default;
    virtual void set_enabled(bool) = 0;
    virtual void set_visible(bool) = 0;
    virtual void set_tooltip(const std::string&) = 0;
    virtual void focus() = 0;
    // True while it, or a control inside it, has the keyboard focus.
    virtual bool has_focus() const = 0;
};

class label : public widget
{
public:
    virtual void set_text(const std::string&) = 0;
};

class button : public widget
{
public:
    virtual void set_text(const std::string&) = 0;
    virtual void set_icon(const icon&) = 0;
    virtual void on_click(std::function<void()>) = 0;
};

// Toolbar item: icon (and/or label) button, optionally a toggle.
class tool_button : public widget
{
public:
    virtual void set_icon(const icon&) = 0;
    virtual bool checked() const = 0;   // toggles only
    virtual void set_checked(bool) = 0;  // doesn't fire on_toggled
    virtual void on_click(std::function<void()>) = 0;
    virtual void on_toggled(std::function<void(bool)>) = 0;
};

// A value on a continuous range: horizontal track with a knob.
class slider : public widget
{
public:
    virtual double value() const = 0;
    virtual void set_value(double) = 0;  // doesn't fire on_changed
    virtual void set_range(double min, double max) = 0;
    virtual void on_changed(std::function<void(double)>) = 0;
};

// A number with up/down steppers.
class spin : public widget
{
public:
    virtual double value() const = 0;
    virtual void set_value(double) = 0;  // doesn't fire on_changed
    virtual void set_range(double min, double max) = 0;
    virtual void on_changed(std::function<void(double)>) = 0;
};

// Shows a colour; clicking opens the platform's colour chooser.
class color_button : public widget
{
public:
    virtual rap::color color() const = 0;
    virtual void set_color(rap::color) = 0;  // doesn't fire on_changed
    virtual void on_changed(std::function<void(rap::color)>) = 0;
};

class check : public widget
{
public:
    virtual bool checked() const = 0;  // false while indeterminate
    virtual void set_checked(bool) = 0;
    virtual check_state state() const = 0;
    virtual void set_state(check_state) = 0;
    virtual void on_toggled(std::function<void(bool)>) = 0;  // user clicks only
};

// Button that drops down a menu below itself (TortoiseGit's "Walk
// Behavior" and "View"), with a drop-down arrow.
class menu_button : public widget
{
public:
    virtual void set_text(const std::string&) = 0;
    virtual void set_icon(const icon&) = 0;  // before the text (a magnifier, an arrow)
    // Drawn pressed while true, e.g. while any of its options is active.
    virtual void set_pushed(bool) = 0;
    // Called each time the menu opens, so check marks are current.
    virtual void on_menu(std::function<menu()>) = 0;
};

class progress_bar : public widget
{
public:
    virtual void set_fraction(float) = 0;  // 0..1
    virtual void pulse() = 0;             // activity of unknown length
};

struct date
{
    int year = 1970, month = 1, day = 1;  // month and day are 1-based
    auto operator<=>(const date&) const = default;
};

class date_picker : public widget
{
public:
    virtual rap::date date() const = 0;
    virtual void set_date(rap::date) = 0;
    virtual void on_changed(std::function<void(rap::date)>) = 0;  // user changes only
};

// Single-line text entry.
class entry : public widget
{
public:
    virtual std::string text() const = 0;
    virtual void set_text(const std::string&) = 0;
    virtual void set_placeholder(const std::string&) = 0;
    // Shows its text (selectable, copyable) without letting it be edited.
    virtual void set_read_only(bool) {}
    // The toolkit's frame round it, or none (spec::entry::frame).
    virtual void set_frame(bool) {}
    virtual void on_changed(std::function<void(const std::string&)>) = 0;
    virtual void on_activate(std::function<void()>) = 0;  // Enter pressed
};

// Drop-down with a fixed set of choices (QComboBox / GtkDropDown).
class drop_down : public widget
{
public:
    virtual void set_items(std::vector<std::string>) = 0;
    virtual int current() const = 0;  // -1 = none
    virtual void set_current(int) = 0;
    virtual void on_changed(std::function<void(int)>) = 0;
};

// Editable drop-down: free text plus suggestions.
class combo : public widget
{
public:
    virtual void set_items(std::vector<std::string>) = 0;
    virtual std::string text() const = 0;
    virtual void set_text(const std::string&) = 0;
    virtual void on_changed(std::function<void(const std::string&)>) = 0;
    virtual void on_activate(std::function<void()>) = 0;
};

// Multi-line text: read-only with styled spans (diffs, details) or editable.
class text_view : public widget
{
public:
    struct span
    {
        size_t begin, end;  // byte offsets into the text
        text_style style;
    };
    virtual std::string text() const = 0;
    virtual void set_text(const std::string&, std::vector<span> spans = {}) = 0;
    virtual void append(const std::string&, const text_style& = {}) = 0;
    virtual void set_read_only(bool) = 0;
    virtual void on_changed(std::function<void()>) = 0;
    // The selected range as byte offsets [begin, end) into text(), like
    // Span's; begin == end is the caret.
    virtual std::pair<size_t, size_t> selection() const = 0;
    // Selects [begin, end) and scrolls it into view.
    virtual void select(size_t begin, size_t end) = 0;
};

class list : public widget
{
public:
    virtual void set_model(list_model*) = 0;              // not owned; must outlive the list
    virtual std::vector<size_t> selection() const = 0;  // ascending
    virtual std::optional<size_t> current() const = 0;
    virtual void select(const std::vector<size_t>& rows) = 0;
    virtual void set_current(size_t row) = 0;
    virtual void scroll_to(size_t row) = 0;

    // The model is virtual, so sorting is the app's job: the list reports
    // header clicks and shows the indicator it is told to.
    virtual void set_sort_indicator(std::optional<size_t> column, sort_order) = 0;

    virtual std::vector<float> column_widths() const = 0;  // for saving state
    virtual void set_column_widths(const std::vector<float>&) = 0;
    virtual bool column_visible(size_t column) const = 0;
    virtual void set_column_visible(size_t column, bool) = 0;
    // The columns in display order, as indices into the declared columns, for
    // saving state: users reorder them by dragging the headers. An order that
    // isn't a permutation of all the columns is ignored.
    virtual std::vector<size_t> column_order() const = 0;
    virtual void set_column_order(const std::vector<size_t>&) = 0;

    virtual void on_selection_changed(std::function<void()>) = 0;
    virtual void on_activate(std::function<void(size_t row)>) = 0;  // double-click / enter
    virtual void on_sort_requested(std::function<void(size_t column, sort_order)>) = 0;
    // Right-click / menu key; return an empty menu for none.
    virtual void on_context_menu(std::function<menu(std::optional<size_t> row)>) = 0;
    // Right-click on a column header (e.g. a menu of columns to show).
    virtual void on_header_menu(std::function<menu(size_t column)>) = 0;
    // A context menu that also knows where a right-click on a row landed:
    // its column and the point in that cell (a label within it), as
    // cell_drag::begin gets them. When set, right-clicks on rows use it; the
    // menu key and clicks outside the rows still use on_context_menu.
    virtual void on_cell_context_menu(std::function<menu(size_t row, size_t column, point_f in_cell)>) = 0;

    // Drag-reordering. With a handler set, the user can drag the selected
    // rows to a new place. `destination` is the row they go in front of, in
    // the order before the move (row_count() = the end). The app moves its
    // data and sets the new selection; return false to refuse the drop.
    using reorder_fn = std::function<bool(const std::vector<size_t>& rows, size_t destination)>;
    virtual void on_reorder(reorder_fn) = 0;
    virtual void set_reorderable(bool) = 0;  // on once on_reorder is set

    // Dragging something out of a cell (not the row itself) onto another
    // row: a branch label onto a commit. `begin` decides from where a press
    // landed whether a drag starts there; past the drag threshold the label
    // follows the pointer, rows `can_drop` accepts highlight, and a release
    // over one calls `drop`. Esc, or a release anywhere else, cancels. A
    // press `begin` declines works as usual (selection, on_reorder).
    struct cell_drag_start
    {
        std::string label;  // shown under the pointer
        std::string data;   // the app's own: handed back to can_drop and drop
    };
    struct cell_drag
    {
        std::function<std::optional<cell_drag_start>(size_t row, size_t column, point_f in_cell)> begin;
        std::function<bool(size_t from, size_t to, const std::string& data)> can_drop;
        std::function<void(size_t from, size_t to, const std::string& data)> drop;
    };
    virtual void on_cell_drag(cell_drag) = 0;
};

// Draggable divider between panes.
class splitter : public widget
{
public:
    // One size per pane along the split, in logical pixels, for saving
    // layout. set_sizes() restores them (sizes of 0 are left alone); called
    // before the window first shows, it replaces the stretch-based layout.
    virtual std::vector<float> sizes() const = 0;
    virtual void set_sizes(const std::vector<float>&) = 0;
    // A divider dragged by the user, once the drag ends; not layout, the
    // window resizing or set_sizes(). For saving a size the user chose.
    virtual void on_moved(std::function<void()>) = 0;
    // How wide a divider is across the split, in logical pixels: what a pane
    // shown adds besides its own size (to grow the window by just that).
    virtual float divider() const = 0;
};

class tabs : public widget
{
public:
    virtual int current() const = 0;
    virtual void set_current(int) = 0;
    virtual void on_changed(std::function<void(int)>) = 0;
};

// -- canvas -------------------------------------------------------------------

enum modifier : unsigned
{
    mod_shift = 1,
    mod_ctrl = 2,
    mod_alt = 4,
    mod_meta = 8,  // Super / Windows / Command
};

enum class pointer_event_type
{
    press,
    release,
    move,
    enter,
    leave,
};

struct pointer_event
{
    pointer_event_type type = pointer_event_type::move;
    point_f pos;      // logical pixels, canvas-relative
    int button = 0;  // Press/Release: 1 left, 2 middle, 3 right, 4/5 back/forward
    int clicks = 1;  // Press: 2 for the second press of a double click
    unsigned modifiers = 0;
};

struct scroll_event
{
    point_f pos;
    // Wheel notches (+1 = one notch down/right), or pixel-precise deltas
    // from a touchpad when `precise` is set.
    float dx = 0, dy = 0;
    bool precise = false;
    unsigned modifiers = 0;
};

// Keys that have no text. Others arrive as key::character with `codepoint`.
enum class key
{
    character,
    escape,
    enter,
    tab,
    backspace,
    delete_,
    insert,
    space,
    left,
    right,
    up,
    down,
    home,
    end,
    page_up,
    page_down,
    f1,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    f11,
    f12,
    other,
};

struct key_event
{
    bool press = true;  // false: release
    rap::key key = rap::key::other;
    char32_t codepoint = 0;  // key::character: lowercase/unshifted where it has one
    unsigned modifiers = 0;
    bool repeat = false;
};

enum class cursor
{
    default_,
    pointer,  // a link / clickable thing
    text,
    crosshair,
    grab,
    grabbing,
    zoom_in,
    zoom_out,
    move,
    wait,
    hidden,
    // Dragging an edge or corner: left-right, up-down, and the diagonals
    // (top-left to bottom-right, top-right to bottom-left).
    resize_ew,
    resize_ns,
    resize_nwse,
    resize_nesw,
};

// -- frames: an image layer under a canvas's painting -------------------------
//
// A canvas can show a frame the app made under what on_paint draws: an image
// viewer renders on its own (on a GPU worker, or the CPU) and presents the
// result, and the overlay (text, selection, a HUD) is painted over it.

// How a frame's values map to colour. sRGB now; the rest are for HDR.
enum class transfer
{
    srgb,    // the sRGB curve, 0..1
    linear,  // scRGB: linear, 1.0 = SDR white, may go past 1 and below 0
    pq,      // SMPTE ST 2084
    hlg,     // ARIB STD-B67
};
enum class primaries
{
    bt709,  // sRGB's
    display_p3,
    bt2020,
};
struct frame_color
{
    rap::transfer curve = transfer::srgb;
    rap::primaries gamut = primaries::bt709;
    float sdr_white_nits = 203;  // what 1.0 (linear) or SDR white (pq, hlg) means
    bool premultiplied = true;   // alpha; opaque frames ignore it
};

// What a canvas's frames should be: their exact size, and where and how the
// app can render them for the canvas to take without a copy.
struct frame_target
{
    // The pixels a frame covering the whole canvas has: the toolkit's own
    // rounding of size() at `scale`, which can differ from device_scale()
    // (GTK 3 draws at 2 where the display shows 1.5).
    int width = 0, height = 0;
    float scale = 1;  // frame pixels per logical pixel

    enum class api
    {
        pixels,  // CPU pixels only: present(image)
        dmabuf,  // Linux
        d3d11,   // Windows
    } kind = api::pixels;
    // The GPU the canvas is shown on: Linux, the DRM render node's dev_t;
    // Windows, the adapter LUID (LowPart | HighPart << 32). 0 when unknown.
    uint64_t device = 0;
    struct format
    {
        uint32_t code = 0;                // DRM fourcc (dmabuf) or DXGI_FORMAT (d3d11)
        std::vector<uint64_t> modifiers;  // dmabuf: importable modifiers; d3d11: empty
    };
    std::vector<format> formats;  // in the toolkit's order of preference
    bool hdr = false;             // the output shows frame_color beyond sRGB as such
};

// A frame on the GPU. The app keeps ownership of the buffer and its
// handles; rapport duplicates what it keeps past present(). Rows run top
// first, as they're shown: a GL app renders flipped (a framebuffer is bottom
// first), and no backend flips.
struct gpu_frame
{
    int width = 0, height = 0;  // pixels
    frame_color color;

    struct dmabuf
    {
        uint32_t fourcc = 0;
        uint64_t modifier = 0;
        int planes = 1;
        int fd[4] = {-1, -1, -1, -1};
        uint32_t offset[4] = {}, stride[4] = {};
        int acquire_fence = -1;  // a sync_file signalled when the frame's rendered; -1: implicit sync
    };
    struct d3d11
    {
        void* shared_handle = nullptr;  // NT handle of an ID3D11Texture2D (MISC_SHARED_NTHANDLE)
        uint32_t dxgi_format = 0;
        void* fence_handle = nullptr;  // a shared ID3D11Fence to wait on; nullptr: none
        uint64_t fence_value = 0;
    };
    std::variant<dmabuf, d3d11> handle;

    // On the UI thread, exactly once, when rapport has stopped reading the
    // frame (it was replaced or copied, the canvas went, or it couldn't be
    // used); the app may then reuse or free the buffer.
    std::function<void()> release;
};

// A surface the app draws itself (an image view, a chart). Everything else
// in a window should stay a native widget.
class canvas : public widget
{
public:
    virtual void invalidate() = 0;  // repaint soon
    virtual size_f size() const = 0;
    virtual float device_scale() const = 0;
    virtual void set_cursor(cursor) = 0;

    // Paint handler: draws the whole canvas, or over a presented frame only
    // what goes on top of it.
    virtual void on_paint(std::function<void(rap::painter&, rap::size_f)>) = 0;
    // Size changed, or the device scale did (moved to another monitor).
    virtual void on_resize(std::function<void(size_f)>) = 0;
    // Input handlers return true when they used the event.
    virtual void on_pointer(std::function<bool(const pointer_event&)>) = 0;
    virtual void on_scroll(std::function<bool(const scroll_event&)>) = 0;
    virtual void on_key(std::function<bool(const key_event&)>) = 0;

    // Called once per displayed frame (paced by the display) with the frame
    // time in seconds, until the subscription is dropped. For animation.
    [[nodiscard]] virtual subscription on_frame(std::function<void(double)>) = 0;

    // Frames (see frame_target): what to render them as, and when that
    // changes (a resize, a new scale, another GPU).
    virtual rap::frame_target target() const = 0;
    virtual void on_frame_target(std::function<void(const rap::frame_target&)>) = 0;
    // Shows a frame under on_paint's drawing from the next displayed frame,
    // replacing the one before (the newest wins; the display paces it). It's
    // placed at the top left, one frame pixel to one of target()'s, never
    // scaled: render at target().width x height. Where it doesn't reach (a
    // resize catching up) rapport fills the canvas's background; past the
    // canvas (a shrink catching up) it's clipped. While a frame is shown
    // on_paint draws only the overlay, leaving it showing. UI thread only:
    // post from a worker.
    virtual void present(gpu_frame frame)
    {
        // Until a backend takes GPU frames (its target() says so).
        if (frame.release) frame.release();
    }
    // The same from CPU pixels, on every backend; kept until replaced.
    virtual void present(std::shared_ptr<const rap::image>, frame_color = {}) = 0;
    // The GPU device this canvas presents on (gpu::device::of): null until it
    // has one (on_frame_target says when), and on backends without one.
    virtual std::shared_ptr<gpu::device> gpu_device() const { return nullptr; }
    // A texture of that device, shown as present(gpu_frame) shows a frame.
    // `ready` is the GPU work that made it, waited for on the GPU, never on
    // the UI thread. Refused (and logged) when it's another device's, or not
    // one the canvas can draw: rgba8 or rgba16f, one level. The canvas holds
    // it until it's replaced and the GPU has finished drawing it.
    virtual void present(gpu::texture_ptr, gpu::ticket ready, frame_color = {}) {}
    // Back to on_paint alone.
    virtual void clear_frame() = 0;
};

// -- windows ------------------------------------------------------------------

enum class dialog_role
{
    none,
    ok,
    cancel,
    apply,
    close,
    yes,
    no,
    help,
    reset
};

class window
{
public:
    virtual ~window() = default;  // destroys all widgets in the window
    virtual void set_title(const std::string&) = 0;
    virtual void show() = 0;
    virtual void close() = 0;

    // Size of the window's content in logical px, and resizing it.
    virtual size_f size() const = 0;
    virtual void resize(size_f) = 0;
    virtual void set_min_size(size_f) = 0;
    // Position in logical px of the frame's top-left. Where the platform
    // doesn't let apps place windows (Wayland), move() does nothing and
    // position() is unknown (nullopt).
    virtual std::optional<point_f> position() const = 0;
    virtual void move(point_f) = 0;
    // Usable area of the monitor the window is on (minus panels/taskbar).
    virtual rect_f work_area() const = 0;

    virtual bool fullscreen() const = 0;
    // Maximized, or tiled (snapped to half the screen) where the platform
    // says so: a size the user gave it, which an app shouldn't resize.
    virtual bool maximized() const = 0;
    virtual void set_fullscreen(bool) = 0;
    virtual void on_fullscreen_changed(std::function<void(bool)>) = 0;
    virtual void set_icon(const icon&) = 0;
    // Shows or hides the menu bar (fullscreen views hide their chrome).
    virtual void set_menu_bar_visible(bool) = 0;

    // Keys reach this before the focused widget; return true to consume. For
    // app-wide keys (navigation, fullscreen, find) that must work whatever
    // has focus. While an editable text field has focus only Escape and the
    // function keys come here; the rest are typing. Read-only text views
    // don't count as editable.
    virtual void on_key(std::function<bool(const key_event&)>) = 0;
    // Dialogs: fired once with the role of the button that ended it
    // (Cancel for Esc / close box). There is no blocking exec(): GTK4
    // has no nested main loops, so everything is callback-driven.
    virtual void on_finished(std::function<void(dialog_role)>) = 0;
    // Return false to veto closing (e.g. unsaved commit message).
    virtual void on_close_request(std::function<bool()>) = 0;
    // Files dragged in (from a file manager) and dropped anywhere on the
    // window, as local paths in UTF-8, in the order given. Setting it is
    // what makes the window accept them; other drags are refused.
    virtual void on_files_dropped(std::function<void(const std::vector<std::string>&)>) = 0;
};

}  // namespace rap
