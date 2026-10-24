#include "win32_common.h"

#include "builtin_icons.h"

#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <thread>
#include <tuple>

namespace rap::win32
{

// -- strings ------------------------------------------------------------------

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

std::wstring window_text(HWND h)
{
    const int n = GetWindowTextLengthW(h);
    std::wstring w(size_t(n) + 1, L'\0');
    GetWindowTextW(h, w.data(), n + 1);
    w.resize(size_t(n));
    return w;
}

std::wstring to_crlf(const std::wstring& s)
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

std::wstring from_crlf(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s)
        if (c != L'\r') out += c;
    return out;
}

// -- factories ----------------------------------------------------------------

ID2D1Factory* d2d_factory()
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

void warm_d2d()
{
    std::thread(
        []
        {
            // A factory of its own (the shared one is single-threaded): what
            // it loads and starts is the process's, which is what's wanted.
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            com<ID2D1Factory> factory;
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                              reinterpret_cast<void**>(factory.put()));
            HDC dc = factory ? CreateCompatibleDC(nullptr) : nullptr;
            BITMAPINFO bi{};
            bi.bmiHeader = {sizeof(BITMAPINFOHEADER), 8, -8, 1, 32, BI_RGB};
            void* bits = nullptr;
            HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
            com<ID2D1DCRenderTarget> rt;
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (bmp && SUCCEEDED(factory->CreateDCRenderTarget(&props, rt.put())))
            {
                const HGDIOBJ old = SelectObject(dc, bmp);
                RECT r{0, 0, 8, 8};
                if (SUCCEEDED(rt->BindDC(dc, &r)))
                {
                    rt->BeginDraw();
                    rt->Clear(D2D1::ColorF(0, 0));
                    rt->EndDraw();
                }
                SelectObject(dc, old);
            }
            rt = {};
            if (bmp)
                DeleteObject(bmp);
            if (dc)
                DeleteDC(dc);
            factory = {};
            CoUninitialize();
        })
        .detach();
}

IDWriteFactory* dwrite_factory()
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

IWICImagingFactory* wic_factory()
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

// -- DPI, fonts ---------------------------------------------------------------

UINT dpi_of(HWND h)
{
    using fn_t = UINT(WINAPI*)(HWND);
    static fn_t fn = reinterpret_cast<fn_t>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (fn && h) return fn(h);
    HDC dc = GetDC(nullptr);
    const UINT dpi = UINT(GetDeviceCaps(dc, LOGPIXELSY));
    ReleaseDC(nullptr, dc);
    return dpi;
}

float scale_of(HWND h)
{
    return dpi_of(h) / 96.f;
}

