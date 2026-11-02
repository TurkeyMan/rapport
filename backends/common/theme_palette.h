// The theme roles' arithmetic, shared by every backend: what a desktop
// publishes arrives in many shapes (the settings portal's accent as three
// doubles, kdeglobals' "r,g,b", Windows' ABGR registry words), and the roles
// a desktop doesn't name are derived from the ones it does. Pure functions,
// with no toolkit, so tests/theme_palette_test.cpp can check them.
#pragma once

#include "rapport/backend.h"
#include "rapport/widgets.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace rap::palette
{

inline color mix(color a, color b, float t)
{
    auto m = [t](uint8_t x, uint8_t y) { return uint8_t(std::lround(x + (y - x) * t)); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

// `top` composited over opaque `under`.
inline color over(color top, color under)
{
    color c = mix(under, color{top.r, top.g, top.b, 255}, top.a / 255.f);
    c.a = 255;
    return c;
}

// WCAG relative luminance, 0 (black) to 1 (white).
inline float luminance(color c)
{
    auto lin = [](uint8_t v)
    {
        const float s = v / 255.f;
        return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}

inline float contrast(color a, color b)
{
    const float x = luminance(a), y = luminance(b);
    return (std::max(x, y) + 0.05f) / (std::min(x, y) + 0.05f);
}

// A background dark enough for light text.
inline bool is_dark(color background)
{
    return luminance(background) < 0.18f;
}

// White or black on `background` (an accent may be very light or very
// dark): white wherever it reads as a control's label must (WCAG's 3:1),
// as desktops put white on their mid blues (GNOME's #3584e4 is 3.7:1),
// else whichever reads better.
inline color text_on(color background)
{
    const color white{255, 255, 255}, black{0, 0, 0};
    const float w = contrast(white, background);
    return w >= 3 || w >= contrast(black, background) ? white : black;
}

// `c` with its HSL lightness set to `l` (hue and saturation kept).
inline color with_lightness(color c, float l)
{
    const float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f;
    const float hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
    const float l0 = (hi + lo) / 2;
    const float sat = d == 0 ? 0 : d / (1 - std::abs(2 * l0 - 1));
    float h = 0;  // in sixths of a turn
    if (d > 0)
        h = hi == r ? std::fmod((g - b) / d + 6, 6.f) : hi == g ? (b - r) / d + 2 : (r - g) / d + 4;
    const float chroma = (1 - std::abs(2 * l - 1)) * sat;
    const float x = chroma * (1 - std::abs(std::fmod(h, 2.f) - 1)), m = l - chroma / 2;
    float rgb[3]{};
    const int sector = int(h) % 6;
    const int order[6][3] = {{0, 1, 2}, {1, 0, 2}, {2, 0, 1}, {2, 1, 0}, {1, 2, 0}, {0, 2, 1}};
    const float parts[3] = {chroma, x, 0};  // largest, middle, smallest
    for (int i = 0; i < 3; ++i) rgb[i] = parts[order[sector][i]] + m;
    auto byte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255)); };
    return color{byte(rgb[0]), byte(rgb[1]), byte(rgb[2]), c.a};
}

// -- what desktops publish ----------------------------------------------------

// The settings portal's org.freedesktop.appearance accent-color: (ddd) in
// 0..1; any part outside that range means the user set none.
inline std::optional<color> from_portal_accent(double r, double g, double b)
{
    for (double v : {r, g, b})
        if (!(v >= 0 && v <= 1)) return std::nullopt;  // (NaN too)
    auto byte = [](double v) { return uint8_t(std::lround(v * 255)); };
    return color{byte(r), byte(g), byte(b), 255};
}

// kdeglobals' "r,g,b" or "r,g,b,a", decimal 0..255, spaces allowed.
inline std::optional<color> parse_rgb_list(std::string_view s)
{
    int parts[4] = {0, 0, 0, 255};
    int n = 0;
    while (true)
    {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        if (n == 4) return std::nullopt;
        int v = 0;
        auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc{} || v < 0 || v > 255) return std::nullopt;
        parts[n++] = v;
        s.remove_prefix(size_t(end - s.data()));
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        if (s.empty()) break;
        if (s.front() != ',') return std::nullopt;
        s.remove_prefix(1);
    }
    if (n < 3) return std::nullopt;
    return color{uint8_t(parts[0]), uint8_t(parts[1]), uint8_t(parts[2]), uint8_t(parts[3])};
}

