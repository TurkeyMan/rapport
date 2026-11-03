// Code shared by the GTK 3 and GTK 4 backends: font metrics, Pango styling,
// the cairo cell painter, dialog button conventions and signal bookkeeping.
// Each backend includes this after its own <gtk/gtk.h>.
#pragma once

#include "rapport/backend.h"
#include "rapport/i18n.h"
#include "theme_palette.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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

// A slider's marks (values) on its scale, which runs along the track.
inline void set_scale_marks(GtkScale* scale, const slider_map& map, const std::vector<std::pair<double, std::string>>& marks)
{
    gtk_scale_clear_marks(scale);
    for (const auto& [at, label] : marks)
        gtk_scale_add_mark(scale, map.track(at), GTK_POS_BOTTOM, label.empty() ? nullptr : label.c_str());
}
// spec::slider::snap: the scale dragged (to `t`, along the track) within a
// few pixels of a mark lands on it. `width`: the scale's, in pixels.
// Returns whether it did (change-value is then handled).
inline bool snap_scale(GtkRange* r, int width, const slider_map& map,
                       const std::vector<std::pair<double, std::string>>& marks, GtkScrollType how, double t)
{
    GtkAdjustment* a = gtk_range_get_adjustment(r);
    const double span = gtk_adjustment_get_upper(a) - gtk_adjustment_get_lower(a);
    if (how != GTK_SCROLL_JUMP || width <= 0 || span <= 0) return false;
    const double reach = 5.0 * span / width;
    const std::pair<double, std::string>* nearest = nullptr;
    for (const auto& m : marks)
        if (!nearest || std::abs(map.track(m.first) - t) < std::abs(map.track(nearest->first) - t)) nearest = &m;
    if (!nearest || std::abs(map.track(nearest->first) - t) > reach) return false;
    gtk_range_set_value(r, map.track(nearest->first));
    return true;
}

inline uint8_t u8(double v)
{
    return uint8_t(std::clamp(v, 0.0, 1.0) * 255 + 0.5);
}
inline guint16 u16(uint8_t v)
{
    return guint16(v * 257);
}

// A theme role for `w`: what the desktop says, else derived (below).
inline color theme_color_of(GtkWidget*, theme_color);

inline color resolve(GtkWidget* w, const paint& p)
{
    return palette::resolve(p, [w](theme_color r) { return theme_color_of(w, r); });
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
    return {mix_colors(base, accent, palette::is_dark(base) ? 0.38f : 0.24f), text};
}

// The row under the pointer in a soft-selection list.
inline color soft_hover_color(GtkWidget* w)
{
    color base = theme_color_of(w, theme_color::base),
          accent = theme_color_of(w, theme_color::accent);
    return mix_colors(base, accent, palette::is_dark(base) ? 0.16f : 0.09f);
}

// Device pixels per logical pixel. GTK 4's widget scale factor rounds a
// fractional scale (150%) up to 2; its surface scale is the true one, and
// drawing areas are replayed at it, so callers can place pixels 1:1.
#if !GTK_CHECK_VERSION(4, 0, 0)
// GTK 3 knows only whole scales and draws at them; the compositor resizes
// the result to a fractional one (150%). The gtk3 backend reads that scale
// (Wayland's fractional-scale protocol) into the toplevel's GdkWindow, in
// 120ths as the protocol sends it; 0 where it isn't known, which includes
// toplevels with no canvas (it's watched from a canvas's realize).
inline constexpr char k_fractional_scale_key[] = "rapport-fractional-scale";
inline float fractional_scale_of(GtkWidget* w)
{
    GtkWidget* top = gtk_widget_get_toplevel(w);
    GdkWindow* win = top ? gtk_widget_get_window(top) : nullptr;
    if (!win) return 0;
    return float(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(win), k_fractional_scale_key))) /
           120.f;
}
#endif

