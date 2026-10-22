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
#include <d2d1.h>
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

// ---------------------------------------------------------------- strings

std::wstring wide(const std::string& utf8);
std::string narrow(const std::wstring& w);
std::string narrow(const wchar_t* w, int len = -1);
std::wstring windowText(HWND h);
// "\n" -> "\r\n" for multi-line edit controls, and back.
std::wstring toCrlf(const std::wstring& s);
std::wstring fromCrlf(const std::wstring& s);

// ---------------------------------------------------------------- COM objects

template <class T>
class Com
{
public:
    Com() = default;
    Com(const Com& o)
        : p_(o.p_)
    {
        if (p_) p_->AddRef();
    }
    Com& operator=(const Com& o)
    {
        if (o.p_) o.p_->AddRef();
        reset();
        p_ = o.p_;
        return *this;
    }
    ~Com() { reset(); }
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

ID2D1Factory* d2dFactory();
IDWriteFactory* dwriteFactory();
IWICImagingFactory* wicFactory();

// ---------------------------------------------------------------- DPI, fonts

// Device pixels per logical pixel for a window (1 at 96 dpi).
float scaleOf(HWND h);
UINT dpiOf(HWND h);
// The system message font at `dpi` (cached; owned by the cache).
HFONT uiFont(UINT dpi, bool bold = false, bool mono = false, float scale = 1.f);
// Average character width and line height of the UI font at `dpi`, in device px.
SIZE fontMetrics(UINT dpi);
// The UI font family name and size in DIPs (for DirectWrite text).
std::wstring uiFontFamily();
float uiFontSizeDip();

// Extent -> device pixels for a window.
int extentPx(HWND h, Extent e, bool vertical);

// ---------------------------------------------------------------- colours

Color sysColor(int index);
Color themeColor(ThemeColor role);
inline COLORREF colorRef(Color c)
{
    return RGB(c.r, c.g, c.b);
}
inline D2D1_COLOR_F d2dColor(Color c)
{
    return D2D1::ColorF(c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f);
}

// ---------------------------------------------------------------- images

// Pixels kept in memory (premultiplied BGRA) plus a Direct2D bitmap per
// render target that has drawn them, created on first use.
class D2DImage : public Image
{
public:
    D2DImage(int w, int h, PixelFormat format, const uint8_t* pixels, size_t stride);
    int width() const override { return w_; }
    int height() const override { return h_; }
    void update(const uint8_t* pixels, size_t stride) override;

    ID2D1Bitmap* bitmapFor(ID2D1RenderTarget* rt) const;
    const std::vector<uint8_t>& pixels() const { return px_; }

private:
    int w_, h_;
    PixelFormat format_;
    std::vector<uint8_t> px_;
    mutable std::map<ID2D1RenderTarget*, Com<ID2D1Bitmap>> bitmaps_;
    mutable std::map<ID2D1RenderTarget*, bool> stale_;
};

// ---------------------------------------------------------------- painter

class D2DPainter : public Painter
{
public:
    // `scale`: device px per logical px; the target's DPI is set to match so
    // all coordinates here are logical px (DIPs).
    D2DPainter(ID2D1RenderTarget* rt, float scale);
    ~D2DPainter() override;

    Color resolve(const Paint& p) const override;
    void line(PointF a, PointF b, const Paint& p, float width, LineCap cap) override;
    void curve(PointF a, PointF c1, PointF c2, PointF b, const Paint& p, float width) override;
    void circle(PointF centre, float radius, std::optional<Paint> fill, std::optional<Paint> stroke) override;
    void rect(RectF r, float cornerRadius, std::optional<Paint> fill, std::optional<Paint> stroke) override;
    void polygon(const std::vector<PointF>& points, std::optional<Paint> fill, std::optional<Paint> stroke,
                 float width) override;
    void arc(PointF centre, float radius, float start, float end, const Paint& p, float width, LineCap cap) override;
    void image(const Image& img, RectF src, RectF dst, Filter filter, float alpha) override;
    float deviceScale() const override { return scale_; }
    void pushClip(RectF r) override;
    void popClip() override;
    void text(PointF at, const std::string& s, const TextStyle& style) override;
    SizeF measure(const std::string& s, const TextStyle& style) const override;

    // Colours that replace a theme role while painting (list selection).
    void overrideColor(ThemeColor role, Color c) { overrides_[int(role)] = c; }

private:
    ID2D1SolidColorBrush* brush(const Paint& p);
    ID2D1StrokeStyle* stroke(LineCap cap);
    Com<IDWriteTextLayout> layout(const std::string& s, const TextStyle& style, float maxWidth = 100000) const;
    void finish(ID2D1Geometry* g, const std::optional<Paint>& fill, const std::optional<Paint>& stroke, float width);

    ID2D1RenderTarget* rt_;
    float scale_;
    Com<ID2D1SolidColorBrush> brush_;
    Com<ID2D1StrokeStyle> round_;
    int clips_ = 0;
    std::map<int, Color> overrides_;
};

// Renders an icon (named icons have no theme on Windows: drawn ones only)
// to a 32-bpp premultiplied DIB of `px` device pixels square, in `ink`.
HBITMAP renderIcon(const Icon& icon, int px, float scale);
HICON renderIconHandle(const Icon& icon, int px, float scale);
// Draws an icon with a Painter onto an HDC rectangle (owner-drawn buttons).
void drawIconOnDC(HDC dc, RECT r, const Icon& icon, float scale, Color ink);

// ---------------------------------------------------------------- layout

// Bumped whenever something may have changed a size (a relayout request); a
// measurement from the same pass is reused. Without it a nested layout
// re-measures each leaf once per level, twice over.
inline unsigned g_layoutPass = 1;

// Anything that takes space in a window: a control, or a container of items.
// Sizes and rectangles are device pixels of the window's current DPI.
class Item
{
public:
    virtual ~Item() = default;
    // Preferred size: measure()d once per layout pass.
    SIZE natural()
    {
        if (measuredIn_ != g_layoutPass)
        {
            measured_ = measure();
            measuredIn_ = g_layoutPass;
        }
        return measured_;
    }
    virtual SIZE measure() = 0;
    virtual void place(RECT r) = 0;       // position (and size) everything
    virtual void showAll(bool shown) = 0;  // show/hide HWNDs (respecting own visibility)
    // Fills the cross axis of a box (multi-line things, containers).
    virtual bool fills() const { return false; }
    // Takes spare width as a form field (text entry, lists, containers).
    virtual bool expandsInForm() const { return false; }

    bool visible = true;
    int stretch = 0;
    int fixedW = 0, fixedH = 0;  // from Common::width/height, device px
    bool isSpacer = false;

private:
    SIZE measured_{};
    unsigned measuredIn_ = 0;
};

using ItemPtr = std::unique_ptr<Item>;


// Distances in the window's scale: spacing between items, margins.
struct Metrics
{
    int spacing, margin, dialogMargin, formColumnGap, rowGap;
    static Metrics forScale(float s)
    {
        auto px = [s](int v) { return int(v * s + 0.5f); };
        return {px(6), px(6), px(11), px(10), px(6)};
    }
};

}  // namespace rap::win32
