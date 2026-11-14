// The toolbar band's control faces (Windows 11, the tabs in the caption, a
// base surface: the band of win32_caption_tabs.h). Every control in the
// band's row takes the field's height (win32_band_field.h) and WinUI's look,
// as Explorer's row and WinUI's command bars have them: a push button is
// WinUI's Button, a menu button its DropDownButton, a drop-down list its
// ComboBox's closed face, an editable combo box a band field with a chevron
// button, a tool button an AppBarButton (subtle), a separator a line the
// field's height. Labels and check boxes stay native, centred in the row.
//
// Each stays the native control, for everything but its look (keyboard,
// UI Automation, its menu or list): only its face is drawn here, over
// whatever surface it's on (nothing here is the band's but its colours'
// ground), at a density (band_density). The band's controls are printed
// through its glass (win32_band_glass.cpp) on black and on white, so a face
// is drawn over what surface_color_of says is round it (the matte, while
// printed): what's round its rounded shape comes out as the band's layer,
// its antialiased corners at their alpha. A button's or field's fill is
// opaque over the band's colour, as a band field's is (Explorer's boxes
// measure opaque); a subtle face (a tool button's) is translucent, at its
// own alpha over the layer. Under high contrast nothing here is drawn: the native controls, at
// the band's metrics.
//
// Out of the band the same faces are on rapport's controls in every window
// where they're fluent (look_options::controls: automatic is Windows 11),
// at the app's density (face_density), over the window's own surface; the
// band's own metrics (its row, its gap) and its glass are the band's alone.
// Native (asked for, or before Windows 11) nothing here is drawn, and out
// of the band the controls keep their native metrics.
#pragma once

#include "win32_band_field.h"

namespace rap::win32
{

// Which of WinUI's faces.
enum class face_kind
{
    button,  // Button: ControlFillColorDefault, the elevation border
    subtle,  // AppBarButton: transparent at rest
    accent,  // a checked AppBarToggleButton: AccentFillColorDefault
    // AccentButtonStyle (ContentDialog's DefaultButton): a dialog's default
    // push button, the accent with AccentControlElevationBorderBrush.
    default_button,
};
struct face_state
{
    bool hot = false, pressed = false, disabled = false;
    bool focus_ring = false;  // focused, with focus cues shown (band_focus_cues)
};

// Face `kind` in `state` filling `r` of `dc` (a control's client area): the
// surface under `h` round its rounded shape (surface_color_of: the matte
// while printed through the glass), the face over it, antialiased, and
// WinUI's focus visual on its edge while state.focus_ring.
void paint_band_face(HWND h, HDC dc, const RECT& r, face_kind kind, const face_state& state);
// An editable combo box's face: a band field's look (band_field_look) in
// `r`, the button fill in `button` (null: none), the focus line along its
// bottom.
void paint_band_field_face(HWND h, HDC dc, const RECT& r, const band_field_look& look,
                           const RECT* button);
// What a face's fill (a button's, a field's) is opaque over: the band's
// opaque colour in the band (as a band field's), else the surface.
color band_face_ground(HWND h);
// The face's text and glyph colour, opaque over the face over the surface.
color band_face_ink(HWND h, face_kind kind, const face_state& state);
// Focus cues are shown in `h` (WM_QUERYUISTATE: no UISF_HIDEFOCUS): WinUI
// shows its focus visual only for the keyboard, so a focused control shows
// its ring only then.
bool band_focus_cues(HWND h);
// The band's text font for `h` (band_font), its grayscale copy while it's
// on the band's glass (CONTRIBUTING.md: no ClearType over the backdrop).
HFONT band_text_font(HWND h, const band_density& density);
// Where a line of `font` goes in `height` from `top`: its capitals' ink
// centred, as a band field centres its text.
int band_text_top(HDC dc, HFONT font, int top, int height);
// A Segoe Fluent Icons glyph (Segoe MDL2 Assets before it was there) at
// `size` device px, centred in `box`, in `ink`.
void draw_band_glyph(HDC dc, const RECT& box, wchar_t glyph, int size, color ink);
inline constexpr wchar_t chevron_down = 0xE70D, chevron_down_small = 0xE96E;

// A control whose band face is drawn here rather than by a custom draw (a
// drop-down list, an editable combo box, a separator): its WM_PAINT and
// WM_PRINTCLIENT go to band_paint (through the band's glass while it's on
// it), the pointer over it is tracked (band_painted_hot), and its other
// messages are offered to band_message first.
class band_painted
{
public:
    virtual ~band_painted() = default;
    virtual void band_paint(HWND h, HDC dc, const RECT& client) = 0;
    // True if handled, `result` its result.
    virtual bool band_message(HWND, UINT, WPARAM, LPARAM, LRESULT&) { return false; }
    // It's been moved or sized (WM_WINDOWPOSCHANGED), drawn here or (under
    // high contrast) not: a combo box keeps the band's height.
    virtual void band_resized(HWND) {}
};
// `h` painted by `painter` (null: by itself again).
void set_band_painted(HWND h, band_painted* painter);
bool is_band_painted(HWND h);
// The pointer's over it (or over a child of it).
bool band_painted_hot(HWND h);
// It has the keyboard focus (its own WM_SETFOCUS and WM_KILLFOCUS).
bool band_painted_focused(HWND h);

// The test tools' message (RegisterWindowMessage "rapport-band-face"), sent
// to a control: 0 if its band face isn't drawn here, else 1 | 2 hot | 4
// pressed | 8 focus ring.
UINT band_face_query_message();

}  // namespace rap::win32