inline float device_scale_of(GtkWidget* w)
{
#if GTK_CHECK_VERSION(4, 12, 0)
    if (GtkNative* native = gtk_widget_get_native(w))
        if (GdkSurface* surface = gtk_native_get_surface(native))
            return float(gdk_surface_get_scale(surface));
#elif !GTK_CHECK_VERSION(4, 0, 0)
    if (const float f = fractional_scale_of(w); f > 0) return f;
#endif
    return float(gtk_widget_get_scale_factor(w));
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

// A dialog button's label, translated ("_OK"; '_' marks the access key, as GTK's).
inline std::string stock_label(dialog_role r)
{
    return rap::i18n::stock_label(r);
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
        // Through the overrides, so on paper a scheme colour takes its light shade.
        return palette::resolve(p,
                                [this](theme_color role)
                                {
                                    for (auto& [r, c] : overrides_)
                                        if (r == role) return c;
                                    return theme_color_of(w_, role);
                                });
    }

    // Replaces a theme colour for this painter (a list's soft selection).
    void override_color(theme_color role, color c) { overrides_.push_back({role, c}); }

    // On paper (print_job): no widget, so text is in `font` laid out at 72
    // dpi (the cairo unit is the point), theme colours are print_color()'s,
    // and the device has `scale` dots per point.
    void for_print(const PangoFontDescription* font, float scale)
    {
        print_font_ = font;
        print_scale_ = scale;
        for (int r = 0; r < theme_color_count; ++r)
            override_color(theme_color(r), print_color(theme_color(r)));
    }

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

    float device_scale() const override
    {
        return print_font_ ? print_scale_ : device_scale_of(w_);
    }
#if !GTK_CHECK_VERSION(4, 0, 0)
    // GTK 3 draws at the whole scale whatever the display's fractional one.
    float pixel_scale() const override
    {
        return print_font_ ? print_scale_ : float(gtk_widget_get_scale_factor(w_));
    }
#endif

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
        PangoLayout* layout = nullptr;
        if (print_font_)
        {
            layout = pango_cairo_create_layout(cr_);
            pango_cairo_context_set_resolution(pango_layout_get_context(layout), 72);
            pango_layout_context_changed(layout);
            pango_layout_set_font_description(layout, print_font_);
            pango_layout_set_text(layout, s.c_str(), -1);
        }
        else
            layout = gtk_widget_create_pango_layout(w_, s.c_str());
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
    const PangoFontDescription* print_font_ = nullptr;
    float print_scale_ = 1;
};

// -- printing -----------------------------------------------------------------

// The desktop's UI font, for text on paper.
inline PangoFontDescription* print_font()
{
    gchar* name = nullptr;
    g_object_get(gtk_settings_get_default(), "gtk-font-name", &name, nullptr);
    PangoFontDescription* font = pango_font_description_from_string(name && *name ? name : "Sans 10");
    g_free(name);
    return font;
}

// "out.png", page 2: "out-2.png".
inline std::string numbered(const std::string& path, int page)
{
    if (page == 0) return path;
    const size_t dot = path.find_last_of('.');
    const std::string n = "-" + std::to_string(page + 1);
    return dot == std::string::npos ? path + n : path.substr(0, dot) + n + path.substr(dot);
}

// RAPPORT_PRINT_TO=out.png: each page as A4 at 144 dpi, no dialog, as on
// Windows: for tests.
inline bool print_to_png(const std::string& path, print_job& job, const PangoFontDescription* font)
{
    const bool landscape = job.orientation == page_orientation::landscape;
    const size_f paper = landscape ? size_f{842, 595} : size_f{595, 842};
    const float dpi = 144;
    int pages = job.pages;
    if (job.on_begin)
        if (const int n = job.on_begin(paper); n >= 0) pages = n;
    bool ok = true;
    for (int page = 0; page < pages && ok; ++page)
    {
        cairo_surface_t* surface = cairo_image_surface_create(
            CAIRO_FORMAT_RGB24, int(std::lround(paper.w * dpi / 72)), int(std::lround(paper.h * dpi / 72)));
        cairo_t* cr = cairo_create(surface);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_scale(cr, dpi / 72, dpi / 72);
        {
            cairo_painter p(cr, nullptr);
            p.for_print(font, dpi / 72);
            job.draw_page(p, page, paper);
        }
        cairo_destroy(cr);
        ok = cairo_surface_write_to_png(surface, numbered(path, page).c_str()) == CAIRO_STATUS_SUCCESS;
        cairo_surface_destroy(surface);
    }
    return ok;
}

