// Windows 11's colours for chrome rapport draws itself where WinUI would
// (the document tabs in the caption): WinUI's own resources, by name, as
// its Common_themeresources_any.xaml has them for the light and dark
// themes. Internal to the win32 backend and only for Windows 11 drawn
// chrome: the theme roles (theme_color) stay what they are, since they
// also drive the native controls and separators, matched to Explorer.
// High contrast has none of these: the system colours, as ever.
#pragma once

#include "rapport/widgets.h"

namespace rap::win32::winui
{

// #AARRGGBB, as the XAML writes it.
constexpr color argb(uint32_t v)
{
    return color{uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), uint8_t(v >> 24)};
}

struct chrome_palette
{
    color text_primary;     // TextFillColorPrimary
    color text_secondary;   // TextFillColorSecondary
    color stroke;           // ControlStrokeColorSecondary
    color subtle_secondary; // SubtleFillColorSecondary: a button hovered
    color subtle_tertiary;  // SubtleFillColorTertiary: a button pressed
    // LayerOnMicaBaseAltFillColorSecondary: a tab not current, hovered
    // (TabViewItemHeaderBackgroundPointerOver). Translucent, over Mica.
    color layer_alt_secondary;
};

// Light: TextFillColorPrimary #E4000000 (#1B1B1B on white),
// TextFillColorSecondary #9E000000 (#616161 on white),
// ControlStrokeColorSecondary #29000000 (#D6D6D6 on white; #C9C9C9 on a
// window that isn't a base surface, whose band is #F0F0F0),
// SubtleFillColorSecondary #09000000, SubtleFillColorTertiary #06000000,
// LayerOnMicaBaseAltFillColorSecondary #0A000000.
//
// The stroke: TabView's own border (TabViewBorderBrush) is
// CardStrokeColorDefault, #0F000000, which is #F0F0F0 drawn opaque on white;
// Explorer's #DADADA is probably that card stroke over Mica, which the
// strip's line can't be: it's opaque, one with the band under it.
// ControlStrokeColorSecondary is the stroke resource that comes out nearest
// drawn opaque.
inline constexpr chrome_palette light{argb(0xE4000000), argb(0x9E000000), argb(0x29000000),
                                      argb(0x09000000), argb(0x06000000), argb(0x0A000000)};

// Dark: TextFillColorPrimary #FFFFFF, TextFillColorSecondary #C5FFFFFF,
// ControlStrokeColorSecondary #18FFFFFF, SubtleFillColorSecondary
// #0FFFFFFF, SubtleFillColorTertiary #0AFFFFFF,
// LayerOnMicaBaseAltFillColorSecondary #0FFFFFFF.
inline constexpr chrome_palette dark{argb(0xFFFFFFFF), argb(0xC5FFFFFF), argb(0x18FFFFFF),
                                     argb(0x0FFFFFFF), argb(0x0AFFFFFF), argb(0x0FFFFFFF)};

// `top` over opaque `under`, opaque.
constexpr color over(color top, color under)
{
    auto mix = [&](uint8_t t, uint8_t u)
    { return uint8_t((t * top.a + u * (255 - top.a) + 127) / 255); };
    return color{mix(top.r, under.r), mix(top.g, under.g), mix(top.b, under.b), 255};
}

}  // namespace rap::win32::winui
