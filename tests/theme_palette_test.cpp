// The theme roles' arithmetic (backends/common/theme_palette.h): what
// desktops publish, parsed, and the roles derived from it.
//   rapport-theme-palette-test
#include "theme_palette.h"

#include <cstdio>
#include <string>

namespace
{

int fails = 0;

std::string hex(rap::color c)
{
    char s[16];
    std::snprintf(s, sizeof s, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return s;
}
std::string hex(std::optional<rap::color> c)
{
    return c ? hex(*c) : "none";
}

void eq(const std::string& got, const std::string& want, const char* what)
{
    if (got == want)
    {
        std::printf("ok   %s\n", what);
        return;
    }
    ++fails;
    std::printf("FAIL %s: got [%s] want [%s]\n", what, got.c_str(), want.c_str());
}

}  // namespace

int main()
{
    namespace p = rap::palette;
    using rap::color;
    using rap::theme_color;

    // The settings portal's accent-color: (ddd) in 0..1, unset outside it.
    eq(hex(p::from_portal_accent(0.2078, 0.5176, 0.8941)), "#3584e4ff", "portal accent");
    eq(hex(p::from_portal_accent(1, 1, 1)), "#ffffffff", "portal accent, white");
    eq(hex(p::from_portal_accent(-1, -1, -1)), "none", "portal accent unset");
    eq(hex(p::from_portal_accent(0.5, 2, 0.5)), "none", "portal accent out of range");

    // kdeglobals' "r,g,b".
    eq(hex(p::parse_rgb_list("61,174,233")), "#3daee9ff", "kde r,g,b");
    eq(hex(p::parse_rgb_list(" 39, 174 ,96 ")), "#27ae60ff", "kde r,g,b with spaces");
    eq(hex(p::parse_rgb_list("1,2,3,128")), "#01020380", "kde r,g,b,a");
    eq(hex(p::parse_rgb_list("1,2")), "none", "kde too few");
    eq(hex(p::parse_rgb_list("1,2,3,4,5")), "none", "kde too many");
    eq(hex(p::parse_rgb_list("1,256,3")), "none", "kde out of range");
    eq(hex(p::parse_rgb_list("#3daee9")), "none", "kde not a list");
    eq(hex(p::parse_rgb_list("")), "none", "kde empty");

    const char* kdeglobals = "[General]\n"
                             "ColorScheme=BreezeDark\n"
                             "\n"
                             "[Colors:Header]\n"
                             "BackgroundNormal=41,44,48\r\n"
                             "ForegroundNormal=252,252,252\n"
                             "[Colors:Selection]\n"
                             "DecorationFocus=61,174,233\n"
                             "DecorationHover=61,174,233\n"
                             "[Colors:View]\n"
                             "# a comment\n"
                             "DecorationHover=1,2,3\n"
                             "ForegroundLink=29,153,243\n"
                             "ForegroundNegative=218,68,83\n"
                             "ForegroundNeutral=246,116,0\n"
                             "ForegroundPositive=39,174,96\n"
                             "ForegroundPositive[$i]=40,175,97\n";
    const p::kde_colors k = p::parse_kdeglobals(kdeglobals);
    eq(hex(k.link), "#1d99f3ff", "kdeglobals link");
    eq(hex(k.positive), "#28af61ff", "kdeglobals positive, the later key wins");
    eq(hex(k.negative), "#da4453ff", "kdeglobals negative");
    eq(hex(k.neutral), "#f67400ff", "kdeglobals neutral (warning)");
    eq(hex(k.hover), "#3daee9ff", "kdeglobals hover, the Selection set's");
    eq(hex(k.focus), "#3daee9ff", "kdeglobals focus");
    eq(hex(k.header), "#292c30ff", "kdeglobals header");
    eq(hex(k.header_text), "#fcfcfcff", "kdeglobals header text");
    eq(std::string(p::ini_value(kdeglobals, "General", "ColorScheme").value_or("?")), "BreezeDark",
       "ini value");
    eq(hex(p::parse_kdeglobals("").link), "none", "empty kdeglobals");

    // Windows: the DWM's AccentColor is ABGR, the colorization colour ARGB.
    eq(hex(p::from_abgr(0xffd77800)), "#0078d7ff", "AccentColor ABGR");
    eq(hex(p::from_argb(0xc40078d7)), "#0078d7ff", "colorization ARGB");
    const uint8_t shades[32] = {0x99, 0xeb, 0xff, 0,    0x4c, 0xc2, 0xff, 0,    0x00, 0x91, 0xf8,
                                0,    0x00, 0x78, 0xd4, 0,    0x00, 0x5a, 0x9e, 0,    0x00, 0x42,
                                0x75, 0,    0x00, 0x26, 0x42, 0,    0xf7, 0x63, 0x0c, 0};
    const auto a = p::parse_accent_palette(shades, sizeof shades);
    eq(a ? hex(a->accent()) : "none", "#0078d4ff", "AccentPalette accent");
    eq(a ? hex(a->light(3)) : "none", "#99ebffff", "AccentPalette light 3");
    eq(a ? hex(a->dark(2)) : "none", "#004275ff", "AccentPalette dark 2");
    eq(p::parse_accent_palette(shades, 28) ? "some" : "none", "none", "AccentPalette short");

    // RAPPORT_THEME's names.
    eq(std::to_string(p::scheme_from_name("dark")), "1", "scheme dark");
    eq(std::to_string(p::scheme_from_name("light")), "2", "scheme light");
    eq(std::to_string(p::scheme_from_name("auto")), "0", "scheme other");

    // Text on an accent: white on a mid or dark one, black on a light one.
    eq(hex(p::text_on({0x00, 0x78, 0xd4})), "#ffffffff", "text on Windows blue");
    eq(hex(p::text_on({0xff, 0xe1, 0x00})), "#000000ff", "text on yellow");
    eq(hex(p::text_on({0x1e, 0x1e, 0x1e})), "#ffffffff", "text on near black");

    // Light and dark.
    eq(p::is_dark({0xff, 0xff, 0xff}) ? "dark" : "light", "light", "white is light");
    eq(p::is_dark({0x1e, 0x1e, 0x1e}) ? "dark" : "light", "dark", "#1e1e1e is dark");
    eq(p::is_dark({0x80, 0x80, 0x80}) ? "dark" : "light", "light", "mid grey is light");

    // Soft selection: Explorer's tints of #0078D7 on light; a mix on dark.
    const p::soft_colors light = p::soft_selection({255, 255, 255}, {0x00, 0x78, 0xd7});
    eq(hex(light.selected), "#cce8ffff", "soft selected, light");
    eq(hex(light.hover), "#e6f4ffff", "soft hover, light");
    const p::soft_colors dark = p::soft_selection({0, 0, 0}, {100, 200, 0});
    eq(hex(dark.selected), "#264c00ff", "soft selected, dark");

    // Derived roles.
    const p::basics b{{0xfa, 0xfa, 0xfa}, {0, 0, 0}, {255, 255, 255}, {0, 0, 0}, {0xff, 0xe1, 0}};
    const color link = p::derived(theme_color::link, b);
    eq(p::contrast(link, b.base) >= 4.5f ? "readable" : hex(link), "readable",
       "derived link reads");
    eq(hex(p::derived(theme_color::highlight_text, b)), "#000000ff", "derived highlight text");
    eq(hex(p::derived(theme_color::warning, b)), "#9c6e03ff", "derived warning, light");
    eq(hex(p::fluent_status(theme_color::positive, true)), "#6ccb5fff", "Fluent success, dark");
    const p::basics light_basics{
        {0xfa, 0xfa, 0xfa}, {0, 0, 0}, {255, 255, 255}, {0, 0, 0}, {0x00, 0x78, 0xd7}};
    eq(hex(p::derived(theme_color::highlight_inactive, light_basics)), "#d9d9d9ff",
       "derived inactive selection, Explorer's grey");
    eq(hex(p::derived(theme_color::hover, light_basics)), "#e6f4ffff",
       "derived hover, the soft tint");
    eq(hex(p::derived(theme_color::card, light_basics)), "#ffffffff", "derived card, light");
    const p::basics dark_basics{{0x24, 0x24, 0x24},
                                {255, 255, 255},
                                {0x1e, 0x1e, 0x1e},
                                {255, 255, 255},
                                {0x35, 0x84, 0xe4}};
    const color card = p::derived(theme_color::card, dark_basics);
    eq(p::luminance(card) > p::luminance(dark_basics.window) ? "raised" : hex(card), "raised",
       "derived card, dark, lighter than the window");
    const color disabled = p::derived(theme_color::disabled_text, dark_basics);
    eq(p::contrast(disabled, dark_basics.window) <
               p::contrast(dark_basics.window_text, dark_basics.window)
           ? "dimmer"
           : hex(disabled),
       "dimmer", "derived disabled text, dimmer than text");
    for (int r = 0; r < rap::theme_color_count; ++r)
        if (rap::print_color(theme_color(r)).a != 255)
            eq("transparent", "opaque", "print colour for every role");

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