// GTK's print operation, the same in GTK 3 and 4: its dialog (or, with
// RAPPORT_PRINT_TO=out.pdf, an export without one), then the pages GTK asks
// for, in points.
inline void run_print_operation(GtkWindow* parent, print_job job)
{
    struct state
    {
        print_job job;
        PangoFontDescription* font = print_font();
        size_f printable;
        bool returned = false, finished = false;
        GtkPrintOperationResult result = GTK_PRINT_OPERATION_RESULT_ERROR;
        std::string error;
    };
    auto* st = new state{std::move(job)};
    auto finish = [](state* s, bool ok, std::string error)
    {
        if (s->job.done) s->job.done(ok, std::move(error));
        pango_font_description_free(s->font);
        delete s;
    };
    if (!st->job.draw_page) return finish(st, false, i18n::tr("rapport", "nothing to print"));
    const char* env = std::getenv("RAPPORT_PRINT_TO");
    const std::string to = env ? env : "";
    auto ends = [&](const char* ext)
    {
        return to.size() > 4 && g_ascii_strcasecmp(to.c_str() + to.size() - 4, ext) == 0;
    };
    if (ends(".png"))
    {
        const bool ok = print_to_png(to, st->job, st->font);
        return finish(st, ok, ok ? "" : i18n::format(i18n::tr("rapport", "couldn't write {0}"), to));
    }

    GtkPrintOperation* op = gtk_print_operation_new();
    gtk_print_operation_set_job_name(
        op, st->job.title.empty() ? i18n::tr("rapport", "Document") : st->job.title.c_str());
    gtk_print_operation_set_n_pages(op, std::max(st->job.pages, 1));
    gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
    gtk_print_operation_set_embed_page_setup(op, true);
    if (st->job.orientation != page_orientation::automatic)
    {
        GtkPageSetup* setup = gtk_page_setup_new();
        gtk_page_setup_set_orientation(setup, st->job.orientation == page_orientation::landscape
                                                  ? GTK_PAGE_ORIENTATION_LANDSCAPE
                                                  : GTK_PAGE_ORIENTATION_PORTRAIT);
        gtk_print_operation_set_default_page_setup(op, setup);
        g_object_unref(setup);
    }
    g_signal_connect(op, "begin-print",
                     G_CALLBACK((+[](GtkPrintOperation* op, GtkPrintContext* ctx, gpointer p)
                                {
                                    auto* s = static_cast<state*>(p);
                                    GtkPageSetup* setup = gtk_print_context_get_page_setup(ctx);
                                    s->printable = {float(gtk_page_setup_get_page_width(setup, GTK_UNIT_POINTS)),
                                                    float(gtk_page_setup_get_page_height(setup, GTK_UNIT_POINTS))};
                                    if (s->job.on_begin)
                                        if (const int n = s->job.on_begin(s->printable); n >= 0)
                                            gtk_print_operation_set_n_pages(op, std::max(n, 1));
                                })),
                     st);
    g_signal_connect(op, "draw-page",
                     G_CALLBACK((+[](GtkPrintOperation*, GtkPrintContext* ctx, gint page, gpointer p)
                                {
                                    auto* s = static_cast<state*>(p);
                                    cairo_painter painter(gtk_print_context_get_cairo_context(ctx), nullptr);
                                    painter.for_print(s->font, float(gtk_print_context_get_dpi_x(ctx) / 72));
                                    s->job.draw_page(painter, page, s->printable);
                                })),
                     st);
    // "done" can come during the run or after it: whichever is second ends.
    g_signal_connect(op, "done",
                     G_CALLBACK((+[](GtkPrintOperation* op, GtkPrintOperationResult result, gpointer p)
                                {
                                    auto* s = static_cast<state*>(p);
                                    s->result = result;
                                    s->finished = true;
                                    if (result == GTK_PRINT_OPERATION_RESULT_ERROR)
                                    {
                                        GError* e = nullptr;
                                        gtk_print_operation_get_error(op, &e);
                                        s->error = e ? e->message : i18n::tr("rapport", "printing failed");
                                        g_clear_error(&e);
                                    }
                                })),
                     st);
    GtkPrintOperationAction action = GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG;
    if (ends(".pdf"))
    {
        gtk_print_operation_set_export_filename(op, to.c_str());
        action = GTK_PRINT_OPERATION_ACTION_EXPORT;
    }
    gtk_print_operation_set_allow_async(op, true);
    GError* error = nullptr;
    const GtkPrintOperationResult r = gtk_print_operation_run(op, action, parent, &error);
    if (error)
    {
        st->result = GTK_PRINT_OPERATION_RESULT_ERROR;
        st->error = error->message;
        st->finished = true;
        g_clear_error(&error);
    }
    if (r != GTK_PRINT_OPERATION_RESULT_IN_PROGRESS && !st->finished)
    {
        st->result = r;
        st->finished = true;
    }
    auto conclude = [](state* s, GtkPrintOperation* op, auto finish)
    {
        g_signal_handlers_disconnect_by_data(op, s);
        const bool ok = s->result == GTK_PRINT_OPERATION_RESULT_APPLY;
        finish(s, ok, ok || s->result == GTK_PRINT_OPERATION_RESULT_CANCEL ? std::string() : s->error);
        g_object_unref(op);
    };
    if (st->finished) return conclude(st, op, finish);
    // In progress: the dialog's answer, then the pages, arrive later.
    g_signal_connect(op, "done",
                     G_CALLBACK((+[](GtkPrintOperation* op, GtkPrintOperationResult, gpointer p)
                                {
                                    auto* s = static_cast<state*>(p);
                                    g_signal_handlers_disconnect_by_data(op, s);
                                    const bool ok = s->result == GTK_PRINT_OPERATION_RESULT_APPLY;
                                    if (s->job.done)
                                        s->job.done(ok, ok || s->result == GTK_PRINT_OPERATION_RESULT_CANCEL
                                                            ? std::string()
                                                            : s->error);
                                    pango_font_description_free(s->font);
                                    delete s;
                                    g_object_unref(op);
                                })),
                     st);
}

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
    void set_visible(bool v) override
    {
        show(outer_, v);
        // A form field's row label goes with it.
        if (auto* label = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(outer_), "rapport-form-label")))
            show(label, v);
    }
    static void show(GtkWidget* w, bool v)
    {
#if GTK_MAJOR_VERSION >= 4
        gtk_widget_set_visible(w, v);
#else
        // Hidden, it stays hidden when its window shows everything; shown,
        // its children show too (they were never shown with the window).
        gtk_widget_set_no_show_all(w, !v);
        if (v)
            gtk_widget_show_all(w);
        else
            gtk_widget_hide(w);
#endif
    }
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
        // Keyed by the style's paints, roles by name: a theme change
        // recolours a role's tags (recolour) rather than leaving them.
        auto part = [](const std::optional<paint>& p)
        {
            if (!p) return std::string();
            if (auto* r = std::get_if<theme_color>(&*p)) return "r" + std::to_string(int(*r));
            auto rgba = [](color c)
            {
                return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) +
                       "," + std::to_string(c.a);
            };
            if (auto* s = std::get_if<scheme_color>(&*p))
                return "s" + rgba(s->light) + "/" + rgba(s->dark);
            return rgba(std::get<color>(*p));
        };
        std::string key = std::to_string(s.bold) + std::to_string(s.italic) +
                          std::to_string(s.monospace) + '|' + part(s.fg) + '|' + part(s.bg);
        for (auto& e : tags_)
            if (e.key == key) return e.tag;

        GtkTextTag* t = gtk_text_buffer_create_tag(buffer, nullptr, nullptr);
        if (s.bold) g_object_set(t, "weight", PANGO_WEIGHT_BOLD, nullptr);
        if (s.italic) g_object_set(t, "style", PANGO_STYLE_ITALIC, nullptr);
        if (s.monospace) g_object_set(t, "family", "monospace", nullptr);
        tags_.push_back({std::move(key), t, s.fg, s.bg});
        colour(w, tags_.back());
        return t;
    }

    // The theme changed: tags of theme roles take the roles' new colours,
    // and scheme colours the shade for the new scheme.
    void recolour(GtkWidget* w)
    {
        for (entry& e : tags_) colour(w, e);
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
    struct entry
    {
        std::string key;
        GtkTextTag* tag;
        std::optional<paint> fg, bg;
    };
    static void colour(GtkWidget* w, const entry& e)
    {
        auto rgba = [&](const paint& p)
        {
            const color c = resolve(w, p);
            return GdkRGBA{c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
        };
        if (e.fg)
        {
            const GdkRGBA c = rgba(*e.fg);
            g_object_set(e.tag, "foreground-rgba", &c, nullptr);
        }
        if (e.bg)
        {
            const GdkRGBA c = rgba(*e.bg);
            g_object_set(e.tag, "background-rgba", &c, nullptr);
        }
    }
    std::vector<entry> tags_;
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

// -- theme roles --------------------------------------------------------------

// The roles resolved so far (named colours are the theme's, the same for
// every widget), the settings portal's accent, and who wants to hear when
// the theme changes. Cleared and told by theme_changed().
struct theme_state
{
    std::array<std::optional<color>, theme_color_count> cache;
    std::optional<color> portal_accent;
    std::unordered_map<const void*, std::function<void()>> listeners;
};
inline theme_state& theme_states()
{
    // Never destroyed: widgets kept in statics (an app's windows) forget
    // their listeners during static teardown, after it would have gone.
    static theme_state* t = new theme_state;
    return *t;
}

// `f` runs after every theme change, until forget_theme_listener(owner).
inline void on_theme_changed(const void* owner, std::function<void()> f)
{
    theme_states().listeners[owner] = std::move(f);
}
inline void forget_theme_listener(const void* owner)
{
    theme_states().listeners.erase(owner);
}

// -- text view: the buffer side, the same in both GTKs -----------------------

// Text and spans in the buffer, UTF-8 byte offsets over GTK's characters.
// Each backend makes the scrolled window around it (and its compact font),
// then calls setup.
class text_view_common : public widget_base<rap::text_view>
{
public:
    ~text_view_common() override { forget_theme_listener(this); }

    std::string text() const override
    {
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buffer_, &a, &b);
        char* s = gtk_text_buffer_get_text(buffer_, &a, &b, false);
        std::string out = s;
        g_free(s);
        return out;
    }

    void set_text(const std::string& t, std::vector<span> spans = {}) override
    {
        gtk_text_buffer_set_text(buffer_, t.c_str(), int(t.size()));
        tags_.apply(buffer_, w_, t, std::move(spans));
    }

    void append(const std::string& t, const text_style& style) override
    {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer_, &end);
        gtk_text_buffer_insert_with_tags(buffer_, &end, t.c_str(), int(t.size()),
                                         tags_.get(buffer_, w_, style), nullptr);
    }

    void set_read_only(bool ro) override
    {
        gtk_text_view_set_editable(GTK_TEXT_VIEW(w_), !ro);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(w_), !ro);
    }

    void on_changed(std::function<void()> f) override { changed_ = std::move(f); }

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

