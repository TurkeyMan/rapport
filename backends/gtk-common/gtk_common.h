// Code shared by the GTK 3 and GTK 4 backends: font metrics, Pango styling,
// the cairo cell painter, dialog button conventions and signal bookkeeping.
// Each backend includes this after its own <gtk/gtk.h> and defines
// theme_color_of().
#pragma once

#include "rapport/backend.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rap::gtkcommon
{

struct metrics
{
    float char_width = 8;
    float line_height = 17;
};
inline metrics g_metrics;

inline void init_metrics()
{
    GtkWidget* probe = gtk_label_new("");
    g_object_ref_sink(probe);
    PangoContext* pc = gtk_widget_get_pango_context(probe);
    PangoFontMetrics* m = pango_context_get_metrics(pc, nullptr, nullptr);
    g_metrics.char_width = float(pango_font_metrics_get_approximate_char_width(m)) / PANGO_SCALE;
    g_metrics.line_height = float(pango_font_metrics_get_height(m)) / PANGO_SCALE;
    pango_font_metrics_unref(m);
    g_object_unref(probe);
}

inline int px(extent e)
{
    switch (e.unit)
    {
    case extent::unit::chars: return int(e.value * g_metrics.char_width + 0.5f);
    case extent::unit::lines: return int(e.value * g_metrics.line_height + 0.5f);
    case extent::unit::px: return int(e.value);
    case extent::unit::auto_: break;
    }
    return -1;
}

inline GtkOrientation gtk_orientation(orientation o)
{
    return o == orientation::vertical ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
}

inline uint8_t u8(double v)
{
    return uint8_t(std::clamp(v, 0.0, 1.0) * 255 + 0.5);
}
inline guint16 u16(uint8_t v)
{
    return guint16(v * 257);
}

// Defined by each backend: theme lookup differs between GTK 3 and 4.
color theme_color_of(GtkWidget*, theme_color);

inline color resolve(GtkWidget* w, const paint& p)
{
    if (auto* c = std::get_if<color>(&p)) return *c;
    return theme_color_of(w, std::get<theme_color>(p));
}

// Fallbacks when the theme doesn't name a colour; `dark` is guessed from
// the text colour.
inline color fallback_color(theme_color role, color text, bool dark)
{
    auto alpha = [&](double a) { return color{text.r, text.g, text.b, u8(a)}; };
    switch (role)
    {
    case theme_color::text:
    case theme_color::window_text: return text;
    case theme_color::placeholder_text: return alpha(0.5);
    case theme_color::mid: return alpha(0.35);
    case theme_color::neutral: return alpha(0.6);
    case theme_color::window: return dark ? color{0x24, 0x24, 0x24} : color{0xfa, 0xfa, 0xfa};
    case theme_color::base: return dark ? color{0x1e, 0x1e, 0x1e} : color{0xff, 0xff, 0xff};
    case theme_color::alternate_base: return dark ? color{0x2a, 0x2a, 0x2a} : color{0xf4, 0xf4, 0xf4};
    case theme_color::highlight:
    case theme_color::accent: return color{0x35, 0x84, 0xe4};
    case theme_color::highlight_text: return color{0xff, 0xff, 0xff};
    case theme_color::link: return dark ? color{0x78, 0xae, 0xed} : color{0x1c, 0x71, 0xd8};
    case theme_color::positive: return dark ? color{0x8f, 0xf0, 0xa4} : color{0x26, 0xa2, 0x69};
    case theme_color::negative: return dark ? color{0xf6, 0x61, 0x51} : color{0xc0, 0x1c, 0x28};
    }
    return text;
}

// List autoscroll: px/s for the pointer `offset` px above (negative) or
// below the anchor. A dead zone, then speed growing faster than the
// distance, so a nudge creeps and a long reach races.
inline float auto_scroll_speed(float offset)
{
    const float dead = 8, d = std::abs(offset) - dead;
    if (d <= 0) return 0;
    return (offset < 0 ? -1.f : 1.f) * (d * 6 + d * d * 0.05f);
}

// The autoscroll anchor: a ring with up and down arrows, centred at (x, y).
inline void paint_auto_scroll_anchor(painter& p, float x, float y)
{
    p.circle({x, y}, 11, paint{theme_color::base}, paint{theme_color::mid});
    p.polygon({{x - 4, y - 3}, {x + 4, y - 3}, {x, y - 8}}, paint{theme_color::text}, std::nullopt, 1);
    p.polygon({{x - 4, y + 3}, {x + 4, y + 3}, {x, y + 8}}, paint{theme_color::text}, std::nullopt, 1);
    p.circle({x, y}, 1.5f, paint{theme_color::text}, std::nullopt);
}

// The desktop's icon for a kind of file, as the file manager shows it:
// the name's guessed MIME type (a trailing '/' is a folder) and the icon
// theme's icon for that. Cached per extension; owned by the cache.
inline GIcon* file_type_icon(const std::string& name)
{
    static std::unordered_map<std::string, GIcon*> cache;
    const bool folder = !name.empty() && name.back() == '/';
    const size_t slash = name.find_last_of('/', folder ? name.size() - 2 : std::string::npos);
    const std::string base = name.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot = base.rfind('.');
    // Extensionless names (Makefile, README) guess by the whole name.
    const std::string key = folder ? "/" : dot == std::string::npos || dot == 0 ? base : base.substr(dot);
    GIcon*& icon = cache[key];
    if (!icon)
    {
        gchar* type = folder ? g_strdup("inode/directory") : g_content_type_guess(base.c_str(), nullptr, 0, nullptr);
        icon = g_content_type_get_icon(type);
        g_free(type);
    }
    return icon;
}

inline GIcon* named_g_icon(const std::string& name)
{
    static std::unordered_map<std::string, GIcon*> cache;
    GIcon*& icon = cache[name];
    if (!icon) icon = g_themed_icon_new(name.c_str());
    return icon;
}

inline color mix_colors(color a, color b, float t)
{
    auto f = [&](uint8_t x, uint8_t y) { return uint8_t(x + (y - x) * t + 0.5f); };
    return {f(a.r, b.r), f(a.g, b.g), f(a.b, b.b), 255};
}

// Soft selection colours: a tint of the accent over the base colour, with
// the normal text colour on top.
// A file filter's extension for saving: the first glob's, if it's a plain
// "*.ext" ("*.patch;*.diff" -> "patch"; "*" -> none). GTK's save dialogs
// don't add one the way Windows' does.
inline std::string default_extension(const std::string& pattern)
{
    const std::string first = pattern.substr(0, pattern.find(';'));
    if (!first.starts_with("*.") || first.find_first_of("*?[", 2) != std::string::npos) return {};
    return first.substr(2);
}

// `path` with `.ext` appended if its file name has no extension.
inline std::string with_extension(std::string path, const std::string& ext)
{
    const size_t slash = path.find_last_of('/');
    const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    if (!ext.empty() && name.find('.') == std::string::npos) path += "." + ext;
    return path;
}

inline std::pair<color, color> soft_selection_colors(GtkWidget* w)
{
    color base = theme_color_of(w, theme_color::base), accent = theme_color_of(w, theme_color::accent);
    color text = theme_color_of(w, theme_color::text);
    bool dark = base.r * 30 + base.g * 59 + base.b * 11 < 12800;
    return {mix_colors(base, accent, dark ? 0.38f : 0.24f), text};
}

// The font size for compact lists and text views: 90% of the widget's
// font, rounded to whole pixels so glyphs sit on the pixel grid.
struct compact_font_size
{
    double base_px;
    int px;
    float scale() const { return base_px > 0 ? float(px / base_px) : 1.f; }
};

inline compact_font_size compact_font(GtkWidget* w)
{
    const PangoFontDescription* fd =
        pango_context_get_font_description(gtk_widget_get_pango_context(w));
    double size = pango_font_description_get_size(fd) / double(PANGO_SCALE);
    if (!pango_font_description_get_size_is_absolute(fd))
    {
        int dpi = 0;
        g_object_get(gtk_settings_get_default(), "gtk-xft-dpi", &dpi, nullptr);
        size = size * (dpi > 0 ? dpi / 1024.0 : 96.0) / 72.0;
    }
    if (size <= 0) size = 14;
    return {size, std::max(8, int(std::lround(size * 0.9)))};
}

inline bool is_dark_text(const GdkRGBA& fg)
{
    return 0.299 * fg.red + 0.587 * fg.green + 0.114 * fg.blue > 0.5;
}

inline color to_color(const GdkRGBA& c)
{
    return color{u8(c.red), u8(c.green), u8(c.blue), u8(c.alpha)};
}

// Null when the style is plain.
inline PangoAttrList* pango_attrs(GtkWidget* w, const text_style& s)
{
    if (!s.fg && !s.bg && !s.bold && !s.italic && !s.monospace && s.size <= 0) return nullptr;
    PangoAttrList* list = pango_attr_list_new();
    if (s.size > 0) pango_attr_list_insert(list, pango_attr_size_new(int(s.size * PANGO_SCALE)));
    if (s.bold) pango_attr_list_insert(list, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    if (s.italic) pango_attr_list_insert(list, pango_attr_style_new(PANGO_STYLE_ITALIC));
    if (s.monospace) pango_attr_list_insert(list, pango_attr_family_new("monospace"));
    if (s.fg)
    {
        color c = resolve(w, *s.fg);
        pango_attr_list_insert(list, pango_attr_foreground_new(u16(c.r), u16(c.g), u16(c.b)));
    }
    if (s.bg)
    {
        color c = resolve(w, *s.bg);
        pango_attr_list_insert(list, pango_attr_background_new(u16(c.r), u16(c.g), u16(c.b)));
    }
    return list;
}

inline const char* stock_label(dialog_role r)
{
    switch (r)
    {
    case dialog_role::ok: return "_OK";
    case dialog_role::cancel: return "_Cancel";
    case dialog_role::apply: return "_Apply";
    case dialog_role::close: return "_Close";
    case dialog_role::yes: return "_Yes";
    case dialog_role::no: return "_No";
    case dialog_role::help: return "_Help";
    case dialog_role::reset: return "_Reset";
    case dialog_role::none: break;
    }
    return "";
}

// GNOME order: help at the start, affirmative action last.
inline int gnome_order(dialog_role r)
{
    switch (r)
    {
    case dialog_role::help: return 0;
    case dialog_role::reset: return 1;
    case dialog_role::none: return 2;
    case dialog_role::cancel:
    case dialog_role::no:
    case dialog_role::close: return 3;
    case dialog_role::apply: return 4;
    case dialog_role::ok:
    case dialog_role::yes: return 5;
    }
    return 2;
}

// "Ctrl+Shift+C" -> "<Control><Shift>c"
inline std::string gtk_accel(std::string s)
{
    std::string out;
    for (auto [from, to] : {std::pair{"Ctrl+", "<Control>"},
                            {"Shift+", "<Shift>"},
                            {"Alt+", "<Alt>"},
                            {"Meta+", "<Super>"}})
    {
        for (size_t at; (at = s.find(from)) != std::string::npos;)
        {
            out += to;
            s.erase(at, std::strlen(from));
        }
    }
    if (s.size() == 1) s[0] = char(g_ascii_tolower(s[0]));
    return out + s;
}

// An Image as a cairo image surface (ARGB32 premultiplied, i.e. rapport's
// Bgra8Premultiplied byte order on little-endian machines).
class cairo_image : public image
{
public:
    cairo_image(int w, int h, pixel_format format, const uint8_t* pixels, size_t stride)
        : surface_(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h))
        , format_(format)
    {
        update(pixels, stride);
    }
    ~cairo_image() override { cairo_surface_destroy(surface_); }

    int width() const override { return cairo_image_surface_get_width(surface_); }
    int height() const override { return cairo_image_surface_get_height(surface_); }

    void update(const uint8_t* pixels, size_t stride) override
    {
        if (!pixels) return;
        cairo_surface_flush(surface_);
        uint8_t* dst = cairo_image_surface_get_data(surface_);
        const int dst_stride = cairo_image_surface_get_stride(surface_);
        const int w = width(), h = height();
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* src = pixels + size_t(y) * stride;
            uint8_t* row = dst + size_t(y) * dst_stride;
            if (format_ == pixel_format::bgra8_premultiplied)
            {
                std::memcpy(row, src, size_t(w) * 4);
                continue;
            }
            for (int x = 0; x < w; ++x)  // straight RGBA -> premultiplied BGRA
            {
                const unsigned a = src[x * 4 + 3];
                row[x * 4 + 0] = uint8_t((src[x * 4 + 2] * a + 127) / 255);
                row[x * 4 + 1] = uint8_t((src[x * 4 + 1] * a + 127) / 255);
                row[x * 4 + 2] = uint8_t((src[x * 4 + 0] * a + 127) / 255);
                row[x * 4 + 3] = uint8_t(a);
            }
        }
        cairo_surface_mark_dirty(surface_);
    }

    cairo_surface_t* surface() const { return surface_; }

private:
    cairo_surface_t* surface_;
    pixel_format format_;
};

