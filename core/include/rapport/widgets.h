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
// Each backend reads what its desktop publishes for a role and derives the
// rest from the window, text and accent colours.
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
    // Status: diff add / remove / context, status pills, check marks.
    // positive is success, negative error (and destructive), warning the
    // desktop's warning (amber); neutral is quiet context, not a status.
    positive,
    negative,
    neutral,
    warning,
    // Surfaces in layers, for what an app draws itself: a header or title
    // area (GNOME's header bars, Plasma's Header colours), a raised
    // container on the window (a card, a boxed list), a tooltip.
    header,
    header_text,
    card,
    tooltip,
    tooltip_text,
    // Interaction: a hovered and a pressed item's background, the focus
    // ring, and the selection while the window isn't active.
    hover,
    pressed,
    focus,
    highlight_inactive,
    highlight_inactive_text,
    // Structure: a frame's edge, a line between items, text that can't be
    // used now.
    border,
    divider,
    disabled_text,
};
inline constexpr int theme_color_count = int(theme_color::disabled_text) + 1;

// A fixed colour in two shades, for an app's own meanings (a file's status,
// a kind of row) that mustn't follow the theme's roles, as an accent would
// take them, yet must still read: `light` where the background it's drawn on
// is light, `dark` where it's dark (a list's base, a canvas's window), so it
// follows the desktop's scheme and an app's own override alike.
struct scheme_color
{
    color light, dark;
};

// A fixed colour, a theme role or a scheme colour, resolved at paint time.
using paint = std::variant<theme_color, color, scheme_color>;

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
    case theme_color::warning: return {154, 103, 0};
    case theme_color::header: return {240, 240, 240};
    case theme_color::card:
    case theme_color::tooltip: return {255, 255, 255};
    case theme_color::header_text:
    case theme_color::tooltip_text:
    case theme_color::highlight_inactive_text: return {0, 0, 0};
    case theme_color::hover: return {235, 235, 235};
    case theme_color::pressed: return {215, 215, 215};
    case theme_color::focus: return {0, 90, 180};
    case theme_color::highlight_inactive: return {225, 225, 225};
    case theme_color::border: return {160, 160, 160};
    case theme_color::divider: return {200, 200, 200};
    case theme_color::disabled_text: return {128, 128, 128};
    }
    return {0, 0, 0, 0};  // none: rapport-theme-palette-test fails on a role missing here
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

class painter;

// An icon for a button or toolbar item: a named theme icon, an app-drawn
// one, or both (the drawing is the fallback where the name isn't found,
// and is always used where there is no icon theme, e.g. Win32).
struct icon
{
    // freedesktop icon name, e.g. "go-up", "document-save".
    std::string name;
    // The desktop's icon for this kind of file, as its file manager shows
    // it (for_file). Only the name matters (its extension or MIME type); the
    // file needn't exist, and is never read: the disk isn't touched (for a
    // path's own icon, which is, see `path`). A name ending in '/' is a
    // folder.
    std::string file;
    // Draws the icon into `size` (logical px) on a Painter with the right
    // device_scale. Draw with theme colours (theme_color::window_text) so it
    // follows dark mode; backends redraw it when the theme or scale changes.
    std::function<void(rap::painter&, rap::size_f)> draw;
    // Drawn over the icon in its square, as a file manager's overlays: a
    // badge on a row's icon in a list (the only place it's drawn).
    std::function<void(rap::painter&, rap::size_f)> overlay;
    // The desktop's own icon for this very path, as its file manager shows
    // it (for_path): a drive's (Windows: C: with the Windows logo, a USB
    // stick's, an optical drive's with its disc's, an autorun's), a folder's
    // own (desktop.ini, GIO's metadata), a program's. This reads the disk
    // (`file` never does): the path, its drive, its autorun.inf or
    // desktop.ini, which can be slow (a disconnected network drive, an
    // optical drive spinning up), so backends find it in the background and
    // never wait for it on the UI thread. Until it's known, and where the
    // backend or the path has none, `name` (else `draw`) is shown instead; it
    // changes in place once found. Found once per path and size for the
    // process: a drive whose media changes keeps the icon it was first found
    // with, and a path found to have none isn't asked again. Qt shows `name`
    // only (as Dolphin, which picks drive icons by device class, not path).
    std::string path;

