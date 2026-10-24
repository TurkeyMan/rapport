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

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
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
// The system message font at `dpi` (cached; owned by the cache).
HFONT ui_font(UINT dpi, bool bold = false, bool mono = false, float scale = 1.f);
// Average character width and line height of the UI font at `dpi`, in device px.
SIZE font_metrics(UINT dpi);
// The UI font family name and size in DIPs (for DirectWrite text).
std::wstring ui_font_family();
float ui_font_size_dip();

// Extent -> device pixels for a window.
int extent_px(HWND h, extent e, bool vertical);

// -- colours ------------------------------------------------------------------

color sys_color(int index);
color theme_color_of(theme_color role);
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
};

// Renders an icon (named icons have no theme on Windows: drawn ones only)
// to a 32-bpp premultiplied DIB of `px` device pixels square, in `ink`.
HBITMAP render_icon(const icon& icon, int px, float scale);
HBITMAP render_icon(const icon& icon, int px, float scale, color ink);
HICON render_icon_handle(const icon& icon, int px, float scale);
// Draws an icon with a Painter onto an HDC rectangle (owner-drawn buttons).
void draw_icon_on_dc(HDC dc, RECT r, const icon& icon, float scale, color ink);

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
    virtual void place(RECT r) = 0;       // position (and size) everything
    virtual void show_all(bool shown) = 0;  // show/hide HWNDs (respecting own visibility)
    // Fills the cross axis of a box (multi-line things, containers).
    virtual bool fills() const { return false; }
    // Takes spare width as a form field (text entry, lists, containers).
    virtual bool expands_in_form() const { return false; }
    // Width beyond its natural size that it would show to advantage, given
    // from a row's spare space: an ellipsized label's full text.
    virtual long wants_wider() const { return 0; }

    bool visible = true;
    int stretch = 0;
    int fixed_w = 0, fixed_h = 0;  // from common::width/height, device px
    bool is_spacer = false;

private:
    SIZE measured_{};
    unsigned measured_in_ = 0;
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
