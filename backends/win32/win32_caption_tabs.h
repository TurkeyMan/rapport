// A window's document tabs in its title bar, as Windows 11's Explorer has
// them (window_spec::tabs, stage 2): the strip drawn where the caption was,
// the system's own minimise, maximise and close buttons kept beside it.
//
// Two parts. caption_tabs is the strip: a child window across the top of the
// client area, up to the caption buttons, that draws its tabs itself with
// premultiplied alpha, so what it leaves empty is transparent to the desktop
// compositor and the window's backdrop (Mica) shows through there, and that
// serves its tabs to UI Automation (win32_uia). Where it's empty it lets the
// mouse through to the window (HTTRANSPARENT), which then says it's the
// caption: dragged, double-clicked and right-clicked as one. The caption_*
// functions are the window's side: its frame extended into the client area
// by the strip's height, the title bar's non-client area taken away, its hit
// tests, and the strip's row beside the caption buttons painted to match.
//
// Only on Windows 11 (build 22000 on), where the caption buttons are drawn
// over an extended frame and Mica is the title bar's; before it, and on a
// window with a menu bar or a dialog, the tabs are stage 1's strip in the
// client area (document_tabs_impl).
#pragma once

#include "win32_common.h"
#include "win32_uia.h"

#include <memory>
#include <string>
#include <vector>

namespace rap::win32
{

// Explorer's strip as measured at 100% on Windows 11 (24H2), in logical px.
// Its colours aren't measured here: the surfaces are the theme's roles, the
// text, line and buttons WinUI's (win32_chrome_palette.h).
namespace caption_metrics
{
// The strip, from the window's top to the line under it (the line its
// last row): Explorer's is 39.
inline constexpr float height = 39;
// The current tab: 8 in from the left, 8 down from the top, its top corners
// rounded by 8; each tab about 240 wide where they fit.
inline constexpr float first_tab = 8, tab_top = 8, corner = 8;
inline constexpr float widest_tab = 240;
// "+": a button whose glyph's centre is about 18 after the last tab.
inline constexpr float add_gap = 4, add_size = 28;
// What's left before the caption buttons, as Explorer leaves to drag by.
inline constexpr float end_gap = 8;
// The close button's box at a tab's end, and the tab's icon.
inline constexpr float close_size = 24, close_inset = 6, icon = 16, text_inset = 12, text_gap = 8;
}  // namespace caption_metrics

// One tab as the strip shows it. `key` is the caller's, stable for as long
// as the tab is there (UI Automation's elements are made from it); the icon
// is the caller's, kept until it calls set_tabs again.
struct caption_tab
{
    unsigned key = 0;
    std::wstring title;
    std::wstring tooltip;  // empty: the title
    HICON icon = nullptr;
};

// What the user did to the strip, told to its owner on the window's thread.
// Each may change the tabs (set_tabs), or destroy the window: the strip does
// nothing more once it's told.
class caption_tabs_owner
{
public:
    virtual void caption_select(int index) = 0;  // a click, Enter or Space, UIA's Select
    virtual void caption_close(int index) = 0;   // its close button, a middle click, Delete
    virtual void caption_new() = 0;              // "+"
    // A right click on tab `index` (-1: none), or the menu key; `screen`
    // where the menu goes.
    virtual void caption_menu(int index, POINT screen) = 0;
    // A tab dragged along the strip has passed another, or gone back as its
    // drag's cancelled: the strip's moved it from `from` to `to` already,
    // for the owner to follow, telling no one.
    virtual void caption_moved(int from, int to) = 0;
    // The drag let go: the tab now at `index` was moved there from elsewhere.
    virtual void caption_reordered(int index) = 0;
    // A tab dragged past the threshold, the pointer at `screen`: what it does
    // there, where it may leave the strip (out to be torn off, or over
    // another window's strip, the owner's drag image following the pointer).
    // The strip goes on as it says.
    enum class drag_there
    {
        along,  // along the strip, as ever
        away,   // out of it: put back where it was, its place left empty
    };
    virtual drag_there caption_drag_at(int index, POINT screen) = 0;
    // Let go while away, at `screen`: the owner drops it or tears it off
    // there (the strip's drag is over, and it does nothing more; it may be
    // destroyed).
    virtual void caption_drop(int index, POINT screen) = 0;
    // The drag over otherwise: let go along the strip, or cancelled (Escape,
    // the capture lost, the app's change).
    virtual void caption_drag_ended() = 0;

protected:
    ~caption_tabs_owner() = default;
};

// What the strip draws its text with (query_message's 7): the DirectWrite
// families, sizes in device px, the titles' weight, and whether the text on
// opaque ground is ClearType. The test tools have the same layout.
struct caption_tabs_fonts
{
    wchar_t text[64];
    wchar_t glyphs[64];
    float text_size, glyph_size;
    int text_weight;
    int cleartype;
    float add_size;  // "+"'s glyphs, in device px
};

class caption_tabs final : public uia_owner
{
public:
    // A child of `parent` (a top-level window), hidden until placed.
    // `tooltip` is a tooltip control for it, which it then owns (null: none).
    caption_tabs(HWND parent, caption_tabs_owner& owner, HWND tooltip);
    ~caption_tabs();
    caption_tabs(const caption_tabs&) = delete;
    caption_tabs& operator=(const caption_tabs&) = delete;