protected:
    explicit text_view_common(GtkWidget* scrolled)
        : widget_base(gtk_text_view_new(), scrolled)
    {
    }
    void setup(const spec::text_view& s)
    {
        auto* tv = GTK_TEXT_VIEW(w_);
        gtk_text_view_set_monospace(tv, s.monospace);
        gtk_text_view_set_wrap_mode(tv, s.wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
        gtk_text_view_set_left_margin(tv, 4);
        gtk_text_view_set_right_margin(tv, 4);
        set_read_only(s.read_only);
        buffer_ = gtk_text_view_get_buffer(tv);
        // Spans in theme roles take the new colours.
        on_theme_changed(this, [this] { tags_.recolour(w_); });
        set_text(s.text);
        connect(
            buffer_, "changed",
            +[](GtkTextBuffer*, gpointer p)
            {
                auto* self = static_cast<text_view_common*>(p);
                if (self->changed_) self->changed_();
            },
            this);
    }

private:
    GtkTextBuffer* buffer_ = nullptr;
    tag_cache tags_;
    std::function<void()> changed_;
};

// -- splitter: a chain of nested GtkPaneds -----------------------------------

// A splitter of n panes is a chain of n-1 nested GtkPaneds (each marked
// "rapport-split-chain"); sizes are one per pane, along the split. The
// paned after `paned` in its chain, if any.
inline GtkWidget* next_in_split(GtkWidget* paned)
{
#if GTK_MAJOR_VERSION >= 4
    GtkWidget* next = gtk_paned_get_end_child(GTK_PANED(paned));
#else
    GtkWidget* next = gtk_paned_get_child2(GTK_PANED(paned));
#endif
    return next && g_object_get_data(G_OBJECT(next), "rapport-split-chain") ? next : nullptr;
}

