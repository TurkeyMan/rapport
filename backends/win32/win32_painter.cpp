#include "win32_common.h"

#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

namespace anyui::win32
{

// ---------------------------------------------------------------- strings

std::wstring wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w, int len)
{
    if (!w) return {};
    if (len < 0) len = int(wcslen(w));
    if (len == 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    return s;
}

std::string narrow(const std::wstring& w)
{
    return narrow(w.data(), int(w.size()));
}

std::wstring windowText(HWND h)
{
    const int n = GetWindowTextLengthW(h);
    std::wstring w(size_t(n) + 1, L'\0');
    GetWindowTextW(h, w.data(), n + 1);
    w.resize(size_t(n));
    return w;
}

std::wstring toCrlf(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == L'\n' && (i == 0 || s[i - 1] != L'\r')) out += L'\r';
        out += s[i];
    }
    return out;
}

std::wstring fromCrlf(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s)
        if (c != L'\r') out += c;
    return out;
}

// ---------------------------------------------------------------- factories

ID2D1Factory* d2dFactory()
{
    static ID2D1Factory* f = []
    {
        ID2D1Factory* p = nullptr;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                          reinterpret_cast<void**>(&p));
        return p;
    }();
    return f;
}

IDWriteFactory* dwriteFactory()
{
    static IDWriteFactory* f = []
    {
        IDWriteFactory* p = nullptr;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&p));
        return p;
    }();
    return f;
}

IWICImagingFactory* wicFactory()
{
    static IWICImagingFactory* f = []
    {
        IWICImagingFactory* p = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                         reinterpret_cast<void**>(&p));
        return p;
    }();
    return f;
}

// ---------------------------------------------------------------- DPI, fonts

UINT dpiOf(HWND h)
{
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (fn && h) return fn(h);
    HDC dc = GetDC(nullptr);
    const UINT dpi = UINT(GetDeviceCaps(dc, LOGPIXELSY));
    ReleaseDC(nullptr, dc);
    return dpi;
}

float scaleOf(HWND h)
{
    return dpiOf(h) / 96.f;
}

namespace
{

NONCLIENTMETRICSW metricsFor(UINT dpi)
{
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    using Fn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi")));
    if (fn)
        fn(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    else
    {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, int(dpi), 96);
    }
    return ncm;
}

}  // namespace

HFONT uiFont(UINT dpi, bool bold, bool mono, float scale)
{
    static std::map<std::tuple<UINT, bool, bool, int>, HFONT> cache;
    const auto key = std::make_tuple(dpi, bold, mono, int(scale * 100));
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    LOGFONTW lf = metricsFor(dpi).lfMessageFont;
    if (bold) lf.lfWeight = FW_BOLD;
    if (mono) wcscpy(lf.lfFaceName, L"Consolas");
    if (scale != 1.f) lf.lfHeight = LONG(std::lround(lf.lfHeight * scale));
    HFONT f = CreateFontIndirectW(&lf);
    cache[key] = f;
    return f;
}

SIZE fontMetrics(UINT dpi)
{
    static std::map<UINT, SIZE> cache;
    if (auto it = cache.find(dpi); it != cache.end()) return it->second;
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, uiFont(dpi));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SIZE sz{};
    GetTextExtentPoint32W(dc, L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ", 52, &sz);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    SIZE out{(sz.cx + 26) / 52, tm.tmHeight};
    cache[dpi] = out;
    return out;
}

std::wstring uiFontFamily()
{
    return metricsFor(96).lfMessageFont.lfFaceName;
}

float uiFontSizeDip()
{
    // lfHeight is negative character height in px at 96 dpi = DIPs.
    const LONG h = metricsFor(96).lfMessageFont.lfHeight;
    return h < 0 ? float(-h) : float(h) * 0.8f;
}

int extentPx(HWND h, Extent e, bool vertical)
{
    const UINT dpi = dpiOf(h);
    switch (e.unit)
    {
    case Extent::Unit::Auto: return 0;
    case Extent::Unit::Px: return int(std::lround(e.value * dpi / 96.f));
    case Extent::Unit::Chars:
        return int(std::lround(e.value * fontMetrics(dpi).cx)) + (vertical ? 0 : int(8 * dpi / 96));
    case Extent::Unit::Lines: return int(std::lround(e.value * fontMetrics(dpi).cy));
    }
    return 0;
}