    HWND hwnd() const { return hwnd_; }
    // The strip's height at `dpi`, the line under it included.
    static int height(UINT dpi);

    // Tabs of the same keys in another order are told to UI Automation as
    // reordered. A tab being dragged stays dragged while it's there.
    void set_tabs(std::vector<caption_tab> tabs, int current);
    void set_current(int current);
    // A tab pressed and maybe being dragged (see caption_tabs::impl::drag).
    bool dragging() const;
    // That drag's tab put back where it was, the owner told of its move
    // back (caption_moved) and that it's over (caption_drag_ended), and
    // nothing else; the mouse released.
    void cancel_drag();
    // Where the pointer holds the dragged tab, from the tab's top-left.
    POINT drag_grab() const;
    // Tab `index` as the strip draws it at rest, at its DPI, for a drag
    // image: a 32-bit top-down DIB `size` px, premultiplied, made opaque on
    // the window's colour; null if it's not in sight.
    HBITMAP tab_image(int index, SIZE& size);
    // Where a tab dropped at `p` (client) would land: 0 to count().
    int drop_index_at(POINT p) const;
    // A mark where a dropped tab would land (-1: none).
    void show_drop(int index);
    int count() const { return int(tabs_.size()); }
    int current() const { return current_; }

    // How it's drawn and hit, from the window's frame: in the caption,
    // transparent where empty (false: opaque, the window colour there, as
    // in fullscreen or high contrast), with the top `resize_band` px the
    // window's to resize by.
    void set_frame(bool transparent, int resize_band);
    // The desktop's colours, fonts, font smoothing or DPI changed: made and
    // drawn anew.
    void refresh();
    // The window moved (WM_WINDOWPOSCHANGED): onto another monitor, its
    // text is drawn again with that monitor's ClearType parameters.
    void moved();

    // The keyboard focus, as F6 gives it: on the current tab.
    void take_focus();
    bool has_focus() const { return GetFocus() == hwnd_; }

    // What's at a point of the strip (its client coordinates).
    enum class part
    {
        none,  // its empty part: the caption's
        tab,
        close,
        add,
    };
    struct hit
    {
        part what = part::none;
        int index = -1;
    };
    hit hit_test(POINT p) const;
    // A part's rectangle (client coordinates); false if there's none (a tab
    // scrolled out of sight).
    bool part_rect(part what, int index, RECT& out) const;

    // A message the test tools send it (RegisterWindowMessage
    // "rapport-caption-tabs"): wParam's low word what to tell (0 the tabs'
    // count, 1 the current one, 2 the one with the focus, 3, 4 and 5 a
    // tab's, its close button's and "+"'s rectangle into the RECT lParam
    // points to, in the process; 6 how many times its rendering parameters
    // have been made; 7 its fonts, into the caption_tabs_fonts lParam
    // points to; 8, a test hook: the first copy of the next `tab` ClearType
    // runs (from the target into the run's own bitmap) made to fail, and
    // how many have been made to fail so far returned); 9 the drop mark's
    // index (-1 none); its high word the tab.
    static UINT query_message();

    // uia_owner
    void uia_invoke(int id) override;
    void uia_select(int id) override;
    void uia_focus(int id) override;