// The value of `key` in `[group]` of an INI text (kdeglobals), as a view
// into `text`; nullopt without one. Later duplicates win, as in KConfig.
inline std::optional<std::string_view> ini_value(std::string_view text, std::string_view group,
                                                 std::string_view key)
{
    auto trim = [](std::string_view v)
    {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
            v.remove_suffix(1);
        return v;
    };
    std::optional<std::string_view> found;
    bool in_group = false;
    while (!text.empty())
    {
        const size_t eol = text.find('\n');
        std::string_view line = trim(text.substr(0, eol));
        text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[')
        {
            in_group =
                line.size() >= 2 && line.back() == ']' && line.substr(1, line.size() - 2) == group;
            continue;
        }
        if (!in_group) continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view k = trim(line.substr(0, eq));
        // KConfig's immutability and locale suffixes: Key[$i], Key[de].
        if (const size_t bracket = k.find('['); bracket != std::string_view::npos)
            k = k.substr(0, bracket);
        if (k == key) found = trim(line.substr(eq + 1));
    }
    return found;
}

inline std::optional<color> ini_color(std::string_view text, std::string_view group,
                                      std::string_view key)
{
    if (auto v = ini_value(text, group, key)) return parse_rgb_list(*v);
    return std::nullopt;
}

// KDE's colour scheme as kdeglobals has it, where the Qt palette has no
// role: the View set's foregrounds for status and links, the decorations
// for hover and focus (the Selection set's, else the View set's), and the
// Header set (Plasma's title areas).
struct kde_colors
{
    std::optional<color> link, positive, negative, neutral;  // neutral: KDE's warning (amber)
    std::optional<color> hover, focus;
    std::optional<color> header, header_text;
};
inline kde_colors parse_kdeglobals(std::string_view text)
{
    kde_colors k;
    k.link = ini_color(text, "Colors:View", "ForegroundLink");
    k.positive = ini_color(text, "Colors:View", "ForegroundPositive");
    k.negative = ini_color(text, "Colors:View", "ForegroundNegative");
    k.neutral = ini_color(text, "Colors:View", "ForegroundNeutral");
    for (std::string_view set : {"Colors:View", "Colors:Selection"})
    {
        if (auto c = ini_color(text, set, "DecorationHover")) k.hover = c;
        if (auto c = ini_color(text, set, "DecorationFocus")) k.focus = c;
    }
    k.header = ini_color(text, "Colors:Header", "BackgroundNormal");
    k.header_text = ini_color(text, "Colors:Header", "ForegroundNormal");
    return k;
}

// HKCU\Software\Microsoft\Windows\DWM AccentColor: 0xAABBGGRR.
inline color from_abgr(uint32_t v)
{
    return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), 255};
}

// DwmGetColorizationColor: 0xAARRGGBB.
inline color from_argb(uint32_t v)
{
    return {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), 255};
}

// HKCU\...\Explorer\Accent AccentPalette: eight RGBA quads, the accent's
// shades from lightest to darkest: light 3, 2, 1, the accent, dark 1, 2, 3
// (and one unused). nullopt unless it's the 32 bytes expected.
struct accent_shades
{
    std::array<color, 7> shade;  // [3] is the accent
    color accent() const { return shade[3]; }
    color light(int n) const { return shade[size_t(3 - n)]; }  // n = 1..3
    color dark(int n) const { return shade[size_t(3 + n)]; }
};
inline std::optional<accent_shades> parse_accent_palette(const uint8_t* bytes, size_t n)
{
    if (!bytes || n != 32) return std::nullopt;
    accent_shades a;
    for (size_t i = 0; i < 7; ++i)
        a.shade[i] = {bytes[i * 4], bytes[i * 4 + 1], bytes[i * 4 + 2], 255};
    return a;
}