    static icon named(std::string n) { return {std::move(n), {}, {}, {}, {}}; }
    static icon for_file(std::string file_name) { return {{}, std::move(file_name), {}, {}, {}}; }
    static icon drawn(std::function<void(rap::painter&, rap::size_f)> f)
    {
        return {{}, {}, std::move(f), {}, {}};
    }
    // `fallback`: a freedesktop name for the meantime and for where there's
    // none, e.g. for_path("C:\\", "drive-harddisk").
    static icon for_path(std::string p, std::string fallback = {})
    {
        return {std::move(fallback), {}, {}, {}, std::move(p)};
    }
    explicit operator bool() const { return !name.empty() || !file.empty() || bool(draw) || !path.empty(); }
};

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
    // Beside the label, at the toolkit's menu icon size, where the desktop
    // shows menu icons: Windows and Qt; GTK's menus have none (GNOME's way).
    rap::icon icon;

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

// Where a picture `w` by `h` goes in `box` keeping its shape: as large as
// fits, placed by `align_x` and `align_y` (0 left or top, 0.5 centred, 1
// right or bottom). For painter::image, so a picture in a box laid out
// smaller than asked shrinks whole rather than squashed:
//     p.image(img, {0, 0, iw, ih}, rap::fit_inside({0, 0, size.w, size.h}, iw, ih));
inline rect_f fit_inside(rect_f box, float w, float h, float align_x = 0.5f, float align_y = 0.5f)
{
    if (w <= 0 || h <= 0 || box.w <= 0 || box.h <= 0) return {box.x, box.y, 0, 0};
    const float k = std::min(box.w / w, box.h / h);
    const float fw = w * k, fh = h * k;
    return {box.x + (box.w - fw) * align_x, box.y + (box.h - fh) * align_y, fw, fh};
}

struct cell_info
{
    size_f size;
    bool selected = false;
    bool focused = false;
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
    // The tooltip at a point of a cell (cell coordinates, as cell_drag::begin
    // gets them): asked again as the pointer moves within the cell, so a
    // painted cell can say something different over each part. Empty: none.
    virtual std::string tooltip(size_t /*row*/, size_t /*column*/, point_f /*in_cell*/) const { return {}; }

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
// Where a value sits along a slider's track, and back, for a track that
// isn't even in the value: a logarithmic one, say. Unset, the track is the
// value.
struct slider_map
{
    std::function<double(double)> to_track, to_value;
    double track(double v) const { return to_track ? to_track(v) : v; }
    double value(double t) const { return to_value ? to_value(t) : t; }
};

class slider : public widget
{
public:
    virtual double value() const = 0;
    virtual void set_value(double) = 0;  // doesn't fire on_changed
    virtual void set_range(double min, double max) = 0;
    // The ticks (spec::slider::marks), for a slider whose notable values
    // change: the value as it was, say.
    virtual void set_marks(std::vector<std::pair<double, std::string>> marks) = 0;
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

// One item of a tree: its label, the app's id for it, and its children.
struct tree_item
{
    std::string label;
    std::string id;  // unique in the tree, not empty: what selection and the handlers name it by
    std::vector<tree_item> children;
    bool expanded = false;  // shown open at first
    // A box before the label (a folder that stands for everything in it,
    // checked, unchecked or mixed); none when unset.
    std::optional<check_state> check;
    // May have children that aren't loaded yet: shows a chevron, and asks for
    // them when it's opened (tree::on_expanding). Ignored with `children`.
    bool has_children = false;
    // A line above the item, setting it apart from the ones before it (a
    // file manager's groups of places). Drawn only when the item has a
    // sibling before it: never over a tree's first row or right under its
    // parent. It's only drawn: the item is selected, reached by the keys and
    // read out as any other, and nothing else is in the tree for it.
    bool separator_before = false;
    // Before the label, as a list cell's: a tree where no item has one looks
    // exactly as if this weren't here. Items without one, in a tree where
    // others have one, may leave room for it, depending on the toolkit.
    // Trees don't draw its overlay; GTK trees show named and file icons only.
    rap::icon icon;
};

// A hierarchy of labelled items, one selected at a time (folders, say): the
// toolkit's own tree view. The app hands over the whole tree, and again when
// it changes.
class tree : public widget
{
public:
    // Replaces every item. The selection stays on the same id if it's still
    // there, and items keep being open or closed as the user left them;
    // new ones take tree_item::expanded.
    virtual void set_items(std::vector<tree_item>) = 0;
    virtual std::optional<std::string> selected() const = 0;
    // Selects `id`, opening its ancestors and scrolling it into view; an id
    // that isn't there clears the selection.
    virtual void select(const std::string& id) = 0;
    virtual void set_expanded(const std::string& id, bool) = 0;