// ---------------------------------------------------------------- colours

Color sysColor(int index)
{
    const COLORREF c = GetSysColor(index);
    return Color{GetRValue(c), GetGValue(c), GetBValue(c), 255};
}

Color themeColor(ThemeColor role)
{
    switch (role)
    {
    case ThemeColor::Window: return sysColor(COLOR_BTNFACE);
    case ThemeColor::WindowText: return sysColor(COLOR_BTNTEXT);
    case ThemeColor::Base: return sysColor(COLOR_WINDOW);
    case ThemeColor::AlternateBase:
    {
        Color b = sysColor(COLOR_WINDOW);
        auto d = [](uint8_t v) { return uint8_t(v > 128 ? v - 8 : v + 8); };
        return Color{d(b.r), d(b.g), d(b.b), 255};
    }
    case ThemeColor::Text: return sysColor(COLOR_WINDOWTEXT);
    case ThemeColor::PlaceholderText: return sysColor(COLOR_GRAYTEXT);
    case ThemeColor::Highlight: return sysColor(COLOR_HIGHLIGHT);
    case ThemeColor::HighlightText: return sysColor(COLOR_HIGHLIGHTTEXT);
    case ThemeColor::Mid: return sysColor(COLOR_BTNSHADOW);
    case ThemeColor::Accent: return sysColor(COLOR_HOTLIGHT);
    case ThemeColor::Link: return sysColor(COLOR_HOTLIGHT);
    case ThemeColor::Positive: return Color{0x26, 0x8a, 0x3c, 255};
    case ThemeColor::Negative: return Color{0xc0, 0x1c, 0x28, 255};
    case ThemeColor::Neutral: return sysColor(COLOR_GRAYTEXT);
    }
    return Color{0, 0, 0, 255};
}

// ---------------------------------------------------------------- images

D2DImage::D2DImage(int w, int h, PixelFormat format, const uint8_t* pixels, size_t stride)
    : w_(w)
    , h_(h)
    , format_(format)
    , px_(size_t(w) * h * 4)
{
    update(pixels, stride);
}

void D2DImage::update(const uint8_t* pixels, size_t stride)
{
    if (!pixels) return;
    for (int y = 0; y < h_; ++y)
    {
        const uint8_t* src = pixels + size_t(y) * stride;
        uint8_t* row = px_.data() + size_t(y) * w_ * 4;
        if (format_ == PixelFormat::Bgra8Premultiplied)
        {
            std::memcpy(row, src, size_t(w_) * 4);
            continue;
        }
        for (int x = 0; x < w_; ++x)  // straight RGBA -> premultiplied BGRA
        {
            const unsigned a = src[x * 4 + 3];
            row[x * 4 + 0] = uint8_t((src[x * 4 + 2] * a + 127) / 255);
            row[x * 4 + 1] = uint8_t((src[x * 4 + 1] * a + 127) / 255);
            row[x * 4 + 2] = uint8_t((src[x * 4 + 0] * a + 127) / 255);
            row[x * 4 + 3] = uint8_t(a);
        }
    }
    for (auto& [rt, stale] : stale_) stale = true;
}

ID2D1Bitmap* D2DImage::bitmapFor(ID2D1RenderTarget* rt) const
{
    auto& bmp = bitmaps_[rt];
    if (!bmp)
    {
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        rt->CreateBitmap(D2D1::SizeU(UINT32(w_), UINT32(h_)), px_.data(), UINT32(w_) * 4, props, bmp.put());
        stale_[rt] = false;
    }
    else if (stale_[rt])
    {
        bmp->CopyFromMemory(nullptr, px_.data(), UINT32(w_) * 4);
        stale_[rt] = false;
    }
    return bmp.get();
}

// ---------------------------------------------------------------- painter

D2DPainter::D2DPainter(ID2D1RenderTarget* rt, float scale)
    : rt_(rt)
    , scale_(scale)
{
    rt_->SetDpi(96.f * scale, 96.f * scale);
    rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush_.put());
}