class cairo_painter : public painter
{
public:
    cairo_painter(cairo_t* cr, GtkWidget* w, std::optional<paint> default_text = {})
        : cr_(cr)
        , w_(w)
        , default_text_(default_text ? *default_text : paint{theme_color::text})
    {
    }

    color resolve(const paint& p) const override
    {
        if (auto* role = std::get_if<theme_color>(&p))
            for (auto& [r, c] : overrides_)
                if (r == *role) return c;
        return gtkcommon::resolve(w_, p);
    }

    // Replaces a theme colour for this painter (a list's soft selection).
    void override_color(theme_color role, color c) { overrides_.push_back({role, c}); }

    void line(point_f a, point_f b, const paint& p, float width, line_cap cap) override
    {
        source(p);
        cairo_set_line_width(cr_, width);
        cairo_set_line_cap(cr_, cap == line_cap::round ? CAIRO_LINE_CAP_ROUND : CAIRO_LINE_CAP_BUTT);
        cairo_move_to(cr_, a.x, a.y);
        cairo_line_to(cr_, b.x, b.y);
        cairo_stroke(cr_);
        cairo_set_line_cap(cr_, CAIRO_LINE_CAP_BUTT);
    }

    void curve(point_f a, point_f c1, point_f c2, point_f b, const paint& p, float width) override
    {
        source(p);
        cairo_set_line_width(cr_, width);
        cairo_move_to(cr_, a.x, a.y);
        cairo_curve_to(cr_, c1.x, c1.y, c2.x, c2.y, b.x, b.y);
        cairo_stroke(cr_);
    }

