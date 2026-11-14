// The toolbar band's metrics and its text fields (Windows 11, the tabs in the
// caption, a base surface: the band of win32_caption_tabs.h, Explorer's
// address row).
//
// The band's toolbar takes WinUI's command-bar metrics, as Explorer's address
// row has them: a taller row, fields as tall as WinUI's text boxes, the
// back, forward, up and refresh buttons as wide as Explorer's.
//
// A field in it (an entry) stays the native EDIT, for everything but its
// look: UI Automation, the IME, its context menu, caret, selection and undo
// are all the control's own. Only its frame and background are drawn here,
// as WinUI draws a TextBox: rounded, opaque within its shape, its fill by
// state (rest, hover, focus with the accent along its bottom, disabled), the
// text vertically centred in it, and its placeholder (and, disabled, its
// text) in WinUI's colours. Round its shape, the band's layer at its own
// alpha, premultiplied, as the band draws it (Mica Alt through it), its
// edge antialiased into it. Under high contrast nothing is drawn: the native bordered EDIT at
// the band's metrics, in the system colours.
//
// Everywhere else (other entries, Windows 10, stage 1's tabs, a window
// without a base surface) nothing here applies.
#pragma once

#include "win32_common.h"

namespace rap::win32
{

// In logical px; each scaled with band_px. Explorer's as measured at 100% on
// Windows 11 (24H2), and WinUI's resource where there's one.
namespace band_metrics
{
// The row from the strip's bottom to the line under it (the line not
// included): Explorer's measures 46. WinUI names no command-bar height that
// matches it (CommandBar's own is 48, its compact one 40).
inline constexpr float row = 46;
// A field's height: TextControlThemeMinHeight. Explorer's boxes measure 32,
// leaving (row - field) / 2 = 7 above and below each.
inline constexpr float field = 32;
// The row's ends: the first button 7 in from the window's edge, the last
// field ending 11 before the other (measured); and what's between two
// items, 8 (between the address and search boxes, measured).
inline constexpr float start = 7, end = 11, gap = 8;
// The navigation buttons: 40 by 32 each, 8 apart (Explorer's back, forward,
// up and refresh: their glyphs' centres 48 apart, measured).
inline constexpr float nav_width = 40, nav_height = 32, nav_gap = 8;
// A field's corners: ControlCornerRadius. Its border:
// TextControlBorderThemeThickness, and the focused one's bottom,
// TextControlBorderThemeThicknessFocused (1,1,1,2).
inline constexpr float corner = 4, border = 1, focus_line = 2;
// Its text's start and end: TextControlThemePadding's left and right
// (10,5,6,6), inside the border. (Vertically the text is centred.)
inline constexpr float text_start = 10, text_end = 6;
// Its text's size: ControlContentThemeFontSize (Explorer's address and
// search text measure the same), where Win32's message font is 9 pt (12).
inline constexpr float font = 14;
// A button inside a field (an entry's action, its clear button): 28 wide,
// 3 in from the border, so a search glyph sits as far in as Explorer's
// (its centre 18 from the box's edge, measured).
inline constexpr float button = 28, button_inset = 3;
}  // namespace band_metrics

// A band metric at `dpi`, in device px.
int band_px(float logical, UINT dpi);

// An entry's EDIT made a band field (`on`), or back to the plain one. Sets
// its font (WinUI's size, the UI face), its frame (none drawn natively but
// under high contrast) and its non-client padding; the caller relays it out.
void set_band_field(HWND edit, bool on);
bool is_band_field(HWND edit);
// A band field whose look is drawn here: not under high contrast.
bool band_field_drawn(HWND edit);
// The desktop's colours, contrast or DPI changed: frame and font anew.
void refresh_band_field(HWND edit);
// The field's height for its font: band_metrics::field, more if its text
// (a large text size) wouldn't fit.
int band_field_height(HWND edit);
// The non-client space each side of a drawn field's client area (where its
// buttons start and end): the border and button_inset.
int band_field_side(UINT dpi);

// WM_CTLCOLOREDIT and WM_CTLCOLORSTATIC (read-only, disabled) from a drawn
// band field: its fill and text colours, the brush in `result`. False for any
// other control.
bool band_field_ctlcolor(HWND control, HDC dc, LRESULT& result);
// A button inside drawn field `edit`: its fill (pressed, hovered or neither)
// and its glyph's ink, opaque. False if `edit` isn't a drawn band field.
bool band_field_button_colors(HWND edit, bool pressed, bool hot, color& fill, color& ink);

// The test tools' message (RegisterWindowMessage "rapport-band-field"), sent
// to an EDIT: 0 if it isn't a band field, else 1 | 2 when its look is drawn
// | 4 hovered | 8 focused.
UINT band_field_query_message();

}  // namespace rap::win32