D2DPainter::~D2DPainter()
{
    while (clips_-- > 0) rt_->PopAxisAlignedClip();
}

Color D2DPainter::resolve(const Paint& p) const
{
    if (const Color* c = std::get_if<Color>(&p)) return *c;
    const ThemeColor role = std::get<ThemeColor>(p);
    if (auto it = overrides_.find(int(role)); it != overrides_.end()) return it->second;
    return themeColor(role);
}

ID2D1SolidColorBrush* D2DPainter::brush(const Paint& p)
{
    brush_->SetColor(d2dColor(resolve(p)));
    return brush_.get();
}

ID2D1StrokeStyle* D2DPainter::stroke(LineCap cap)
{
    if (cap == LineCap::Butt) return nullptr;
    if (!round_)
    {
        const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
        d2dFactory()->CreateStrokeStyle(props, nullptr, 0, round_.put());
    }
    return round_.get();
}

void D2DPainter::line(PointF a, PointF b, const Paint& p, float width, LineCap cap)
{
    rt_->DrawLine(D2D1::Point2F(a.x, a.y), D2D1::Point2F(b.x, b.y), brush(p), width, stroke(cap));
}

void D2DPainter::curve(PointF a, PointF c1, PointF c2, PointF b, const Paint& p, float width)
{
    Com<ID2D1PathGeometry> g;
    d2dFactory()->CreatePathGeometry(g.put());
    Com<ID2D1GeometrySink> sink;
    g->Open(sink.put());
    sink->BeginFigure(D2D1::Point2F(a.x, a.y), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(c1.x, c1.y), D2D1::Point2F(c2.x, c2.y),
                                        D2D1::Point2F(b.x, b.y)));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt_->DrawGeometry(g.get(), brush(p), width);
}

void D2DPainter::circle(PointF c, float r, std::optional<Paint> fill, std::optional<Paint> stroke)
{
    const D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(c.x, c.y), r, r);
    if (fill) rt_->FillEllipse(e, brush(*fill));
    if (stroke) rt_->DrawEllipse(e, brush(*stroke), 1.f);
}

void D2DPainter::rect(RectF r, float radius, std::optional<Paint> fill, std::optional<Paint> stroke)
{
    const D2D1_RECT_F rc = D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h);
    if (radius > 0)
    {
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rc, radius, radius);
        if (fill) rt_->FillRoundedRectangle(rr, brush(*fill));
        if (stroke) rt_->DrawRoundedRectangle(rr, brush(*stroke), 1.f);
        return;
    }
    if (fill) rt_->FillRectangle(rc, brush(*fill));
    if (stroke) rt_->DrawRectangle(rc, brush(*stroke), 1.f);
}

void D2DPainter::finish(ID2D1Geometry* g, const std::optional<Paint>& fill, const std::optional<Paint>& stroke,
                        float width)
{
    if (fill) rt_->FillGeometry(g, brush(*fill));
    if (stroke) rt_->DrawGeometry(g, brush(*stroke), width);
}

void D2DPainter::polygon(const std::vector<PointF>& pts, std::optional<Paint> fill, std::optional<Paint> stroke,
                         float width)
{
    if (pts.empty()) return;
    Com<ID2D1PathGeometry> g;
    d2dFactory()->CreatePathGeometry(g.put());
    Com<ID2D1GeometrySink> sink;
    g->Open(sink.put());
    sink->BeginFigure(D2D1::Point2F(pts[0].x, pts[0].y), D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 1; i < pts.size(); ++i) sink->AddLine(D2D1::Point2F(pts[i].x, pts[i].y));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    finish(g.get(), fill, stroke, width);
}

void D2DPainter::arc(PointF c, float r, float start, float end, const Paint& p, float width, LineCap cap)
{
    if (end - start >= 2 * 3.14159265f - 1e-4f)
    {
        rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y), r, r), brush(p), width, stroke(cap));
        return;
    }
    Com<ID2D1PathGeometry> g;
    d2dFactory()->CreatePathGeometry(g.put());
    Com<ID2D1GeometrySink> sink;
    g->Open(sink.put());
    const D2D1_POINT_2F p0 = D2D1::Point2F(c.x + r * std::cos(start), c.y + r * std::sin(start));
    const D2D1_POINT_2F p1 = D2D1::Point2F(c.x + r * std::cos(end), c.y + r * std::sin(end));
    sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(p1, D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  end - start > 3.14159265f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt_->DrawGeometry(g.get(), brush(p), width, stroke(cap));
}