    void circle(point_f c, float r, std::optional<paint> fill, std::optional<paint> stroke) override
    {
        cairo_new_path(cr_);
        cairo_arc(cr_, c.x, c.y, r, 0, 2 * G_PI);
        finish(fill, stroke);
    }

    void rect(rect_f r, float radius, std::optional<paint> fill,
              std::optional<paint> stroke) override
    {
        cairo_new_path(cr_);
        if (radius <= 0)
        {
            cairo_rectangle(cr_, r.x, r.y, r.w, r.h);
        }
        else
        {
            double x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
            cairo_arc(cr_, x1 - radius, y0 + radius, radius, -G_PI / 2, 0);
            cairo_arc(cr_, x1 - radius, y1 - radius, radius, 0, G_PI / 2);
            cairo_arc(cr_, x0 + radius, y1 - radius, radius, G_PI / 2, G_PI);
            cairo_arc(cr_, x0 + radius, y0 + radius, radius, G_PI, 3 * G_PI / 2);
            cairo_close_path(cr_);
        }
        finish(fill, stroke);
    }

    void polygon(const std::vector<point_f>& pts, std::optional<paint> fill,
                 std::optional<paint> stroke, float width) override
    {
        if (pts.empty()) return;
        cairo_new_path(cr_);
        cairo_move_to(cr_, pts[0].x, pts[0].y);
        for (size_t i = 1; i < pts.size(); ++i) cairo_line_to(cr_, pts[i].x, pts[i].y);
        cairo_close_path(cr_);
        finish(fill, stroke, width);
    }