namespace
{

NONCLIENTMETRICSW metrics_for(UINT dpi)
{
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    using fn_t = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static fn_t fn = reinterpret_cast<fn_t>(reinterpret_cast<void*>(
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

HFONT ui_font(UINT dpi, bool bold, bool mono, float scale)
{
    static std::map<std::tuple<UINT, bool, bool, int>, HFONT> cache;
    const auto key = std::make_tuple(dpi, bold, mono, int(scale * 100));
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    LOGFONTW lf = metrics_for(dpi).lfMessageFont;
    if (bold) lf.lfWeight = FW_BOLD;
    if (mono) wcscpy(lf.lfFaceName, L"Consolas");
    if (scale != 1.f) lf.lfHeight = LONG(std::lround(lf.lfHeight * scale));
    HFONT f = CreateFontIndirectW(&lf);
    cache[key] = f;
    return f;
}

SIZE font_metrics(UINT dpi)
{
    static std::map<UINT, SIZE> cache;
    if (auto it = cache.find(dpi); it != cache.end()) return it->second;
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, ui_font(dpi));
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

std::wstring ui_font_family()
{
    return metrics_for(96).lfMessageFont.lfFaceName;
}

float ui_font_size_dip()
{
    // lfHeight is negative character height in px at 96 dpi = DIPs.
    const LONG h = metrics_for(96).lfMessageFont.lfHeight;
    return h < 0 ? float(-h) : float(h) * 0.8f;
}

int extent_px(HWND h, extent e, bool vertical)
{
    const UINT dpi = dpi_of(h);
    switch (e.unit)
    {
    case extent::unit::auto_: return 0;
    case extent::unit::px: return int(std::lround(e.value * dpi / 96.f));
    case extent::unit::chars:
        return int(std::lround(e.value * font_metrics(dpi).cx)) + (vertical ? 0 : int(8 * dpi / 96));
    case extent::unit::lines: return int(std::lround(e.value * font_metrics(dpi).cy));
    }
    return 0;
}

// -- colours ------------------------------------------------------------------

color sys_color(int index)
{
    const COLORREF c = GetSysColor(index);
    return color{GetRValue(c), GetGValue(c), GetBValue(c), 255};
}

color theme_color_of(theme_color role)
{
    switch (role)
    {
    case theme_color::window: return sys_color(COLOR_BTNFACE);
    case theme_color::window_text: return sys_color(COLOR_BTNTEXT);
    case theme_color::base: return sys_color(COLOR_WINDOW);
    case theme_color::alternate_base:
    {
        color b = sys_color(COLOR_WINDOW);
        auto d = [](uint8_t v) { return uint8_t(v > 128 ? v - 8 : v + 8); };
        return color{d(b.r), d(b.g), d(b.b), 255};
    }
    case theme_color::text: return sys_color(COLOR_WINDOWTEXT);
    case theme_color::placeholder_text: return sys_color(COLOR_GRAYTEXT);
    case theme_color::highlight: return sys_color(COLOR_HIGHLIGHT);
    case theme_color::highlight_text: return sys_color(COLOR_HIGHLIGHTTEXT);
    case theme_color::mid: return sys_color(COLOR_BTNSHADOW);
    case theme_color::accent: return sys_color(COLOR_HOTLIGHT);
    case theme_color::link: return sys_color(COLOR_HOTLIGHT);
    case theme_color::positive: return color{0x26, 0x8a, 0x3c, 255};
    case theme_color::negative: return color{0xc0, 0x1c, 0x28, 255};
    case theme_color::neutral: return sys_color(COLOR_GRAYTEXT);
    }
    return color{0, 0, 0, 255};
}

// -- images -------------------------------------------------------------------

d2d_image::d2d_image(int w, int h, pixel_format format, const uint8_t* pixels, size_t stride)
    : w_(w)
    , h_(h)
    , format_(format)
    , px_(size_t(w) * h * 4)
{
    update(pixels, stride);
}

void d2d_image::update(const uint8_t* pixels, size_t stride)
{
    if (!pixels) return;
    for (int y = 0; y < h_; ++y)
    {
        const uint8_t* src = pixels + size_t(y) * stride;
        uint8_t* row = px_.data() + size_t(y) * w_ * 4;
        if (format_ == pixel_format::bgra8_premultiplied)
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
    for (auto& [key, c] : bitmaps_) c.stale = true;
}

namespace
{
std::set<uint64_t>& live_keys()
{
    static std::set<uint64_t> keys;
    return keys;
}
}  // namespace

uint64_t new_resource_key()
{
    static uint64_t next = 0;
    live_keys().insert(++next);
    return next;
}
void retire_resource_key(uint64_t key) { live_keys().erase(key); }
bool resource_key_alive(uint64_t key) { return live_keys().count(key) != 0; }

com<ID2D1Bitmap> d2d_image::bitmap_for(ID2D1RenderTarget* rt, uint64_t key) const
{
    auto make = [&]
    {
        com<ID2D1Bitmap> bmp;
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        rt->CreateBitmap(D2D1::SizeU(UINT32(w_), UINT32(h_)), px_.data(), UINT32(w_) * 4, props, bmp.put());
        return bmp;
    };
    if (!key) return make();
    auto it = bitmaps_.find(key);
    if (it == bitmaps_.end())
    {
        // Those of targets and devices gone (a lost GPU's) go too.
        std::erase_if(bitmaps_, [](const auto& kv) { return !resource_key_alive(kv.first); });
        it = bitmaps_.emplace(key, cached{make()}).first;
    }
    else if (it->second.stale)
    {
        it->second.bitmap->CopyFromMemory(nullptr, px_.data(), UINT32(w_) * 4);
        it->second.stale = false;
    }
    return it->second.bitmap;
}

// -- painter ------------------------------------------------------------------

d2d_painter::d2d_painter(ID2D1RenderTarget* rt, float scale, uint64_t resources)
    : rt_(rt)
    , resources_(resources)
    , scale_(scale)
{
    rt_->GetFactory(factory_.put());
    rt_->SetDpi(96.f * scale, 96.f * scale);
    rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush_.put());
}

d2d_painter::~d2d_painter()
{
    while (clips_-- > 0) rt_->PopAxisAlignedClip();
}

color d2d_painter::resolve(const paint& p) const
{
    if (const color* c = std::get_if<color>(&p)) return *c;
    const theme_color role = std::get<theme_color>(p);
    if (auto it = overrides_.find(int(role)); it != overrides_.end()) return it->second;
    return theme_color_of(role);
}

ID2D1SolidColorBrush* d2d_painter::brush(const paint& p)
{
    brush_->SetColor(d2d_color(resolve(p)));
    return brush_.get();
}

ID2D1StrokeStyle* d2d_painter::stroke(line_cap cap)
{
    if (cap == line_cap::butt) return nullptr;
    if (!round_)
    {
        const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
        factory_->CreateStrokeStyle(props, nullptr, 0, round_.put());
    }
    return round_.get();
}

void d2d_painter::line(point_f a, point_f b, const paint& p, float width, line_cap cap)
{
    rt_->DrawLine(D2D1::Point2F(a.x, a.y), D2D1::Point2F(b.x, b.y), brush(p), width, stroke(cap));
}

void d2d_painter::curve(point_f a, point_f c1, point_f c2, point_f b, const paint& p, float width)
{
    com<ID2D1PathGeometry> g;
    factory_->CreatePathGeometry(g.put());
    com<ID2D1GeometrySink> sink;
    g->Open(sink.put());
    sink->BeginFigure(D2D1::Point2F(a.x, a.y), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(c1.x, c1.y), D2D1::Point2F(c2.x, c2.y),
                                        D2D1::Point2F(b.x, b.y)));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt_->DrawGeometry(g.get(), brush(p), width);
}

void d2d_painter::circle(point_f c, float r, std::optional<paint> fill, std::optional<paint> stroke)
{
    const D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(c.x, c.y), r, r);
    if (fill) rt_->FillEllipse(e, brush(*fill));
    if (stroke) rt_->DrawEllipse(e, brush(*stroke), 1.f);
}

void d2d_painter::rect(rect_f r, float radius, std::optional<paint> fill, std::optional<paint> stroke)
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

void d2d_painter::finish(ID2D1Geometry* g, const std::optional<paint>& fill, const std::optional<paint>& stroke,
                        float width)
{
    if (fill) rt_->FillGeometry(g, brush(*fill));
    if (stroke) rt_->DrawGeometry(g, brush(*stroke), width);
}

void d2d_painter::polygon(const std::vector<point_f>& pts, std::optional<paint> fill, std::optional<paint> stroke,
                         float width)
{
    if (pts.empty()) return;
    com<ID2D1PathGeometry> g;
    factory_->CreatePathGeometry(g.put());
    com<ID2D1GeometrySink> sink;
    g->Open(sink.put());
    sink->BeginFigure(D2D1::Point2F(pts[0].x, pts[0].y), D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 1; i < pts.size(); ++i) sink->AddLine(D2D1::Point2F(pts[i].x, pts[i].y));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    finish(g.get(), fill, stroke, width);
}

void d2d_painter::arc(point_f c, float r, float start, float end, const paint& p, float width, line_cap cap)
{
    if (end - start >= 2 * 3.14159265f - 1e-4f)
    {
        rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y), r, r), brush(p), width, stroke(cap));
        return;
    }
    com<ID2D1PathGeometry> g;
    factory_->CreatePathGeometry(g.put());
    com<ID2D1GeometrySink> sink;
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