// Each pane's length along the split, from the chain's first paned.
inline std::vector<float> split_sizes(GtkWidget* first)
{
    const bool vertical =
        gtk_orientable_get_orientation(GTK_ORIENTABLE(first)) == GTK_ORIENTATION_VERTICAL;
    auto length = [&](GtkWidget* w)
    {
#if GTK_MAJOR_VERSION >= 4
        return float(vertical ? gtk_widget_get_height(w) : gtk_widget_get_width(w));
#else
        return float(vertical ? gtk_widget_get_allocated_height(w)
                              : gtk_widget_get_allocated_width(w));
#endif
    };
    std::vector<float> out;
    for (GtkWidget* w = first;;)
    {
        auto* p = GTK_PANED(w);
        out.push_back(float(gtk_paned_get_position(p)));
        GtkWidget* next = next_in_split(w);
        if (!next)
        {
#if GTK_MAJOR_VERSION >= 4
            GtkWidget* last = gtk_paned_get_end_child(p);
#else
            GtkWidget* last = gtk_paned_get_child2(p);
#endif
            out.push_back(last ? length(last) : 0);
            return out;
        }
        w = next;
    }
}

// A divider at `position`. GtkPaned clamps a position to its current
// length, so one asked for before the window has grown to fit (a pane
// opening and the window widening for it, a layout restored before the
// window reaches its saved size) would come out short; it's set again as
// the paned grows, until it fits.
inline void place_divider(GtkPaned* p, int position)
{
    gtk_paned_set_position(p, position);
    int max_pos = 0;
    g_object_get(p, "max-position", &max_pos, nullptr);
    // G_MAXINT: not laid out with both panes yet (one was hidden), so not
    // known to fit either.
    if (max_pos >= position && max_pos != G_MAXINT) return;
    // Too long for now: again when the splitter grows (once it fits).
    auto* want = static_cast<int*>(g_object_get_data(G_OBJECT(p), "rapport-want-position"));
    if (!want)
    {
        want = new int(position);
        g_object_set_data_full(
            G_OBJECT(p), "rapport-want-position", want,
            +[](gpointer d) { delete static_cast<int*>(d); });
        g_signal_connect(
            p, "notify::max-position",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer)
                       {
                           auto* want =
                               static_cast<int*>(g_object_get_data(obj, "rapport-want-position"));
                           if (!want || *want < 0) return;
                           int max_pos = 0;
                           g_object_get(obj, "max-position", &max_pos, nullptr);
                           if (max_pos == G_MAXINT) return;
                           gtk_paned_set_position(GTK_PANED(obj), std::min(*want, max_pos));
                           if (max_pos >= *want) *want = -1;  // there: the user's from here on
                       }),
            nullptr);
    }
    *want = position;
}

// Overrides the first layout from stretch factors: each divider placed
// (place_divider), from the chain's first paned.
inline void set_split_sizes(GtkWidget* first, const std::vector<float>& s)
{
    g_object_set_data(G_OBJECT(first), "rapport-user-sized", GINT_TO_POINTER(1));
    GtkWidget* w = first;
    for (size_t i = 0; i + 1 < s.size() && w; ++i)
    {
        if (s[i] > 0) place_divider(GTK_PANED(w), int(s[i]));
        w = next_in_split(w);
    }
}