    void arc(point_f c, float r, float start, float end, const paint& p, float width,
             line_cap cap) override
    {
        source(p);
        cairo_set_line_width(cr_, width);
        cairo_set_line_cap(cr_, cap == line_cap::round ? CAIRO_LINE_CAP_ROUND : CAIRO_LINE_CAP_BUTT);
        cairo_new_path(cr_);
        cairo_arc(cr_, c.x, c.y, r, start, end);
        cairo_stroke(cr_);
        cairo_set_line_cap(cr_, CAIRO_LINE_CAP_BUTT);
    }

    void image(const rap::image& img, rect_f src, rect_f dst, filter filter, float alpha) override
    {
        if (src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0) return;
        cairo_save(cr_);
        cairo_rectangle(cr_, dst.x, dst.y, dst.w, dst.h);
        cairo_clip(cr_);
        cairo_translate(cr_, dst.x, dst.y);
        cairo_scale(cr_, dst.w / src.w, dst.h / src.h);
        cairo_set_source_surface(cr_, static_cast<const cairo_image&>(img).surface(), -src.x,
                                 -src.y);
        cairo_pattern_t* pattern = cairo_get_source(cr_);
        cairo_pattern_set_filter(pattern, filter == filter::nearest ? CAIRO_FILTER_NEAREST
                                                                    : CAIRO_FILTER_BILINEAR);
        cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);  // no dark fringe at the edges
        if (alpha >= 1)
            cairo_paint(cr_);
        else
            cairo_paint_with_alpha(cr_, alpha);
        cairo_restore(cr_);
    }

    float device_scale() const override { return float(gtk_widget_get_scale_factor(w_)); }

    void push_clip(rect_f r) override
    {
        cairo_save(cr_);
        cairo_rectangle(cr_, r.x, r.y, r.w, r.h);
        cairo_clip(cr_);
    }
    void pop_clip() override { cairo_restore(cr_); }

    void text(point_f at, const std::string& s, const text_style& style) override
    {
        PangoLayout* layout = make_layout(s, style);
        if (style.bg)
        {
            int w, h;
            pango_layout_get_pixel_size(layout, &w, &h);
            source(*style.bg);
            cairo_rectangle(cr_, at.x, at.y, w, h);
            cairo_fill(cr_);
        }
        source(style.fg ? *style.fg : default_text_);
        cairo_move_to(cr_, at.x, at.y);
        pango_cairo_show_layout(cr_, layout);
        g_object_unref(layout);
    }

    size_f measure(const std::string& s, const text_style& style) const override
    {
        PangoLayout* layout = make_layout(s, style);
        int w, h;
        pango_layout_get_pixel_size(layout, &w, &h);
        g_object_unref(layout);
        return {float(w), float(h)};
    }