    virtual void on_selection_changed(std::function<void()>) = 0;  // the user's changes only
    virtual void on_activate(std::function<void(const std::string& id)>) = 0;  // double-click / Enter
    // Right-click / menu key. A right-click selects its item first, as file
    // managers do; nullopt for blank space. Return an empty menu for none.
    virtual void on_context_menu(std::function<menu(std::optional<std::string> id)>) = 0;

    // Items' boxes (tree_item::check). As a list's model does, the app owns
    // their states: a click on a box (or Space on its item) only reports it
    // through on_toggled, and the app sets what that means with set_check,
    // the item's and others' (a folder's box sets what's in it, whose own
    // make the folder's mixed). An item made without a box gets none.
    virtual void set_check(const std::string& id, check_state) = 0;
    virtual void on_toggled(std::function<void(const std::string& id)>) = 0;

    // Loading children on demand (tree_item::has_children).
    //
    // A request to load, not a report of a change: called whenever a node
    // that has_children and has nothing loaded becomes open, whoever opened
    // it (the user, set_expanded, select() opening ancestors, tree_item::
    // expanded, or set_items and set_children keeping a node open). Once
    // until it's answered: closing and reopening with an answer outstanding
    // doesn't ask again. Once loaded, reopening never asks; the app refreshes
    // with set_children. The node shows open at once with a placeholder row,
    // which the app never sees. Called after any change under way has
    // finished, so answering from inside the handler is fine.
    virtual void on_expanding(std::function<void(const std::string& id)>) = 0;
    // The answer, at any time after, or a refresh with nobody asking. Replaces
    // everything under `id` as set_items replaces the whole tree: the
    // selection stays on the same id if it's still there, and is cleared if
    // not; items keep being open or closed as the user left them; new ones
    // take tree_item::expanded; nothing is called back for the selection.
    // Empty: no children, so no chevron, and the node closes. An answer for a
    // node closed since applies and leaves it closed. An id that isn't there
    // is ignored (the node went while the answer was on its way).
    virtual void set_children(const std::string& id, std::vector<tree_item>) = 0;
    // One node in place: its children, open state, selection and box are left
    // alone. has_children false with an answer outstanding is an empty
    // answer. has_children true on a node with nothing loaded brings its
    // chevron back (a watcher saw a new subfolder); on one with children
    // loaded it does nothing.
    virtual void update_item(const std::string& id, const std::string& label, const rap::icon&,
                             bool has_children) = 0;
    // An item's tree_item::separator_before, in place (update_item leaves it
    // alone). The item keeps its id, selection and open state, as do the
    // others. How it's shown is the toolkit's: on Win32 only that item's row
    // is redrawn; on GTK 3 a separator row comes or goes before it, moving
    // the rows below. An id that isn't there is ignored.
    virtual void set_separator_before(const std::string& id, bool) = 0;
};

class date_picker : public widget
{
public:
    virtual rap::date date() const = 0;
    virtual void set_date(rap::date) = 0;
    virtual void on_changed(std::function<void(rap::date)>) = 0;  // user changes only
};

// A button inside an entry, at its end (layout's entry::trailing_action).
class entry_action
{
public:
    virtual ~entry_action() = default;
    virtual void set_icon(const icon&) = 0;
    virtual void set_tooltip(const std::string&) = 0;
    // Drawn held down while checked: an option in effect (a date range set).
    virtual void set_checked(bool) = 0;
    virtual bool checked() const = 0;
    virtual void set_enabled(bool) = 0;
    // Clicking drops this menu down under the icon, built as it opens, in
    // place of on_click (spec::entry_action::on_menu).
    virtual void on_menu(std::function<menu()>) = 0;
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

// Keys held with a pointer action (a canvas's events, a list drop).
enum modifier : unsigned
{
    mod_shift = 1,
    mod_ctrl = 2,
    mod_alt = 4,
    mod_meta = 8,  // Super / Windows / Command
};

// What a drop does with what was dragged.
enum class drop_action
{
    move,
    copy,
};

// Data offered by a drag (and, later, the clipboard) in one or more
// formats, best first: MIME type and bytes (binary-safe). An app's own
// type ("application/x-tortilla-commits") travels between its windows and
// processes; "text/plain" (UTF-8) reaches other programs.
struct data_offer
{
    std::vector<std::pair<std::string, std::string>> formats;
};

// Where scroll_to puts a row in the view: as little scrolling as brings it
// into view, or at the view's top, middle or bottom.
enum class scroll_align
{
    nearest,
    start,
    center,
    end,
};

class list : public widget
{
public:
    virtual void set_model(list_model*) = 0;              // not owned; must outlive the list
    virtual std::vector<size_t> selection() const = 0;  // ascending
    virtual std::optional<size_t> current() const = 0;
    virtual void select(const std::vector<size_t>& rows) = 0;
    virtual void set_current(size_t row) = 0;
    virtual void scroll_to(size_t row, scroll_align = scroll_align::nearest) = 0;
    // Where a row sits in the view: its top in logical px below the top of
    // the rows area (under any header); nullopt while it isn't in view, or
    // the rows aren't laid out. With set_row_offset, a view repopulated (a
    // filter, a reload) keeps the row where the user saw it.
    virtual std::optional<float> row_offset(size_t row) const = 0;
    // Scrolls so the row's top sits `offset` below the top of the rows area,
    // as near as the view's ends allow; a row already there moves nothing.
    virtual void set_row_offset(size_t row, float offset) = 0;

