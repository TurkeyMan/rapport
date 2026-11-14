// Shared pieces of the Win32 backend: strings, DPI, fonts, theme colours,
// the Direct2D painter and images, icon rendering and the layout engine.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <commctrl.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>

#include "rapport/backend.h"
#include "theme_palette.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace rap::win32
{

// -- strings ------------------------------------------------------------------

std::wstring wide(const std::string& utf8);
std::string narrow(const std::wstring& w);
std::string narrow(const wchar_t* w, int len = -1);
std::wstring window_text(HWND h);
// "\n" -> "\r\n" for multi-line edit controls, and back.
std::wstring to_crlf(const std::wstring& s);
std::wstring from_crlf(const std::wstring& s);

// -- COM objects --------------------------------------------------------------

template <class T>
class com
{
public:
    com() = default;
    com(const com& o)
        : p_(o.p_)
    {
        if (p_) p_->AddRef();
    }
    com& operator=(const com& o)
    {
        if (o.p_) o.p_->AddRef();
        reset();
        p_ = o.p_;
        return *this;
    }
    ~com() { reset(); }
    void reset()
    {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T** put()
    {
        reset();
        return &p_;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

ID2D1Factory* d2d_factory();
// Direct2D's CPU rasteriser (WARP) costs a few milliseconds to start, the
// first time in the process: warm_d2d() pays that on a thread of its own,
// started with the backend, while the UI is built. (Small owner-drawn
// things draw on the CPU; a canvas starts there while its GPU target is
// made, which is slower still: see canvas_impl.)
void warm_d2d();
IDWriteFactory* dwrite_factory();
IWICImagingFactory* wic_factory();

// -- DPI, fonts ---------------------------------------------------------------

// Device pixels per logical pixel for a window (1 at 96 dpi).
float scale_of(HWND h);
UINT dpi_of(HWND h);
// For tests: every window taken to be at `dpi` (0: its own again), so a
// never-shown window can be sent WM_DPICHANGED for it.
void set_dpi_for_tests(UINT dpi);
// The system message font at `dpi` (cached; owned by the cache), in
// Segoe UI Variable where it's Segoe UI and that's installed.
HFONT ui_font(UINT dpi, bool bold = false, bool mono = false, float scale = 1.f);
// Average character width and line height of the UI font at `dpi`, in device px.
SIZE font_metrics(UINT dpi);
// The UI font family name and size in DIPs (for DirectWrite text).
std::wstring ui_font_family();
float ui_font_size_dip();
// The desktop's text size (Settings > Accessibility > Text size): 1 at 100%,
// up to 2.25; what the message font is enlarged by from its 9 pt.
float text_scale();
// For tests: the text size taken to be `scale` (0: the desktop's again),
// without changing the setting; the message font itself stays as it is.
void set_text_scale_for_tests(float scale);
// For tests: the message font taken to be `dip` DIPs high (0: the desktop's
// again), as a text size changes it, without changing the setting.
void set_message_font_size_for_tests(float dip);

// The desktop's message font or text size changed since the UI fonts were
// made (Settings > Accessibility > Text size, while windows are open)?
// Compared as the palette is, as most WM_SETTINGCHANGEs change neither.
// If so, the fonts are made anew from here on (ui_font, font_metrics, and
// ui_fonts_generation() moves on for the DirectWrite formats made from
// them), the old ones kept (retired) for the controls that still have
// them, and true. UI thread only.
bool renew_ui_fonts();
// The font a retired one `f` stands for, made anew: the same style, DPI
// and scale (null: `f` isn't a retired UI font).
HFONT renewed_ui_font(HFONT f);
// The retired fonts no control has (none in `in_use`) deleted; the rest
// kept for later.
void free_retired_ui_fonts(const std::set<HFONT>& in_use);
size_t retired_ui_font_count();  // for the tests
// Moves on as renew_ui_fonts() renews: a cache of formats made from the UI
// font is made again when it's moved on since.
unsigned ui_fonts_generation();

// A zoom RichEdit's EM_SETZOOM takes, nearest num/den: both parts at most
// 65535 (rounded toward 1 to fit), the ratio strictly between 1/64 and 64
// (6399/100 and 100/6399 at the ends: 64/1 and 1/64 it refuses), 0/0 for 1
// or for nothing (a part 0 or less).
std::pair<int, int> richedit_zoom(long long num, long long den);

// Extent -> device pixels for a window.
int extent_px(HWND h, extent e, bool vertical);

// -- colours ------------------------------------------------------------------

color sys_color(int index);
color theme_color_of(theme_color role);
// A paint in the desktop's colours (scheme colours by the base's darkness).
color resolve(const paint& p);
// The desktop's colours changed: read them again on next use.
void forget_theme_colors();
// Windows' dark app mode (palette::windows_dark), and high contrast.
bool dark_mode();
bool high_contrast();
// WinUI's AccentFillColorDefault: the user's accent's Dark1 shade (Light2 in
// dark mode), from its palette, else the accent itself.
color accent_fill_default();
// A solid brush of a role, kept until the colours change.
HBRUSH theme_brush(theme_color role);
// What the background of `h`'s window is painted in, wherever rapport paints
// it (its own and its containers' erase, labels' and check boxes'
// WM_CTLCOLOR*, group boxes, tab pages, toolbars, a scroll pane's off-screen
// print): base for a window_spec::base_surface window on Windows 11, else
// the window colour. And the text on it.
theme_color surface_of(HWND h);
theme_color surface_text_of(HWND h);
// The colour itself, and a brush of it (kept until the colours change): the
// surface's, or the toolbar band's fill for a control wholly in its
// window's band (win32_caption_tabs.h).
color surface_color_of(HWND h);
HBRUSH surface_brush_of(HWND h);
// Marks top-level `h` as a base surface window (base_surface_supported()
// checked by the caller).
void set_base_surface(HWND h, bool on);
bool base_surface_supported();
// The Windows build number (RtlGetVersion: GetVersionEx lies).
DWORD windows_build();
// Dark mode for native chrome (win32_theme.cpp): the app's menus, a
// top-level window's title bar, a control's visual style. Each follows
// dark_mode() when called; call again after it changes.
void apply_app_mode();
void theme_top_window(HWND h);
// Windows 11's frame for a top-level window: rounded corners (square in
// fullscreen) and Mica behind the title bar, a dialog's the transient kind;
// a popup's (a tooltip's) small rounded corners. Nothing before Windows 11.
void frame_top_window(HWND h, bool dialog, bool fullscreen = false);
void frame_popup(HWND h);
void theme_control(HWND h);
// Paints a dark menu bar (WM_UAHDRAWMENU, WM_UAHDRAWMENUITEM, and the
// line under it after WM_NCPAINT and WM_NCACTIVATE); false to leave the
// message to DefWindowProc.
bool dark_menu_bar(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
inline COLORREF color_ref(color c)
{
    return RGB(c.r, c.g, c.b);
}
inline D2D1_COLOR_F d2d_color(color c)
{
    return D2D1::ColorF(c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f);
}

// -- images -------------------------------------------------------------------

// What a Direct2D bitmap belongs to (a CPU target, or the GPU's device,
// whose bitmaps every canvas on it shares), as a key that's never reused:
// a target or device made where a lost one was mustn't find its bitmaps.
// UI thread only.
uint64_t new_resource_key();
void retire_resource_key(uint64_t key);  // its bitmaps go
bool resource_key_alive(uint64_t key);

// Pixels kept in memory (premultiplied BGRA) plus a Direct2D bitmap per
// resource key (see above) that has drawn them, created on first use.
class d2d_image : public image
{
public:
    d2d_image(int w, int h, pixel_format format, const uint8_t* pixels, size_t stride);
    int width() const override { return w_; }
    int height() const override { return h_; }
    void update(const uint8_t* pixels, size_t stride) override;

    // Key 0: a bitmap for this one use, not kept.
    com<ID2D1Bitmap> bitmap_for(ID2D1RenderTarget* rt, uint64_t key) const;
    const std::vector<uint8_t>& pixels() const { return px_; }

private:
    int w_, h_;
    pixel_format format_;
    std::vector<uint8_t> px_;
    struct cached
    {
        com<ID2D1Bitmap> bitmap;
        bool stale = false;
    };
    mutable std::map<uint64_t, cached> bitmaps_;
};

// -- painter ------------------------------------------------------------------

class d2d_painter : public painter
{
public:
    // `scale`: device px per logical px; the target's DPI is set to match so
    // all coordinates here are logical px (DIPs). `resources`: the target's
    // key for images' bitmaps (0: not kept, for a target used once).
    d2d_painter(ID2D1RenderTarget* rt, float scale, uint64_t resources = 0);
    ~d2d_painter() override;

    color resolve(const paint& p) const override;
    void line(point_f a, point_f b, const paint& p, float width, line_cap cap) override;
    void curve(point_f a, point_f c1, point_f c2, point_f b, const paint& p, float width) override;
    void circle(point_f centre, float radius, std::optional<paint> fill, std::optional<paint> stroke) override;
    void rect(rect_f r, float corner_radius, std::optional<paint> fill, std::optional<paint> stroke) override;
    void polygon(const std::vector<point_f>& points, std::optional<paint> fill, std::optional<paint> stroke,
                 float width) override;
    void arc(point_f centre, float radius, float start, float end, const paint& p, float width, line_cap cap) override;
    void image(const rap::image& img, rect_f src, rect_f dst, filter filter, float alpha) override;
    float device_scale() const override { return scale_; }
    void push_clip(rect_f r) override;
    void pop_clip() override;
    void text(point_f at, const std::string& s, const text_style& style) override;
    size_f measure(const std::string& s, const text_style& style) const override;

    // Colours that replace a theme role while painting (list selection).
    void override_color(theme_color role, color c) { overrides_[int(role)] = c; }
    // On paper: theme roles are print_color()'s, whatever the screen's theme.
    void set_printing() { printing_ = true; }
    // On a target rendering text GDI-classic (a list's rows): text laid out
    // with GDI-classic advances to match its snapped glyphs, which natural
    // ones put a fraction off each ('throu gh').
    void set_gdi_classic() { gdi_classic_ = true; }

private:
    ID2D1SolidColorBrush* brush(const paint& p);
    ID2D1StrokeStyle* stroke(line_cap cap);
    com<IDWriteTextLayout> layout(const std::string& s, const text_style& style, float max_width = 100000) const;
    void finish(ID2D1Geometry* g, const std::optional<paint>& fill, const std::optional<paint>& stroke, float width);

    ID2D1RenderTarget* rt_;
    uint64_t resources_;
    com<ID2D1Factory> factory_;  // the target's: geometry must come from the factory it was made by
    float scale_;
    com<ID2D1SolidColorBrush> brush_;
    com<ID2D1StrokeStyle> round_;
    int clips_ = 0;
    std::map<int, color> overrides_;
    bool printing_ = false;
    bool gdi_classic_ = false;
};

// Renders an icon (named icons have no theme on Windows: drawn ones only)
// to a 32-bpp premultiplied DIB of `px` device pixels square, in `ink`.
HBITMAP render_icon(const icon& icon, int px, float scale);
HBITMAP render_icon(const icon& icon, int px, float scale, color ink);
// Several icons side by side in one bitmap (px * count wide), drawn through
// one render target: for an image list, which splits it into its icons.
HBITMAP render_icons(const std::vector<const icon*>& icons, int px, float scale, color ink);
HICON render_icon_handle(const icon& icon, int px, float scale);
// Draws an icon with a Painter onto an HDC rectangle (owner-drawn buttons).
void draw_icon_on_dc(HDC dc, RECT r, const icon& icon, float scale, color ink);
// A control that paints `icon` (draw_icon_on_dc) at `logical` px: any of
// Windows' own (win32_shell_icons.h) prepared for it now and at each new DPI.
// Call when the control takes the icon, not from paint.
void want_icon(const icon& icon, int logical, UINT dpi);

// -- layout -------------------------------------------------------------------

// Bumped whenever something may have changed a size (a relayout request); a
// measurement from the same pass is reused. Without it a nested layout
// re-measures each leaf once per level, twice over.
inline unsigned g_layout_pass = 1;

// Anything that takes space in a window: a control, or a container of items.
// Sizes and rectangles are device pixels of the window's current DPI.
class item
{
public:
    virtual ~item() = default;
    // Preferred size: measure()d once per layout pass.
    SIZE natural()
    {
        if (measured_in_ != g_layout_pass)
        {
            measured_ = measure();
            measured_in_ = g_layout_pass;
        }
        return measured_;
    }
    virtual SIZE measure() = 0;
    // Height when laid out `width` across: text that wraps takes more lines
    // in less room than its natural width. Once per layout pass and width.
    long height_at(long width)
    {
        if (height_in_ != g_layout_pass || height_width_ != width)
        {
            height_ = measure_height(width);
            height_in_ = g_layout_pass;
            height_width_ = width;
        }
        return height_;
    }
    virtual long measure_height(long) { return natural().cy; }
    virtual void place(RECT r) = 0;       // position (and size) everything
    virtual void show_all(bool shown) = 0;  // show/hide HWNDs (respecting own visibility)
    // Fills the cross axis of a box (multi-line things, containers).
    virtual bool fills() const { return false; }
    // Takes spare width as a form field (text entry, lists, containers).
    virtual bool expands_in_form() const { return false; }
    // Width beyond its natural size that it would show to advantage, given
    // from a row's spare space: an ellipsized label's full text.
    virtual long wants_wider() const { return 0; }
    // In the toolbar band's toolbar, or out of it (win32_band_field.h): its
    // metrics and, for a field, its look. Set before it's measured.
    virtual void set_in_band(bool) {}

    bool visible = true;
    int stretch = 0;
    int fixed_w = 0, fixed_h = 0;  // from common::width/height, device px
    bool is_spacer = false;

private:
    SIZE measured_{};
    unsigned measured_in_ = 0;
    long height_ = 0, height_width_ = -1;
    unsigned height_in_ = 0;
};

using item_ptr = std::unique_ptr<item>;


// Distances in the window's scale: spacing between items, margins.
struct metrics
{
    int spacing, margin, dialog_margin, form_column_gap, row_gap;
    static metrics for_scale(float s)
    {
        auto px = [s](int v) { return int(v * s + 0.5f); };
        return {px(6), px(6), px(11), px(10), px(6)};
    }
};

}  // namespace rap::win32