inline void queue_draw_tree(GtkWidget* w)
{
    gtk_widget_queue_draw(w);
#if GTK_CHECK_VERSION(4, 0, 0)
    // GTK 4 keeps each widget's drawing; a canvas painting theme roles
    // must be asked again itself.
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        queue_draw_tree(c);
#endif
}

// The theme, its dark variant or the desktop's accent changed: resolve the
// roles again, tell the listeners (CSS written with colours in it) and
// repaint everything.
inline void theme_changed()
{
    auto& t = theme_states();
    t.cache = {};
    std::vector<std::function<void()>> tell;
    for (auto& [owner, f] : t.listeners) tell.push_back(f);
    for (auto& f : tell) f();
    GList* tops = gtk_window_list_toplevels();
    for (GList* l = tops; l; l = l->next) queue_draw_tree(GTK_WIDGET(l->data));
    g_list_free(tops);
}

// GTK's own theme (Adwaita, Default): its selection blue is a constant,
// not a choice, so the desktop's accent comes first. Another theme's
// accent is its author's choice and is kept.
inline bool default_gtk_theme()
{
    gchar* name = nullptr;
    g_object_get(gtk_settings_get_default(), "gtk-theme-name", &name, nullptr);
    const std::string_view n = name ? name : "";
    // (libadwaita sets Adwaita-empty, its stylesheet being its own.)
    const bool builtin = n.empty() || n == "Adwaita" || n == "Adwaita-dark" ||
                         n == "Adwaita-empty" || n == "Default" || n == "Default-dark" ||
                         n == "Default-light";
    g_free(name);
    return builtin;
}

// The first of `names` the theme defines (and isn't transparent). The
// lookup is deprecated in GTK 4 with nothing to replace it for named
// colours, so it's kept until one exists.
inline std::optional<color> named_color(GtkWidget* w, std::initializer_list<const char*> names)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkStyleContext* ctx = gtk_widget_get_style_context(w);
    for (const char* name : names)
    {
        GdkRGBA c;
        if (gtk_style_context_lookup_color(ctx, name, &c) && c.alpha > 0) return to_color(c);
    }
    G_GNUC_END_IGNORE_DEPRECATIONS
    return std::nullopt;
}

// What the desktop says a role is: the theme's named colours (libadwaita's
// names first, then the classic ones GTK's own themes and most others
// define) and the settings portal's accent; nullopt where it says nothing.
inline std::optional<color> desktop_color(GtkWidget* w, theme_color role)
{
    using tc = theme_color;
    const std::optional<color> portal = theme_states().portal_accent;
    const bool portal_first = portal && default_gtk_theme();
    switch (role)
    {
    case tc::window: return named_color(w, {"window_bg_color", "theme_bg_color"});
    case tc::window_text: return named_color(w, {"window_fg_color", "theme_fg_color"});
    case tc::base: return named_color(w, {"view_bg_color", "theme_base_color"});
    case tc::text: return named_color(w, {"view_fg_color", "theme_text_color"});
    case tc::accent:
    case tc::highlight:
        if (portal_first) return portal;
        if (auto c = named_color(w, {"accent_bg_color", "accent_color", "theme_selected_bg_color"}))
            return c;
        return portal;
    case tc::highlight_text:
        if (portal_first) return palette::text_on(*portal);
        if (auto c = named_color(w, {"accent_fg_color", "theme_selected_fg_color"})) return c;
        if (portal) return palette::text_on(*portal);
        return std::nullopt;
    case tc::link: return named_color(w, {"link_color", "theme_link_color"});
    case tc::positive: return named_color(w, {"success_color", "theme_success_color"});
    case tc::warning: return named_color(w, {"warning_color", "theme_warning_color"});
    case tc::negative:
        return named_color(w, {"error_color", "destructive_color", "theme_error_color"});
    case tc::header: return named_color(w, {"headerbar_bg_color", "theme_header_bg_color"});
    case tc::header_text: return named_color(w, {"headerbar_fg_color", "theme_header_fg_color"});
    case tc::card: return named_color(w, {"card_bg_color"});
    // GTK's own theme draws tooltips black at 80% over anything, with
    // white text; others may name theirs.
    case tc::tooltip:
        if (auto c = named_color(w, {"tooltip_bg_color", "theme_tooltip_bg_color"})) return c;
        if (default_gtk_theme()) return color{0x33, 0x33, 0x33};
        return std::nullopt;
    case tc::tooltip_text:
        if (auto c = named_color(w, {"tooltip_fg_color", "theme_tooltip_fg_color"})) return c;
        if (default_gtk_theme()) return color{0xff, 0xff, 0xff};
        return std::nullopt;
    // Hover and pressed as GTK's soft-selection lists tint their rows.
    case tc::hover: return soft_hover_color(w);
    case tc::pressed: return soft_selection_colors(w).first;
    case tc::highlight_inactive: return named_color(w, {"theme_unfocused_selected_bg_color"});
    case tc::highlight_inactive_text: return named_color(w, {"theme_unfocused_selected_fg_color"});
    case tc::border: return named_color(w, {"borders", "border_color"});
    case tc::disabled_text: return named_color(w, {"insensitive_fg_color"});
    default: return std::nullopt;
    }
}