// RAPPORT_THEME=dark|light forces a scheme, for snapshots of both without
// touching the desktop's setting: 0 none, 1 dark, 2 light (the settings
// portal's color-scheme numbering).
inline int scheme_from_name(std::string_view v)
{
    return v == "dark" ? 1 : v == "light" ? 2 : 0;
}
// The app's choice (ui::set_color_scheme), in the same numbering.
inline int& app_scheme()
{
    static int scheme = 0;
    return scheme;
}
inline int scheme_number(color_scheme s)
{
    return s == color_scheme::dark ? 1 : s == color_scheme::light ? 2 : 0;
}
// The scheme to use whatever the desktop's: RAPPORT_THEME, else the
// app's; 0 to follow the desktop.
inline int forced_scheme()
{
    const char* v = std::getenv("RAPPORT_THEME");
    if (const int env = v ? scheme_from_name(v) : 0) return env;
    return app_scheme();
}

// Windows' app mode: dark when Settings > Personalisation > Colours says
// so (AppsUseLightTheme 0) or RAPPORT_THEME forces it, never in high
// contrast (whose colours are the system's), and only from 1809 (build
// 17763), the first with dark common controls.
inline bool windows_dark(bool high_contrast, int forced_scheme,
                         std::optional<uint32_t> apps_use_light_theme, uint32_t build)
{
    if (high_contrast || build < 17763) return false;
    if (forced_scheme) return forced_scheme == 1;
    return apps_use_light_theme && *apps_use_light_theme == 0;
}

// Windows' dark mode surfaces (GetSysColor stays light in dark mode):
// Explorer's and Fluent's dark values for what isn't the accent's.
inline std::optional<color> windows_dark_color(theme_color role)
{
    switch (role)
    {
    case theme_color::window: return color{0x20, 0x20, 0x20};
    case theme_color::base: return color{0x19, 0x19, 0x19};
    case theme_color::alternate_base: return color{0x23, 0x23, 0x23};
    case theme_color::window_text:
    case theme_color::text:
    case theme_color::header_text:
    case theme_color::tooltip_text:
    case theme_color::highlight_inactive_text:
    case theme_color::focus: return color{0xff, 0xff, 0xff};
    case theme_color::placeholder_text:
    case theme_color::neutral: return color{0x9a, 0x9a, 0x9a};
    case theme_color::mid: return color{0x6b, 0x6b, 0x6b};
    case theme_color::header:
    case theme_color::card:
    case theme_color::tooltip: return color{0x2b, 0x2b, 0x2b};
    case theme_color::highlight_inactive: return color{0x3a, 0x3a, 0x3a};
    case theme_color::border: return color{0x50, 0x50, 0x50};
    case theme_color::divider: return color{0x33, 0x33, 0x33};
    case theme_color::disabled_text: return color{0x78, 0x78, 0x78};
    default: return std::nullopt;  // the accent's, the status colours, hover and pressed
    }
}

// -- defaults -----------------------------------------------------------------