private:
    PangoLayout* make_layout(const std::string& s, text_style style) const
    {
        PangoLayout* layout = gtk_widget_create_pango_layout(w_, s.c_str());
        style.fg.reset();  // colours are drawn with cairo
        style.bg.reset();
        if (PangoAttrList* attrs = pango_attrs(w_, style))
        {
            pango_layout_set_attributes(layout, attrs);
            pango_attr_list_unref(attrs);
        }
        return layout;
    }

    void source(const paint& p)
    {
        color c = resolve(p);
        cairo_set_source_rgba(cr_, c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0);
    }

    void finish(const std::optional<paint>& fill, const std::optional<paint>& stroke,
                float width = 1)
    {
        if (fill)
        {
            source(*fill);
            if (stroke)
                cairo_fill_preserve(cr_);
            else
                cairo_fill(cr_);
        }
        if (stroke)
        {
            source(*stroke);
            cairo_set_line_width(cr_, width);
            cairo_stroke(cr_);
        }
    }

    std::vector<std::pair<theme_color, color>> overrides_;
    cairo_t* cr_;
    GtkWidget* w_;
    paint default_text_;
};

// Tracks signal connections so a window can cut them all before it
// destroys its GTK widgets; app callbacks must not fire during teardown.
class part
{
public:
    virtual ~part() = default;

