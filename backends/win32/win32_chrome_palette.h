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
    // LayerFillColorDefault: the band under the tabs (Explorer's address
    // row) and the current tab on it. Opaque: see below.
    color layer;
};

// `top` over opaque `under`, opaque.
constexpr color over(color top, color under)
{
    auto mix = [&](uint8_t t, uint8_t u)
    { return uint8_t((t * top.a + u * (255 - top.a) + 127) / 255); };
    return color{mix(top.r, under.r), mix(top.g, under.g), mix(top.b, under.b), 255};
}

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
//
// The layer: LayerFillColorDefault is #80FFFFFF, translucent, meant over
// Mica, which the band (opaque, one with the window under it) can't be
// over. Drawn opaque, it's what Explorer's address row measures at 100%,
// #F8F8F8 (#80FFFFFF over SolidBackgroundFillColorBase, #F3F3F3, would be
// #F9F9F9).
inline constexpr chrome_palette light{argb(0xE4000000), argb(0x9E000000), argb(0x29000000),
                                      argb(0x09000000), argb(0x06000000), argb(0x0A000000),
                                      argb(0xFFF8F8F8)};

// Dark: TextFillColorPrimary #FFFFFF, TextFillColorSecondary #C5FFFFFF,
// ControlStrokeColorSecondary #18FFFFFF, SubtleFillColorSecondary
// #0FFFFFFF, SubtleFillColorTertiary #0AFFFFFF,
// LayerOnMicaBaseAltFillColorSecondary #0FFFFFFF. The layer:
// LayerFillColorDefault #4C3A3A3A drawn opaque over Mica's dark base
// (SolidBackgroundFillColorBase, #202020): #282828.
inline constexpr chrome_palette dark{argb(0xFFFFFFFF), argb(0xC5FFFFFF), argb(0x18FFFFFF),
                                     argb(0x0FFFFFFF), argb(0x0AFFFFFF), argb(0x0FFFFFFF),
                                     over(argb(0x4C3A3A3A), argb(0xFF202020))};

}  // namespace rap::win32::winui