void d2d_painter::image(const rap::image& img, rect_f src, rect_f dst, filter filter, float alpha)
{
    if (src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0) return;
    const com<ID2D1Bitmap> bmp = static_cast<const d2d_image&>(img).bitmap_for(rt_, resources_);
    if (!bmp) return;
    rt_->DrawBitmap(bmp.get(), D2D1::RectF(dst.x, dst.y, dst.x + dst.w, dst.y + dst.h), alpha,
                    filter == filter::nearest ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                              : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                    D2D1::RectF(src.x, src.y, src.x + src.w, src.y + src.h));
}

void d2d_painter::push_clip(rect_f r)
{
    rt_->PushAxisAlignedClip(D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h), D2D1_ANTIALIAS_MODE_ALIASED);
    ++clips_;
}

void d2d_painter::pop_clip()
{
    if (clips_ <= 0) return;
    rt_->PopAxisAlignedClip();
    --clips_;
}

com<IDWriteTextLayout> d2d_painter::layout(const std::string& s, const text_style& style, float max_width) const
{
    // Formats are immutable and few (a handful of styles), so they're made
    // once each; creating one per call cost more than the drawing. UI thread
    // only, like all painting.
    const float size = style.size > 0 ? style.size * 96.f / 72.f : ui_font_size_dip();
    static std::map<std::tuple<float, bool, bool, bool>, com<IDWriteTextFormat>> formats;
    com<IDWriteTextFormat>& format = formats[{size, style.bold, style.italic, style.monospace}];
    if (!format)
        dwrite_factory()->CreateTextFormat(style.monospace ? L"Consolas" : ui_font_family().c_str(), nullptr,
                                          style.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                          style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, size, L"", format.put());
    com<IDWriteTextLayout> layout;
    const std::wstring w = wide(s);
    if (format)
        dwrite_factory()->CreateTextLayout(w.c_str(), UINT32(w.size()), format.get(), max_width, 100000,
                                          layout.put());
    return layout;
}

