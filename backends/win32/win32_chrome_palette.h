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
    // LayerOnMicaBaseAltFillColorDefault: the band under the tabs
    // (Explorer's address row) and the current tab on it, at its own alpha,
    // over the extended frame: the compositor puts Mica Alt under it, as
    // Explorer's (TabView's selected tab is this layer, over Mica Alt).
    color layer_on_mica_base_alt_default;
    // LayerOnMicaBaseAltFillColorTertiary: the same family's opaque one,
    // what the layer comes to over Mica's base, for what can't be
    // translucent: GDI's brushes (a native control's background).
    color layer_on_mica_base_alt_tertiary;
    // DividerStrokeColorDefault: the separator between two tabs
    // (TabViewItemSeparator), translucent.
    color divider;
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
// The layer: LayerOnMicaBaseAltFillColorDefault, #B3FFFFFF (white at 70%),
// over the extended frame, where the compositor puts Mica Alt under it.
// Measured on Explorer (Windows 11, active, light, 100%): its empty strip,
// Mica Alt alone, #CEDCEC; its current tab #F0F5F9 and its address row
// #F1F5F9. White over #CEDCEC comes to those at alpha 0.69, 0.71 and 0.68
// by channel: 0.70, this layer. LayerFillColorDefault (#80FFFFFF), tried
// first, came out #E8EEF6 over the same strip. Opaque:
// LayerOnMicaBaseAltFillColorTertiary, #F9F9F9.
// DividerStrokeColorDefault #0F000000.
inline constexpr chrome_palette light{argb(0xE4000000), argb(0x9E000000), argb(0x29000000),
                                      argb(0x09000000), argb(0x06000000), argb(0x0A000000),
                                      argb(0xB3FFFFFF),   argb(0xFFF9F9F9), argb(0x0F000000)};

// Dark: TextFillColorPrimary #FFFFFF, TextFillColorSecondary #C5FFFFFF,
// ControlStrokeColorSecondary #18FFFFFF, SubtleFillColorSecondary
// #0FFFFFFF, SubtleFillColorTertiary #0AFFFFFF,
// LayerOnMicaBaseAltFillColorSecondary #0FFFFFFF. The layer: dark
// LayerOnMicaBaseAltFillColorDefault, #733A3A3A (#3A3A3A at alpha 0x73,
// 45%), over Mica Alt; opaque, LayerOnMicaBaseAltFillColorTertiary,
// #2C2C2C (the layer over Mica's dark base, #202020, to a level).
// DividerStrokeColorDefault #15FFFFFF.
inline constexpr chrome_palette dark{argb(0xFFFFFFFF), argb(0xC5FFFFFF), argb(0x18FFFFFF),
                                     argb(0x0FFFFFFF), argb(0x0AFFFFFF), argb(0x0FFFFFFF),
                                     argb(0x733A3A3A),   argb(0xFF2C2C2C), argb(0x15FFFFFF)};

}  // namespace rap::win32::winui