void D2DPainter::image(const Image& img, RectF src, RectF dst, Filter filter, float alpha)
{
    if (src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0) return;
    ID2D1Bitmap* bmp = static_cast<const D2DImage&>(img).bitmapFor(rt_);
    if (!bmp) return;
    rt_->DrawBitmap(bmp, D2D1::RectF(dst.x, dst.y, dst.x + dst.w, dst.y + dst.h), alpha,
                    filter == Filter::Nearest ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                              : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                    D2D1::RectF(src.x, src.y, src.x + src.w, src.y + src.h));
}

void D2DPainter::pushClip(RectF r)
{
    rt_->PushAxisAlignedClip(D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h), D2D1_ANTIALIAS_MODE_ALIASED);
    ++clips_;
}

void D2DPainter::popClip()
{
    if (clips_ <= 0) return;
    rt_->PopAxisAlignedClip();
    --clips_;
}

Com<IDWriteTextLayout> D2DPainter::layout(const std::string& s, const TextStyle& style, float maxWidth) const
{
    // Formats are immutable and few (a handful of styles), so they're made
    // once each; creating one per call cost more than the drawing. UI thread
    // only, like all painting.
    const float size = style.size > 0 ? style.size * 96.f / 72.f : uiFontSizeDip();
    static std::map<std::tuple<float, bool, bool, bool>, Com<IDWriteTextFormat>> formats;
    Com<IDWriteTextFormat>& format = formats[{size, style.bold, style.italic, style.monospace}];
    if (!format)
        dwriteFactory()->CreateTextFormat(style.monospace ? L"Consolas" : uiFontFamily().c_str(), nullptr,
                                          style.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                          style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, size, L"", format.put());
    Com<IDWriteTextLayout> layout;
    const std::wstring w = wide(s);
    if (format)
        dwriteFactory()->CreateTextLayout(w.c_str(), UINT32(w.size()), format.get(), maxWidth, 100000,
                                          layout.put());
    return layout;
}

void D2DPainter::text(PointF at, const std::string& s, const TextStyle& style)
{
    Com<IDWriteTextLayout> l = layout(s, style);
    if (!l) return;
    if (style.bg)
    {
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        rt_->FillRectangle(D2D1::RectF(at.x, at.y, at.x + m.width, at.y + m.height), brush(*style.bg));
    }
    rt_->DrawTextLayout(D2D1::Point2F(at.x, at.y), l.get(), brush(style.fg ? *style.fg : Paint{ThemeColor::Text}));
}

SizeF D2DPainter::measure(const std::string& s, const TextStyle& style) const
{
    Com<IDWriteTextLayout> l = layout(s, style);
    if (!l) return {};
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return {m.widthIncludingTrailingWhitespace, m.height};
}

// ---------------------------------------------------------------- icons