    struct impl;

private:
    HWND hwnd_ = nullptr;
    std::unique_ptr<impl> d_;
    std::vector<caption_tab> tabs_;
    int current_ = -1;
    friend struct impl;
};

// -- the window's side --------------------------------------------------------

// Windows 11: tabs in the caption at all.
bool caption_tabs_supported();
// How far down a maximised window's strip starts: the frame's thickness,
// by which its client area's top hangs past the monitor's (the client area
// stays the window's from its top, so the compositor keeps answering for
// its caption buttons); 0 when it isn't maximised.
int caption_inset(HWND top);
// The band at the top of a window that resizes it (the frame's thickness).
int caption_resize_band(HWND top);
// Where the caption buttons are, in the window's client coordinates: from
// the compositor once it knows them, else where Windows 11 puts them.
RECT caption_buttons(HWND top);
// The frame extended down over the strip, `height` px, with Mica Alt behind
// it (DWMSBT_TABBEDWINDOW, as Explorer's; plain Mica if refused; none in
// high contrast); and back to the plain frame, with Mica (frame_top_window).
void extend_caption_frame(HWND top, int height);
void release_caption_frame(HWND top, bool dialog, bool fullscreen);
// The window's messages while its tabs are in the caption, first: the
// compositor's (the caption buttons'), the non-client area's size (the title
// bar's gone) and its hit tests. True if handled, with `result`.
bool caption_frame_message(HWND top, UINT msg, WPARAM wp, LPARAM lp, int strip_height, LRESULT& result);
// The strip's row where the strip isn't (it's a child: the window's paint
// is clipped around it), under the caption buttons: as the strip's empty
// part (transparent, or opaque) with its line along the bottom.
void paint_caption_band(HWND top, HDC dc, int strip_height, bool transparent);

// A line's width at `dpi` (the strip's, the toolbar band's): a pixel, scaled.
int caption_line_px(UINT dpi);

// -- the toolbar band ---------------------------------------------------------
//
// Explorer's address row: under the tabs in the caption a band in WinUI's
// LayerOnMicaBaseAltFillColorDefault (#B3FFFFFF; dark, #733A3A3A), as
// Explorer's measures, at its own alpha, the
// frame extended over it (extend_caption_frame; not in high contrast, where
// it's opaque) so the compositor puts Mica Alt under it, its native controls
// given alpha to match (glass_band_controls), with the stroke as an opaque
// line under it, and the current
// tab filled the same, so the tab and the band read as one surface; the
// panes under it white.
//
// Only while all three hold: Windows 11, the tabs in the caption (stage 2),
// and a base surface window. And only for a toolbar that's the window's
// first content item (the content itself, or the first shown item of a
// column) and spans the content's full width; nothing else qualifies. The
// band is then the client area's width, from the strip's bottom down to
// the toolbar's, its line the row under the toolbar; the toolbar's
// controls (wholly in it) painted on its fill wherever rapport paints
// their background (surface_color_of).
//
// No such toolbar: no band and no line, the current tab the base surface,
// as the window under it. High contrast: the system's colours, no fill
// apart from the window's, the line the divider's. Before Windows 11, in
// stage 1 (a menu bar, a dialog, fullscreen) and without a base surface,
// nothing changes.

// Set by the window's layout: the band's rectangle in its client area, line
// included (empty: none). Its windows' look follows (repainted by the caller).
void set_toolbar_band(HWND top, const RECT& band);
// The band `top` has: false if none.
bool toolbar_band(HWND top, RECT& band);
// `fill` at its alpha (opaque in high contrast); `solid`, the same drawn
// opaque, for GDI; `line`, opaque.
struct toolbar_band_colors
{
    color fill, solid, line;
};
toolbar_band_colors band_colors(HWND top);
// The band's fill and line onto `dc` (the window's client area), if it has
// one: through a 32-bit buffer, premultiplied, the fill at its alpha.
void paint_toolbar_band(HWND top, HDC dc);
// `h`, a child, is wholly within its window's band: `fill` its colour drawn
// opaque (`solid`), for GDI's brushes.
bool in_toolbar_band(HWND h, color& fill);

// The band's native controls over the extended frame (win32_band_glass.cpp):
// each child of `top` wholly in its band given correct alpha, a layer
// control (a toolbar, buttons, labels) its pixels worked out over the band's
// layer, an opaque one (a field) its pixels made opaque; called as the band
// is laid out, once the children are where they go.
void glass_band_controls(HWND top);
// While a layer control is printed onto a matte for that: the matte, which
// its background (surface_color_of, surface_brush_of, paint_toolbar_band)
// is instead of the band's.
bool glass_matte(color& matte);

}  // namespace rap::win32