// Status colours in two design languages, for desktops that name none.
inline color adwaita_status(theme_color role, bool dark)
{
    switch (role)
    {
    case theme_color::positive: return dark ? color{0x8f, 0xf0, 0xa4} : color{0x26, 0xa2, 0x69};
    case theme_color::warning: return dark ? color{0xf8, 0xe4, 0x5c} : color{0x9c, 0x6e, 0x03};
    default: return dark ? color{0xf6, 0x61, 0x51} : color{0xc0, 0x1c, 0x28};
    }
}
inline color fluent_status(theme_color role, bool dark)
{
    switch (role)
    {
    case theme_color::positive: return dark ? color{0x6c, 0xcb, 0x5f} : color{0x0f, 0x7b, 0x0f};
    case theme_color::warning: return dark ? color{0xfc, 0xe1, 0x00} : color{0x9d, 0x5d, 0x00};
    default: return dark ? color{0xff, 0x99, 0xa4} : color{0xc4, 0x2b, 0x1c};
    }
}

// What every role is derived from where a desktop names it not.
struct basics
{
    color window, window_text, base, text, accent;
    bool dark() const { return is_dark(base); }
};

// Selected and hovered list rows in a light tint of the accent, with normal
// text: on light themes the accent's hue at Explorer's lightness (#0078D7
// becomes #CCE8FF selected, about #E5F3FF hovered; mixing toward white drains the
// saturation and reads grey); on dark ones a mix into the base colour.
struct soft_colors
{
    color selected, hover, hover_selected;
};
inline soft_colors soft_selection(color base, color accent)
{
    if (!is_dark(base))
        return {with_lightness(accent, 0.90f), with_lightness(accent, 0.95f),
                with_lightness(accent, 0.88f)};
    return {mix(base, accent, 0.38f), mix(base, accent, 0.16f), mix(base, accent, 0.45f)};
}

// A role from the basics alone: each backend's answer where its desktop
// gives none.
inline color derived(theme_color role, const basics& b)
{
    const bool dark = b.dark();
    switch (role)
    {
    case theme_color::window: return b.window;
    case theme_color::window_text: return b.window_text;
    case theme_color::base: return b.base;
    case theme_color::text: return b.text;
    case theme_color::accent: return b.accent;
    case theme_color::alternate_base: return mix(b.base, b.text, dark ? 0.05f : 0.04f);
    case theme_color::placeholder_text: return mix(b.base, b.text, 0.5f);
    case theme_color::highlight: return b.accent;
    case theme_color::highlight_text: return text_on(b.accent);
    case theme_color::mid: return mix(b.window, b.window_text, 0.35f);
    case theme_color::link:
        // The accent, moved toward the text until it reads as text does.
        for (float t = 0; t <= 1; t += 0.125f)
            if (color c = mix(b.accent, b.text, t); contrast(c, b.base) >= 4.5f) return c;
        return b.text;
    case theme_color::neutral: return mix(b.base, b.text, 0.6f);
    case theme_color::positive:
    case theme_color::negative:
    case theme_color::warning: return adwaita_status(role, dark);
    case theme_color::header: return mix(b.window, b.window_text, dark ? 0.06f : 0.04f);
    case theme_color::header_text: return b.window_text;
    // Raised: the view's colour on light themes, a step lighter than the
    // window on dark ones (where light is near).
    case theme_color::card: return dark ? mix(b.window, b.window_text, 0.06f) : b.base;
    case theme_color::tooltip: return dark ? mix(b.window, b.window_text, 0.1f) : b.base;
    case theme_color::tooltip_text: return b.text;
    case theme_color::hover: return soft_selection(b.base, b.accent).hover;
    case theme_color::pressed: return soft_selection(b.base, b.accent).hover_selected;
    case theme_color::focus: return b.accent;
    // An unfocused window's selection: grey, as Explorer's and GTK's.
    case theme_color::highlight_inactive: return mix(b.base, b.text, dark ? 0.2f : 0.15f);
    case theme_color::highlight_inactive_text: return b.text;
    case theme_color::border: return mix(b.window, b.window_text, dark ? 0.25f : 0.22f);
    case theme_color::divider: return mix(b.base, b.text, dark ? 0.15f : 0.1f);
    case theme_color::disabled_text: return mix(b.window, b.window_text, 0.45f);
    }
    return b.text;
}

}  // namespace rap::palette