    void disconnect()
    {
        for (auto [obj, data] : conns_) g_signal_handlers_disconnect_by_data(obj, data);
        conns_.clear();
        on_disconnect();
    }

protected:
    template <class F>
    void connect(gpointer obj, const char* signal, F* callback, gpointer data)
    {
        g_signal_connect(obj, signal, G_CALLBACK(callback), data);
        conns_.push_back({obj, data});
    }
    // After the default handler (drawing over what the widget drew).
    template <class F>
    void connect_after(gpointer obj, const char* signal, F* callback, gpointer data)
    {
        g_signal_connect_after(obj, signal, G_CALLBACK(callback), data);
        conns_.push_back({obj, data});
    }

    virtual void on_disconnect() {}

private:
    std::vector<std::pair<gpointer, gpointer>> conns_;
};

template <class Base>
class widget_base : public Base, public part
{
public:
    explicit widget_base(GtkWidget* w, GtkWidget* outer = nullptr)
        : w_(w)
        , outer_(outer ? outer : w)
    {
    }

    void set_enabled(bool e) override { gtk_widget_set_sensitive(outer_, e); }
    void set_visible(bool v) override { gtk_widget_set_visible(outer_, v); }
    void set_tooltip(const std::string& t) override { gtk_widget_set_tooltip_text(w_, t.c_str()); }
    void focus() override { gtk_widget_grab_focus(w_); }
    bool has_focus() const override
    {
#if GTK_MAJOR_VERSION >= 4
        GtkRoot* root = gtk_widget_get_root(outer_);
        GtkWidget* f = root ? gtk_root_get_focus(root) : nullptr;
#else
        GtkWidget* top = gtk_widget_get_toplevel(outer_);
        GtkWidget* f = GTK_IS_WINDOW(top) ? gtk_window_get_focus(GTK_WINDOW(top)) : nullptr;
#endif
        return f && (f == outer_ || gtk_widget_is_ancestor(f, outer_));
    }

    GtkWidget* outer() const { return outer_; }

protected:
    GtkWidget* w_;
    GtkWidget* outer_;
};