void d2d_painter::text(point_f at, const std::string& s, const text_style& style)
{
    com<IDWriteTextLayout> l = layout(s, style);
    if (!l) return;
    if (style.bg)
    {
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        rt_->FillRectangle(D2D1::RectF(at.x, at.y, at.x + m.width, at.y + m.height), brush(*style.bg));
    }
    rt_->DrawTextLayout(D2D1::Point2F(at.x, at.y), l.get(), brush(style.fg ? *style.fg : paint{theme_color::text}));
}

size_f d2d_painter::measure(const std::string& s, const text_style& style) const
{
    com<IDWriteTextLayout> l = layout(s, style);
    if (!l) return {};
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return {m.widthIncludingTrailingWhitespace, m.height};
}

// -- icons --------------------------------------------------------------------

namespace
{

// A WIC bitmap render target of `px` square, cleared to transparent, with
// the icon drawn through a D2DPainter at `scale`.
com<IWICBitmap> icon_bitmap(const icon& icon, int px, float scale, color ink)
{
    com<IWICBitmap> wic;
    if (!wic_factory() ||
        FAILED(wic_factory()->CreateBitmap(UINT(px), UINT(px), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                          wic.put())))
        return {};
    com<ID2D1RenderTarget> rt;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(d2d_factory()->CreateWicBitmapRenderTarget(wic.get(), props, rt.put()))) return {};
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    {
        d2d_painter p(rt.get(), scale);
        p.override_color(theme_color::window_text, ink);
        p.override_color(theme_color::text, ink);
        if (icon.draw)
            icon.draw(p, {px / scale, px / scale});
        else
            builtin::draw_builtin_icon(icon.name, p, {px / scale, px / scale});
    }
    rt->EndDraw();
    return wic;
}

}  // namespace

HBITMAP render_icon(const icon& icon, int px, float scale)
{
    return render_icon(icon, px, scale, theme_color_of(theme_color::window_text));
}

HBITMAP render_icons(const std::vector<const icon*>& icons, int px, float scale, color ink)
{
    const int w = px * int(icons.size());
    if (w <= 0) return nullptr;
    com<IWICBitmap> wic;
    if (!wic_factory() ||
        FAILED(wic_factory()->CreateBitmap(UINT(w), UINT(px), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, wic.put())))
        return nullptr;
    com<ID2D1RenderTarget> rt;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(d2d_factory()->CreateWicBitmapRenderTarget(wic.get(), props, rt.put()))) return nullptr;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    {
        d2d_painter p(rt.get(), scale);
        p.override_color(theme_color::window_text, ink);
        p.override_color(theme_color::text, ink);
        const size_f size{px / scale, px / scale};
        for (size_t i = 0; i < icons.size(); ++i)
        {
            // Each in its own square, kept to it.
            const float x = float(i) * px / scale;
            rt->SetTransform(D2D1::Matrix3x2F::Translation(x, 0));
            p.push_clip({0, 0, size.w, size.h});
            if (icons[i]->draw)
                icons[i]->draw(p, size);
            else
                builtin::draw_builtin_icon(icons[i]->name, p, size);
            p.pop_clip();
        }
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    rt->EndDraw();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -px;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits)
    {
        wic->CopyPixels(nullptr, UINT(w) * 4, UINT(w) * UINT(px) * 4, static_cast<BYTE*>(bits));
        // Image lists want straight alpha; Direct2D drew premultiplied.
        auto* p = static_cast<uint8_t*>(bits);
        for (int i = 0; i < w * px; ++i, p += 4)
            if (const unsigned a = p[3]; a && a < 255)
                for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::min(255u, (p[c] * 255 + a / 2) / a));
    }
    return bmp;
}

HBITMAP render_icon(const icon& icon, int px, float scale, color ink)
{
    com<IWICBitmap> wic = icon_bitmap(icon, px, scale, ink);
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

HICON render_icon_handle(const icon& icon, int px, float scale)
{
    HBITMAP color = render_icon(icon, px, scale);
    if (!color) return nullptr;
    HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return h;
}

void draw_icon_on_dc(HDC dc, RECT r, const icon& icon, float scale, color ink)
{
    if (!icon) return;
    // On the CPU: an icon is a few hundred pixels, and a GPU target's first
    // costs far more than drawing it.
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96, 96);
    com<ID2D1DCRenderTarget> rt;
    if (FAILED(d2d_factory()->CreateDCRenderTarget(&props, rt.put()))) return;
    if (FAILED(rt->BindDC(dc, &r))) return;
    rt->BeginDraw();
    {
        d2d_painter p(rt.get(), scale);
        p.override_color(theme_color::window_text, ink);
        p.override_color(theme_color::text, ink);
        const size_f size{(r.right - r.left) / scale, (r.bottom - r.top) / scale};
        if (icon.draw)
            icon.draw(p, size);
        else
            builtin::draw_builtin_icon(icon.name, p, size);
    }
    rt->EndDraw();
}

}  // namespace rap::win32
