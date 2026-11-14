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
// Out of the band an entry is a field of this kind too where the app's
// controls are fluent (look_options::controls: win32_band_face.h), on its
// window's own surface; native, or before Windows 11, it's the plain EDIT.
#pragma once

#include "win32_common.h"

namespace rap::win32
{

// In logical px; each scaled with band_px. Explorer's as measured at 100% on
// Windows 11 (24H2), and WinUI's resource where there's one (WinUI 2's
// themeresources, as its winui2/main branch has them).
namespace band_metrics
{
// The row's ends: the first button 7 in from the window's edge, the last
// field ending 11 before the other (measured); and what's between two
// items, 8 (between the address and search boxes, measured).
inline constexpr float start = 7, end = 11, gap = 8;
// The navigation buttons 8 apart (Explorer's back, forward, up and refresh:
// their glyphs' centres 48 apart, measured, each 40 wide).
inline constexpr float nav_gap = 8;
// A field's corners: ControlCornerRadius. Its border:
// TextControlBorderThemeThickness, and the focused one's bottom,
// TextControlBorderThemeThicknessFocused (1,1,1,2). A button's corners and
// border the same (ControlCornerRadius, ButtonBorderThemeThickness 1).
inline constexpr float corner = 4, border = 1, focus_line = 2;
// Its text's start and end: TextControlThemePadding's left and right
// (10,5,6,6), inside the border. (Vertically the text is centred.)
inline constexpr float text_start = 10, text_end = 6;
// A button inside a field (an entry's action, its clear button, an editable
// combo box's chevron): 28 wide, 3 in from the border, so a search glyph
// sits as far in as Explorer's (its centre 18 from the box's edge,
// measured).
inline constexpr float button = 28, button_inset = 3;
// A button's content in from its border each side: ButtonPadding's left and
// right (11,5,11,6). (Vertically it's centred.) An icon and text 8 apart.
inline constexpr float button_padding = 11, icon_gap = 8;
// A drop-down button's chevron: DropDownButton's fallback glyph (Segoe
// Fluent Icons' ChevronDownSmall, U+E96E) at 8, in a 12 square 8 after the
// content.
inline constexpr float menu_glyph = 8, menu_glyph_box = 12, menu_glyph_gap = 8;
// A drop-down list's (ComboBox's closed face): its text ComboBoxPadding's
// 12 in; its chevron (ChevronDown, U+E70D) at 12, in a 12 square right
// aligned 14 from the edge, in the template's last column, 38 wide.
inline constexpr float combo_text_start = 12, combo_glyph = 12, combo_glyph_end = 14,
                       combo_glyph_column = 38;
// WinUI's focus visual: FocusVisualPrimaryThickness 2 (FocusStrokeColorOuter)
// round FocusVisualSecondaryThickness 1 (FocusStrokeColorInner). WinUI
// draws it 3 outside the control (FocusVisualMargin -3); a child window
// can't draw outside itself, so here it's on the control's own edge, over
// its border, its corners rounded as the control's are.
inline constexpr float focus_outer = 2, focus_inner = 1;
}  // namespace band_metrics

// What differs between the band's two densities: WinUI's standard sizes, or
// its compact ones (band_density_now). In logical px.
struct band_density
{
    // The row from the strip's bottom to the line under it (the line not
    // included).
    float row;
    // Every control's height in it: a field's (TextControlThemeMinHeight),
    // a button's, a drop-down's, a tool button's.
    float field;
    // A tool button's width.
    float nav_width;
    // The controls' text size; 0: the message font's own.
    float font;
    // TextControlThemePadding's top and bottom: what a field taller than
    // `field` (a large text size) keeps round its text.
    float text_top, text_bottom;
    bool compact;
};
// Standard: Explorer's row measures 46 (WinUI names no command-bar height
// that matches it: CommandBar's own is 48, its compact one 40), its boxes
// 32 (TextControlThemeMinHeight), leaving 7 above and below each; its
// navigation buttons 40 by 32. The text ControlContentThemeFontSize, 14
// (Explorer's address and search text measure the same), where Win32's
// message font is 9 pt (12).
inline constexpr band_density standard_band{46, 32, 40, 14, 5, 6, false};
// Compact: WinUI's compact TextControlThemeMinHeight, 24, 6 above and below
// it; tool buttons 32 wide; the text the message font's size. WinUI 2's
// repository has no compact density dictionary to read its paddings from:
// the field keeps 2 above and 3 below its text (what 24 leaves the message
// font's line, centred), the buttons ButtonPadding's sides, everything
// centred vertically.
inline constexpr band_density compact_band{36, 24, 32, 0, 2, 3, true};

// The app's control style and density (look_options::controls, density),
// as the backend's made.
void set_control_style(control_style style, rap::density density);
// rapport's controls in WinUI's look: fluent or automatic, on Windows 11
// (build 22000 and later); native before it, or asked for.
bool fluent_controls();
// Their faces drawn now: fluent, and not under high contrast (native then,
// at the density's heights).
bool faces_on();
// The density controls take: the app's (look_options::density), compact
// where the environment says RAPPORT_DENSITY=compact (a development aid, as
// RAPPORT_THEME is), or a test's (set_band_density_for_tests).
const band_density& band_density_now();
// The density a control out of the band takes: band_density_now() where
// the controls are fluent, else none (native, at native metrics).
const band_density* face_density();
// For tests: compact or not (0, 1), or the environment's again (-1). Windows
// laid out after it take it.
void set_band_density_for_tests(int compact);

// A band metric at `dpi`, in device px.
int band_px(float logical, UINT dpi);

// The band's text at `dpi` for `density`: WinUI's size (enlarged by the
// desktop's text size, as the message font is), or the message font.
HFONT band_font(UINT dpi, const band_density& density);

// An entry's EDIT made a band field of `density`, or back to the plain one
// (null). Sets its font (the density's size, the UI face), its frame (none
// drawn natively but under high contrast) and its non-client padding; the
// caller relays it out.
void set_band_field(HWND edit, const band_density* density);
bool is_band_field(HWND edit);
// A band field whose look is drawn here: not under high contrast.
bool band_field_drawn(HWND edit);
// The desktop's colours, contrast or DPI changed: frame and font anew.
void refresh_band_field(HWND edit);
// The field's height for its font: its density's field, more if its text
// (a large text size) wouldn't fit.
int band_field_height(HWND edit);
// The same for any band control showing `font`'s text at `dpi`.
int band_control_height(HFONT font, UINT dpi, const band_density& density);
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

// A band field's look in a state, for a control drawn as one (an editable
// combo box's face): its fill, its ring (hovered, focused, disabled), the
// accent line along its bottom focused, its text, and a button's fill in it
// (pressed, hovered or neither); each opaque over `behind`.
struct band_field_look
{
    color face, stroke, accent, text, button;
    bool ringed = false, focused = false;
};
band_field_look band_field_look_at(color behind, bool enabled, bool focused, bool hot,
                                   bool pressed_button, bool hot_button);

// The test tools' message (RegisterWindowMessage "rapport-band-field"), sent
// to an EDIT: 0 if it isn't a band field, else 1 | 2 when its look is drawn
// | 4 hovered | 8 focused.
UINT band_field_query_message();

}  // namespace rap::win32