namespace
{

// Windows has no icon theme: common freedesktop names get drawn icons here
// (16-unit designs scaled to the box, in the ink colour).
bool drawBuiltin(const std::string& fullName, Painter& p, SizeF s)
{
    std::string name = fullName;
    if (const size_t k = name.rfind("-symbolic"); k != std::string::npos && k + 9 == name.size()) name.resize(k);
    const Paint ink = ThemeColor::WindowText;
    const float u = std::min(s.w, s.h) / 16.f;
    auto P = [u](float x, float y) { return PointF{x * u, y * u}; };
    const float w = 1.5f * u;
    auto ln = [&](float x0, float y0, float x1, float y1) { p.line(P(x0, y0), P(x1, y1), ink, w, LineCap::Round); };
    auto box = [&](float x, float y, float bw, float bh, float r = 0) { p.rect({x * u, y * u, bw * u, bh * u}, r * u, std::nullopt, ink); };
    if (name == "go-previous") { ln(10, 3, 5, 8); ln(5, 8, 10, 13); }
    else if (name == "go-next") { ln(6, 3, 11, 8); ln(11, 8, 6, 13); }
    else if (name == "go-up") { ln(3, 10, 8, 5); ln(8, 5, 13, 10); }
    else if (name == "go-down") { ln(3, 6, 8, 11); ln(8, 11, 13, 6); }
    else if (name == "zoom-in" || name == "zoom-out")
    {
        p.circle(P(6.5f, 6.5f), 4.5f * u, std::nullopt, ink);
        ln(10, 10, 14, 14);
        ln(4.5f, 6.5f, 8.5f, 6.5f);
        if (name == "zoom-in") ln(6.5f, 4.5f, 6.5f, 8.5f);
    }
    else if (name == "zoom-original")
    {
        box(2, 3, 12, 10, 1.5f);
        ln(6, 6, 7, 5.5f); ln(7, 5.5f, 7, 10.5f); ln(10, 6, 11, 5.5f); ln(11, 5.5f, 11, 10.5f);
        p.circle(P(8.5f, 7), 0.6f * u, ink, std::nullopt);
        p.circle(P(8.5f, 9.5f), 0.6f * u, ink, std::nullopt);
    }
    else if (name == "zoom-fit-best" || name == "view-fullscreen")
    {
        const bool out = name == "view-fullscreen";
        const float a = out ? 2 : 4, b = out ? 6 : 6.5f;
        ln(a, a, b, a); ln(a, a, a, b);
        ln(16 - a, a, 16 - b, a); ln(16 - a, a, 16 - a, b);
        ln(a, 16 - a, b, 16 - a); ln(a, 16 - a, a, 16 - b);
        ln(16 - a, 16 - a, 16 - b, 16 - a); ln(16 - a, 16 - a, 16 - a, 16 - b);
    }
    else if (name == "media-playback-start") p.polygon({P(5, 3), P(13, 8), P(5, 13)}, ink, std::nullopt);
    else if (name == "media-playback-pause") { p.rect({4 * u, 3 * u, 3 * u, 10 * u}, 0, ink, std::nullopt); p.rect({9 * u, 3 * u, 3 * u, 10 * u}, 0, ink, std::nullopt); }
    else if (name == "document-open")
    {
        p.polygon({P(1.5f, 4), P(6, 4), P(7.5f, 5.5f), P(14.5f, 5.5f), P(14.5f, 13), P(1.5f, 13)}, std::nullopt, ink, w);
        ln(1.5f, 8, 14.5f, 8);
    }
    else if (name == "media-floppy" || name == "document-save")
    {
        p.polygon({P(2, 2), P(12, 2), P(14, 4), P(14, 14), P(2, 14)}, std::nullopt, ink, w);
        box(5, 2, 6, 4);
        box(4.5f, 9, 7, 5);
    }
    else if (name == "edit-cut")
    {
        p.circle(P(4.5f, 11.5f), 2.2f * u, std::nullopt, ink);
        p.circle(P(11.5f, 11.5f), 2.2f * u, std::nullopt, ink);
        ln(6, 10, 12, 2); ln(10, 10, 4, 2);
    }
    else if (name == "edit-copy") { box(2, 5, 8, 9, 1); box(6, 2, 8, 9, 1); }
    else if (name == "edit-paste") { box(3, 3, 10, 11, 1); box(6, 1.5f, 4, 3, 1); }
    else if (name == "view-grid")
    {
        p.rect({2 * u, 2 * u, 5 * u, 5 * u}, u, ink, std::nullopt);
        p.rect({9 * u, 2 * u, 5 * u, 5 * u}, u, ink, std::nullopt);
        p.rect({2 * u, 9 * u, 5 * u, 5 * u}, u, ink, std::nullopt);
        p.rect({9 * u, 9 * u, 5 * u, 5 * u}, u, ink, std::nullopt);
    }
    else if (name == "document-edit")
    {
        p.polygon({P(11, 2), P(14, 5), P(6, 13), P(2.5f, 13.5f), P(3, 10)}, std::nullopt, ink, w);
        ln(9.5f, 3.5f, 12.5f, 6.5f);
    }
    else if (name == "dialog-information")
    {
        p.circle(P(8, 8), 6.5f * u, std::nullopt, ink);
        ln(8, 7, 8, 11.5f);
        p.circle(P(8, 4.8f), 0.9f * u, ink, std::nullopt);
    }
    else if (name == "preferences-system")
    {
        for (int k = 0; k < 8; ++k)
        {
            const float a = k * 3.14159265f / 4;
            ln(8 + 4.5f * std::cos(a), 8 + 4.5f * std::sin(a), 8 + 6.5f * std::cos(a), 8 + 6.5f * std::sin(a));
        }
        p.circle(P(8, 8), 4.5f * u, std::nullopt, ink);
        p.circle(P(8, 8), 1.8f * u, std::nullopt, ink);
    }
    else if (name == "edit-find" || name == "system-search")
    {
        p.circle(P(6.5f, 6.5f), 4.5f * u, std::nullopt, ink);
        ln(10, 10, 14, 14);
    }
    else if (name == "view-refresh")
    {
        p.arc(P(8, 8), 5.5f * u, 0.3f, 5.4f, ink, w, LineCap::Round);
        p.polygon({P(14.5f, 6.5f), P(13.5f, 10.5f), P(10.5f, 8)}, ink, std::nullopt);
    }
    else if (name == "edit-delete" || name == "window-close") { ln(4, 4, 12, 12); ln(12, 4, 4, 12); }
    else if (name == "list-add") { ln(8, 3, 8, 13); ln(3, 8, 13, 8); }
    else if (name == "list-remove") ln(3, 8, 13, 8);
    else
        return false;
    return true;
}

// A WIC bitmap render target of `px` square, cleared to transparent, with
// the icon drawn through a D2DPainter at `scale`.
Com<IWICBitmap> iconBitmap(const Icon& icon, int px, float scale, Color ink)
{
    Com<IWICBitmap> wic;
    if (!wicFactory() ||
        FAILED(wicFactory()->CreateBitmap(UINT(px), UINT(px), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                          wic.put())))
        return {};
    Com<ID2D1RenderTarget> rt;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(d2dFactory()->CreateWicBitmapRenderTarget(wic.get(), props, rt.put()))) return {};
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    {
        D2DPainter p(rt.get(), scale);
        p.overrideColor(ThemeColor::WindowText, ink);
        p.overrideColor(ThemeColor::Text, ink);
        if (icon.draw)
            icon.draw(p, {px / scale, px / scale});
        else
            drawBuiltin(icon.name, p, {px / scale, px / scale});
    }
    rt->EndDraw();
    return wic;
}

}  // namespace