// Text tags cached per style; diffs reuse a handful of styles many times.
// Shared by both text_view implementations (GtkTextBuffer is the same API).
class tag_cache
{
public:
    GtkTextTag* get(GtkTextBuffer* buffer, GtkWidget* w, const text_style& s)
    {
        auto rgba = [&](const std::optional<paint>& p) -> std::optional<GdkRGBA>
        {
            if (!p) return std::nullopt;
            color c = resolve(w, *p);
            return GdkRGBA{c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
        };
        auto fg = rgba(s.fg), bg = rgba(s.bg);

        std::string key =
            std::to_string(s.bold) + std::to_string(s.italic) + std::to_string(s.monospace);
        for (auto& c : {fg, bg})
        {
            key += '|';
            if (c)
            {
                char* str = gdk_rgba_to_string(&*c);
                key += str;
                g_free(str);
            }
        }
        for (auto& [k, t] : tags_)
            if (k == key) return t;

        GtkTextTag* t = gtk_text_buffer_create_tag(buffer, nullptr, nullptr);
        if (s.bold) g_object_set(t, "weight", PANGO_WEIGHT_BOLD, nullptr);
        if (s.italic) g_object_set(t, "style", PANGO_STYLE_ITALIC, nullptr);
        if (s.monospace) g_object_set(t, "family", "monospace", nullptr);
        if (fg) g_object_set(t, "foreground-rgba", &*fg, nullptr);
        if (bg) g_object_set(t, "background-rgba", &*bg, nullptr);
        tags_.emplace_back(std::move(key), t);
        return t;
    }

    // Applies byte-offset spans to text just loaded into the buffer.
    void apply(GtkTextBuffer* buffer, GtkWidget* w, const std::string& text,
               std::vector<text_view::span> spans)
    {
        std::ranges::sort(spans, {}, &text_view::span::begin);
        size_t byte = 0;
        long chars = 0;
        auto to_chars = [&](size_t target)
        {
            target = std::min(target, text.size());
            chars += g_utf8_pointer_to_offset(text.c_str() + byte, text.c_str() + target);
            byte = target;
            return int(chars);
        };
        for (auto& s : spans)
        {
            if (s.end <= s.begin || s.begin < byte) continue;
            GtkTextIter a, b;
            gtk_text_buffer_get_iter_at_offset(buffer, &a, to_chars(s.begin));
            gtk_text_buffer_get_iter_at_offset(buffer, &b, to_chars(s.end));
            gtk_text_buffer_apply_tag(buffer, get(buffer, w, s.style), &a, &b);
        }
    }

private:
    std::vector<std::pair<std::string, GtkTextTag*>> tags_;
};

// Running under WSL, where WSLg shows the windows on the Windows desktop:
// the monitor GDK reports is the whole desktop (it doesn't know the Windows
// taskbar), and its Wayland can't place windows.
inline bool under_wsl()
{
    static const bool wsl = []
    {
        if (std::getenv("WSL_DISTRO_NAME")) return true;
        char buf[256] = {};
        if (FILE* f = std::fopen("/proc/sys/kernel/osrelease", "r"))
        {
            const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            std::fclose(f);
        }
        return std::strstr(buf, "microsoft") || std::strstr(buf, "Microsoft");
    }();
    return wsl;
}

// What WSLg leaves out of the work area: Windows' taskbar along the bottom,
// at Windows 11's height, which covers Windows 10's.
inline rect_f without_windows_taskbar(rect_f area)
{
    if (under_wsl()) area.h = std::max(0.f, area.h - 48);
    return area;
}

// The rapport key for a GDK keyval (the same in GTK 3 and 4).
inline key key_for(guint keyval)
{
    switch (keyval)
    {
    case GDK_KEY_Escape: return key::escape;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter: return key::enter;
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: return key::tab;
    case GDK_KEY_BackSpace: return key::backspace;
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete: return key::delete_;
    case GDK_KEY_Insert:
    case GDK_KEY_KP_Insert: return key::insert;
    case GDK_KEY_space: return key::space;
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left: return key::left;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right: return key::right;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up: return key::up;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down: return key::down;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home: return key::home;
    case GDK_KEY_End:
    case GDK_KEY_KP_End: return key::end;
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up: return key::page_up;
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down: return key::page_down;
    default: break;
    }
    if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12)
        return key(int(key::f1) + int(keyval - GDK_KEY_F1));
    return gdk_keyval_to_unicode(keyval) ? key::character : key::other;
}

}  // namespace rap::gtkcommon