    // The model is virtual, so sorting is the app's job: the list reports
    // header clicks and shows the indicator it is told to.
    virtual void set_sort_indicator(std::optional<size_t> column, sort_order) = 0;

    virtual std::vector<float> column_widths() const = 0;  // for saving state
    // A width of 0 leaves its column as it is.
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
    // A reorder that can also copy, in place of on_reorder: dropped with
    // the keys of `copy_modifiers` held (mod_ctrl, as file managers), the
    // rows are copied rather than moved, and while they're held the pointer
    // shows the toolkit's copy cursor where it has one. The keys are read
    // as the drag goes and at the drop, not at the press (Ctrl-click
    // selects).
    using reorder_copy_fn =
        std::function<bool(const std::vector<size_t>& rows, size_t destination, drop_action)>;
    virtual void on_reorder_or_copy(reorder_copy_fn, unsigned copy_modifiers) = 0;
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

    // Dragging rows out to another window or program, and dropping data in
    // from one, through the system's drag and drop, so across processes.
    //
    // drag_out: dragging the selected rows past the threshold asks `begin`
    // for what they carry; nullopt starts no drag. In a list with
    // on_reorder, a drag that stays inside the list reorders as before; the
    // system drag starts only once the pointer leaves the list's bounds.
    // The drop is a copy: nothing is removed from the source.
    //
    // drop_in: data of one of `types` (the first the drag offers wins) can
    // be dropped between rows. While it hovers, the insertion line shows
    // where it goes and the list scrolls near its top and bottom edges.
    // `at` is the row it goes in front of (row_count() = the end).
    // can_drop sees only the type: on some systems (Wayland) the data
    // arrives only at the drop. drop returns false to refuse it.
    struct drag_out
    {
        std::function<std::optional<data_offer>(const std::vector<size_t>& rows)> begin;
    };
    struct drop_in
    {
        std::vector<std::string> types;
        std::function<bool(size_t at, const std::string& type)> can_drop;  // unset: always
        std::function<bool(size_t at, const std::string& type, const std::string& data)> drop;
    };
    virtual void on_drag_out(drag_out) = 0;
    virtual void on_drop_in(drop_in) = 0;
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

class window;

// One of a window's document tabs (window_spec::tabs): a folder in a file
// manager, a file in an editor.
struct document_tab
{
    std::string id;  // the app's own, unique among the window's tabs
    std::string title;
    rap::icon icon;
    std::string tooltip;  // e.g. the full path the title shortens
};

// A window's row of document tabs, as a browser's: under the menu bar,
// across the window, above its content. The tabs only say which document is
// current; the app swaps the content when it changes.
//
// Calls from the app (insert, remove, select...) never fire the handlers;
// the user's clicks and keys do, once each. The keys, wherever the focus is:
// Ctrl+T on_new, Ctrl+W on_close for the current tab, Ctrl+Tab and
// Ctrl+Shift+Tab the next and previous (round), Ctrl+1..8 the first eight
// and Ctrl+9 the last, Ctrl+Shift+PgUp and Ctrl+Shift+PgDn move the current
// tab left and right; a middle click closes the tab under it.
// Closing is the app's to do (remove): it may ask first, or keep the last
// tab. A bar with one tab is still shown; set_visible hides it.
//
// Tabs are dragged along the row to reorder them, the others making room as
// the dragged one passes (Escape puts it back, where the toolkit allows: on
// Windows). Moved, the tab is already where the user put it: on_reorder
// tells the app its id and new index, once, as the drag is let go (or for
// each key), and only if its index changed; ids() says the new order.
// Without on_detach, a drag that leaves the row keeps the tab at its end; it
// doesn't take the tab out of the window.
//
// Tearing off and dropping (see on_detach, on_receive and set_group): a tab
// dragged out of the row tears off into a window of its own, which the app
// makes; dragged over another window's row in the same group, it shows
// where it would land, and let go there it moves into that window. The
// windows' tabs only ever change through the app's own calls (remove,
// insert) from its handlers: rapport asks, the app does. Per toolkit:
//  - Windows: torn off past a short distance out of the row (above, below,
//    or past the window's sides), the new window is carried by the system's
//    own move loop until let go, and a window's only tab drags its window
//    the same way; Escape puts a tab back, and once it's torn off sends it
//    home: the window it came from is asked to take it back at its old
//    place (its on_receive, the torn-off window the source), whatever the
//    group, and the app closes the emptied window.
//  - GTK: GtkNotebook's own drag and drop; torn off as it's let go outside
//    any notebook, the window shown where it was let go. GTK 4 says nothing
//    of where: the window goes where the desktop puts it (under Wayland it
//    can't be put at the pointer at all).
//  - Qt: not by dragging yet; the menu's Move to new window works.
//
// The right-click menu: New tab, Close tab, Move to new window (with
// on_detach), then the app's own items (on_context_menu) after a separator.
// From the keys too (Shift+F10, the Menu key) on a focused tab: where the
// strip takes the focus, as Windows 11's in the caption does from F6; the
// others never take it.
class document_tabs : public widget
{
public:
    // A tab leaving its window, as a drag let go over another window's row
    // or a torn-off window dropped there: what the target's on_receive is
    // given. `index` is where in the target's row it was dropped (0 to its
    // count); `source` the window whose tabs hold it now (a torn-off window,
    // if the drag made one).
    struct transfer
    {
        std::string id;
        std::string title;
        rap::icon icon;
        std::string tooltip;
        int index = -1;
        rap::window* source = nullptr;
    };
    // A tab torn off into a window of its own. A tab is only an entry in a
    // row: whatever it shows (a folder, a repository, a document) is the
    // app's, so the window is the app's to make, and rapport only asks.
    // `screen` is where the pointer is, in the source window's logical px
    // (window::position's units); nullopt for the menu's Move to new window.
    //
    // The handler makes a window for the tab's content, whose tabs spec
    // already holds a tab with this id (its title and icon as the app likes),
    // removes the tab from the source window's row (its bind's remove), and
    // returns the new window, not yet shown. Rapport keeps no tab state of
    // its own across windows: the entry goes from one row to the other only
    // through the app's own calls. (A window's only tab is never torn off:
    // dragged, it drags its window; the menu's item is disabled.)
    //
    // Dragged, rapport puts the window under the pointer, shows it and
    // carries the drag on with it, the button still held: let go over
    // another window's row in the group, that window's on_receive is asked
    // to take the tab (see there); anywhere else, the window stays where it
    // was let go. From the menu (no point), the app places the window itself
    // before showing it, with window::move (e.g. a little down and right of
    // the source's window::position(); nothing on Wayland, where the
    // compositor places windows). Returning nullptr refuses: nothing changes,
    // and the drag goes on along the row. Asked at most once a drag.
    using detach_fn = std::function<rap::window*(const std::string& id, std::optional<point_f> screen)>;
    // A tab let go over this window's row, from another window in the same
    // group (set_group), or (on Windows) one torn off from this window and
    // sent home by Escape, group or none: `t` says which, where it was
    // dropped (or was before), and the window it's in. Rapport changes neither row. To take it the app
    // removes it from t.source's tabs, inserts it into this window's at
    // t.index (through the binds), and returns true; false leaves both rows
    // as they were, and the drag ends where it was. A source left empty
    // (a torn-off window made for the drag) is the app's to close; rapport
    // never closes it.
    using receive_fn = std::function<bool(const transfer& t)>;