inline color widget_text_color(GtkWidget* w)
{
    GdkRGBA fg;
#if GTK_CHECK_VERSION(4, 0, 0)
    gtk_widget_get_color(w, &fg);
#else
    GtkStyleContext* ctx = gtk_widget_get_style_context(w);
    gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg);
#endif
    return to_color(fg);
}

inline color theme_color_of(GtkWidget* w, theme_color role)
{
    std::optional<color>& slot = theme_states().cache[size_t(role)];
    if (slot) return *slot;
    color c;
    if (std::optional<color> d = desktop_color(w, role))
    {
        c = *d;
        // A surface a theme gives as a tint (libadwaita's dark cards are
        // white at 8%) as it shows on the window, so it can be painted
        // anywhere.
        using tc = theme_color;
        if (c.a < 255 && (role == tc::header || role == tc::card || role == tc::tooltip ||
                          role == tc::highlight_inactive))
            c = palette::over(c, theme_color_of(w, tc::window));
    }
    else
    {
        // A theme naming nothing: Adwaita's colours, dark when its text is
        // light; every other role derived from these.
        const color fg = widget_text_color(w);
        const bool dark = palette::luminance(fg) > 0.5f;
        using tc = theme_color;
        switch (role)
        {
        case tc::window_text:
        case tc::text: c = {fg.r, fg.g, fg.b, 255}; break;
        case tc::window: c = dark ? color{0x24, 0x24, 0x24} : color{0xfa, 0xfa, 0xfa}; break;
        case tc::base: c = dark ? color{0x1e, 0x1e, 0x1e} : color{0xff, 0xff, 0xff}; break;
        case tc::accent: c = {0x35, 0x84, 0xe4}; break;
        default:
            c = palette::derived(role,
                                 {theme_color_of(w, tc::window), theme_color_of(w, tc::window_text),
                                  theme_color_of(w, tc::base), theme_color_of(w, tc::text),
                                  theme_color_of(w, tc::accent)});
        }
    }
    slot = c;
    return c;
}

// A widget never shown, for looking up the theme's colours when no other
// is at hand (rewriting CSS after a theme change).
inline GtkWidget* theme_probe()
{
    static GtkWidget* w = GTK_WIDGET(g_object_ref_sink(gtk_label_new("")));
    return w;
}

inline std::string css_rgb(color c)
{
    return "rgb(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) +
           ")";
}

// CSS with theme colours written into it (a theme's @names aren't all
// defined by every theme): `rule()` now, and again after every theme
// change. GTK 4: one display-wide provider, kept for the process; GTK 3:
// `w`'s own, rewritten until forget_theme_listener(owner).
inline void load_css(GtkCssProvider* css, const std::string& rule)
{
#if GTK_CHECK_VERSION(4, 0, 0)
    gtk_css_provider_load_from_string(css, rule.c_str());
#else
    gtk_css_provider_load_from_data(css, rule.c_str(), -1, nullptr);
#endif
}
#if GTK_CHECK_VERSION(4, 0, 0)
inline void themed_display_css(GtkWidget* w, std::string (*rule)())
{
    GtkCssProvider* css = gtk_css_provider_new();
    load_css(css, rule());
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(w), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    on_theme_changed(css, [css, rule] { load_css(css, rule()); });
}
#else
inline void themed_widget_css(GtkWidget* w, const void* owner, std::function<std::string()> rule)
{
    GtkCssProvider* css = gtk_css_provider_new();
    load_css(css, rule());
    gtk_style_context_add_provider(gtk_widget_get_style_context(w), GTK_STYLE_PROVIDER(css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    // The widget holds the provider; the listener goes with the list.
    on_theme_changed(owner, [css, rule] { load_css(css, rule()); });
    g_object_unref(css);
}
#endif

// The settings portal's value for org.freedesktop.appearance `key`,
// unwrapped; nullptr without one. ReadOne (portal version 2), else Read,
// whose value is wrapped once more.
inline GVariant* portal_setting(GDBusConnection* bus, const char* key)
{
    for (const char* method : {"ReadOne", "Read"})
    {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Settings", method,
            g_variant_new("(ss)", "org.freedesktop.appearance", key), G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 500, nullptr, &error);
        g_clear_error(&error);
        if (!reply) continue;
        GVariant* value = nullptr;
        g_variant_get(reply, "(v)", &value);
        g_variant_unref(reply);
        while (value && g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT))
        {
            GVariant* inner = g_variant_get_variant(value);
            g_variant_unref(value);
            value = inner;
        }
        if (value) return value;
    }
    return nullptr;
}