HBITMAP renderIcon(const Icon& icon, int px, float scale)
{
    Com<IWICBitmap> wic = iconBitmap(icon, px, scale, themeColor(ThemeColor::WindowText));
    if (!wic) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits)
    {
        wic->CopyPixels(nullptr, UINT(px) * 4, UINT(px) * UINT(px) * 4, static_cast<BYTE*>(bits));
        // Image lists and icons want straight alpha; Direct2D drew premultiplied.
        auto* p = static_cast<uint8_t*>(bits);
        for (int i = 0; i < px * px; ++i, p += 4)
            if (const unsigned a = p[3]; a && a < 255)
                for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::min(255u, (p[c] * 255 + a / 2) / a));
    }
    return bmp;
}

HICON renderIconHandle(const Icon& icon, int px, float scale)
{
    HBITMAP color = renderIcon(icon, px, scale);
    if (!color) return nullptr;
    HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return h;
}

void drawIconOnDC(HDC dc, RECT r, const Icon& icon, float scale, Color ink)
{
    if (!icon) return;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96, 96);
    Com<ID2D1DCRenderTarget> rt;
    if (FAILED(d2dFactory()->CreateDCRenderTarget(&props, rt.put()))) return;
    if (FAILED(rt->BindDC(dc, &r))) return;
    rt->BeginDraw();
    {
        D2DPainter p(rt.get(), scale);
        p.overrideColor(ThemeColor::WindowText, ink);
        p.overrideColor(ThemeColor::Text, ink);
        const SizeF size{(r.right - r.left) / scale, (r.bottom - r.top) / scale};
        if (icon.draw)
            icon.draw(p, size);
        else
            drawBuiltin(icon.name, p, size);
    }
    rt->EndDraw();
}

}  // namespace anyui::win32