    // At `index` (-1 or past the end: last). Not made current.
    virtual void insert(int index, const document_tab&) = 0;
    void add(const document_tab& t) { insert(-1, t); }
    // The current one going makes the one after it current (the one before
    // if it was last), as browsers do; current() says which.
    virtual void remove(const std::string& id) = 0;
    // The tab with t.id gets t's title, icon and tooltip.
    virtual void update(const document_tab& t) = 0;
    virtual void select(const std::string& id) = 0;
    virtual std::string current() const = 0;           // empty when there are none
    virtual std::vector<std::string> ids() const = 0;  // in order
    virtual void on_select(std::function<void(const std::string& id)>) = 0;
    virtual void on_close(std::function<void(const std::string& id)>) = 0;
    virtual void on_new(std::function<void()>) = 0;
    virtual void on_reorder(std::function<void(const std::string& id, int new_index)>) = 0;
    // Tearing off: see detach_fn. Unset, a tab dragged out stays in the row
    // and the menu has no Move to new window.
    virtual void on_detach(detach_fn) = 0;
    // Dropping in from another window: see receive_fn.
    virtual void on_receive(receive_fn) = 0;
    // The windows whose rows may trade tabs: those of the same, non-empty
    // group. Empty (the default): none, though tearing off still works.
    virtual void set_group(const std::string& group) = 0;
    // The right-click menu's own items, for the tab with `id`, after the
    // fixed ones and a separator; built each time it opens. Unset or empty:
    // the fixed ones alone.
    virtual void on_context_menu(std::function<rap::menu(const std::string& id)>) = 0;
};

// -- canvas -------------------------------------------------------------------

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
    // Navigation: the mouse's side buttons (the X buttons), and the keys
    // some keyboards have. Only presses are reported, and only to the
    // window's on_key, whatever has focus. A canvas taking a side button
    // itself (on_pointer returning true for button 4 or 5) produces none.
    back,
    forward,
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
    virtual void present(gpu::texture_ptr, gpu::ticket /*ready*/, frame_color = {}) {}
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
    // A taskbar group of the window's own, apart from the app's other
    // windows, its button showing the window's icon (Windows: its
    // AppUserModelID; best set before it shows, as the taskbar regroups a
    // window whose id changes). Empty: the app's. Elsewhere nothing: a
    // Wayland app_id is the process's, and finds its .desktop file.
    virtual void set_app_id(const std::string&) = 0;
    // Shows or hides the menu bar (fullscreen views hide their chrome).
    virtual void set_menu_bar_visible(bool) = 0;

    // Keys reach this before the focused widget; return true to consume. For
    // app-wide keys (navigation, fullscreen, find) that must work whatever
    // has focus. While an editable text field has focus only Escape, the
    // function keys and back/forward come here; the rest are typing.
    // Read-only text views don't count as editable.
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

    // The working-in-the-background pointer over this window (the shape
    // ui::set_busy gives all of them; under Qt a widget with a pointer of
    // its own keeps it here), counted: from the first begin_busy until as
    // many end_busy. The count is the window's and goes with it, so a
    // window closed while it loads leaves nothing busy.
    void begin_busy()
    {
        if (busy_count_++ == 0) busy_changed(true);
    }
    void end_busy()
    {
        if (busy_count_ > 0 && --busy_count_ == 0) busy_changed(false);
    }
    bool busy() const { return busy_count_ > 0; }

protected:
    // The pointer to show, as the count leaves or comes back to nothing.
    virtual void busy_changed(bool on) = 0;

private:
    int busy_count_ = 0;
};

}  // namespace rap