// The portal's accent-color, (ddd); none when it's out of range (unset).
inline std::optional<color> portal_accent_of(GVariant* v)
{
    if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE("(ddd)"))) return std::nullopt;
    double r = -1, g = -1, b = -1;
    g_variant_get(v, "(ddd)", &r, &g, &b);
    return palette::from_portal_accent(r, g, b);
}

// Theme changes GTK announces: a new theme, or its dark variant.
inline void watch_gtk_theme()
{
    static bool watching = false;
    if (watching) return;
    watching = true;
    GtkSettings* settings = gtk_settings_get_default();
    for (const char* property : {"gtk-theme-name", "gtk-application-prefer-dark-theme",
                                 "gtk-interface-color-scheme", "gtk-interface-contrast"})
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(settings), property))
            g_signal_connect(settings, ("notify::" + std::string(property)).c_str(),
                             G_CALLBACK(+[](GObject*, GParamSpec*, gpointer) { theme_changed(); }),
                             nullptr);
}

// The desktop's appearance, which plain GTK doesn't follow (libadwaita
// does): the Settings portal's org.freedesktop.appearance color-scheme,
// else GNOME's own key where there's no portal, and the portal's
// accent-color. `apply` gets 0 (no preference), 1 (dark) or 2 (light)
// now, before anything is shown, and again on every change; with
// RAPPORT_THEME=dark|light, that once and never the desktop's.
// The desktop's scheme as last read, and how the backend applies one.
struct scheme_state
{
    void (*apply)(int scheme) = nullptr;
    int desktop = 0;
};
inline scheme_state& schemes()
{
    static scheme_state s;
    return s;
}
// The app's or RAPPORT_THEME's scheme, else the desktop's.
inline void apply_scheme()
{
    const scheme_state& s = schemes();
    if (!s.apply) return;
    const int forced = palette::forced_scheme();
    s.apply(forced ? forced : s.desktop);
}
inline void desktop_scheme_changed(int desktop)
{
    schemes().desktop = desktop;
    apply_scheme();
}

inline void follow_color_scheme(void (*apply)(int scheme))
{
    schemes().apply = apply;
    watch_gtk_theme();
    apply_scheme();
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    g_clear_error(&error);
    if (bus)
    {
        GVariant* scheme = portal_setting(bus, "color-scheme");
        GVariant* accent = portal_setting(bus, "accent-color");
        const bool known = scheme && g_variant_is_of_type(scheme, G_VARIANT_TYPE_UINT32);
        if (known) desktop_scheme_changed(int(g_variant_get_uint32(scheme)));
        theme_states().portal_accent = portal_accent_of(accent);
        if (scheme) g_variant_unref(scheme);
        if (accent) g_variant_unref(accent);
        if (known || accent)
        {
            g_dbus_connection_signal_subscribe(
                bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Settings",
                "SettingChanged", "/org/freedesktop/portal/desktop", "org.freedesktop.appearance",
                G_DBUS_SIGNAL_FLAGS_NONE,
                +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                    GVariant* params, gpointer)
                {
                    const gchar* ns = nullptr;
                    const gchar* key = nullptr;
                    GVariant* v = nullptr;
                    g_variant_get(params, "(&s&sv)", &ns, &key, &v);
                    if (g_strcmp0(key, "color-scheme") == 0 &&
                        g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
                        desktop_scheme_changed(int(g_variant_get_uint32(v)));
                    if (g_strcmp0(key, "accent-color") == 0)
                    {
                        theme_states().portal_accent = portal_accent_of(v);
                        theme_changed();
                    }
                    if (v) g_variant_unref(v);
                },
                nullptr, nullptr);
            if (known) return;  // the bus stays referenced for the subscription
        }
        else
            g_object_unref(bus);
    }
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    GSettingsSchema* schema =
        source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", true)
               : nullptr;
    const bool has_key = schema && g_settings_schema_has_key(schema, "color-scheme");
    if (schema) g_settings_schema_unref(schema);
    if (!has_key) return;
    GSettings* gnome = g_settings_new("org.gnome.desktop.interface");  // kept for the process
    auto read = +[](GSettings* s)
    {
        gchar* v = g_settings_get_string(s, "color-scheme");
        const int scheme = g_strcmp0(v, "prefer-dark") == 0    ? 1
                           : g_strcmp0(v, "prefer-light") == 0 ? 2
                                                               : 0;
        g_free(v);
        return scheme;
    };
    desktop_scheme_changed(read(gnome));
    g_signal_connect(
        gnome, "changed::color-scheme",
        G_CALLBACK(+[](GSettings* s, gchar*, gpointer f)
                   { desktop_scheme_changed(reinterpret_cast<int (*)(GSettings*)>(f)(s)); }),
        reinterpret_cast<gpointer>(read));
}

}  // namespace rap::gtkcommon
