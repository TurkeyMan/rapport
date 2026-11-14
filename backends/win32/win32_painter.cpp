#include "win32_shell_icons.h"
#include "win32_common.h"

#include "builtin_icons.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
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

namespace
{
UINT g_dpi_for_tests = 0;
}

void set_dpi_for_tests(UINT dpi)
{
    g_dpi_for_tests = dpi;
}

UINT dpi_of(HWND h)
{
    if (g_dpi_for_tests) return g_dpi_for_tests;
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

// Windows 11's UI font, Segoe UI Variable (its text optical size), on
// Windows 11 where it's installed: GDI and DirectWrite both know it as a
// family of its own. Windows 10 keeps Segoe UI, even with it installed.
bool has_variable_font()
{
    static const bool found = []
    {
        if (windows_build() < 22000) return false;
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy(lf.lfFaceName, L"Segoe UI Variable Text");
        bool any = false;
        HDC dc = GetDC(nullptr);
        EnumFontFamiliesExW(
            dc, &lf,
            [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int
            {
                *reinterpret_cast<bool*>(p) = true;
                return 0;
            },
            reinterpret_cast<LPARAM>(&any), 0);
        ReleaseDC(nullptr, dc);
        return any;
    }();
    return found;
}

// The message font, in Segoe UI Variable where it's Segoe UI (Windows 10's
// and 11's own); another, a locale's or the user's, is kept as it is.
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
    if (!_wcsicmp(ncm.lfMessageFont.lfFaceName, L"Segoe UI") && has_variable_font())
        wcscpy(ncm.lfMessageFont.lfFaceName, L"Segoe UI Variable Text");
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

namespace
{
float g_text_scale_for_tests = 0;
}

void set_text_scale_for_tests(float scale)
{
    g_text_scale_for_tests = scale;
}

float text_scale()
{
    if (g_text_scale_for_tests > 0) return g_text_scale_for_tests;
    // TextScaleFactor, in percent (100 to 225); absent at 100%.
    DWORD percent = 0, size = sizeof percent;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD,
                     nullptr, &percent, &size) != ERROR_SUCCESS)
        return 1.f;
    return float(std::clamp<DWORD>(percent, 100, 225)) / 100.f;
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

namespace
{

palette::basics dark_basics(color accent)
{
    auto c = [](theme_color r) { return *palette::windows_dark_color(r); };
    return {c(theme_color::window), c(theme_color::window_text), c(theme_color::base),
            c(theme_color::text), accent};
}

// What Windows says about colour beyond GetSysColor, read once and again
// after forget_theme_colors() (the top-level windows call it on
// WM_SETTINGCHANGE, WM_SYSCOLORCHANGE and WM_DWMCOLORIZATIONCOLORCHANGED).
struct desktop_colors
{
    bool high_contrast = false;
    bool dark = false;                             // the app mode (palette::windows_dark)
    std::optional<palette::accent_shades> shades;  // the accent's light and dark steps
    color accent;
};
std::optional<desktop_colors> g_desktop;
std::array<HBRUSH, theme_color_count> g_brushes{};

// The user's accent colour: Settings > Personalisation > Colours, which
// UISettings' UIColorType::Accent reports. Its shades from the Explorer
// key (UISettings reads the same), else the DWM's AccentColor, else the
// colorization colour; the selection colour where none is set.
desktop_colors read_desktop_colors()
{
    desktop_colors d;
    HIGHCONTRASTW hc{sizeof hc};
    d.high_contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) &&
                      (hc.dwFlags & HCF_HIGHCONTRASTON);
    d.accent = sys_color(COLOR_HIGHLIGHT);
    if (d.high_contrast) return d;  // the system colours unchanged
    DWORD light = 1, light_size = sizeof light;
    const bool known =
        RegGetValueW(
            HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &light_size) == ERROR_SUCCESS;
    d.dark = palette::windows_dark(false, palette::forced_scheme(),
                                   known ? std::optional<uint32_t>(light) : std::nullopt,
                                   windows_build());
    uint8_t bytes[32];
    DWORD size = sizeof bytes;
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, bytes, &size) == ERROR_SUCCESS)
        d.shades = palette::parse_accent_palette(bytes, size);
    DWORD word = 0;
    size = sizeof word;
    BOOL opaque = FALSE;
    if (d.shades)
        d.accent = d.shades->accent();
    else if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                          RRF_RT_REG_DWORD, nullptr, &word, &size) == ERROR_SUCCESS)
        d.accent = palette::from_abgr(word);
    else if (SUCCEEDED(DwmGetColorizationColor(&word, &opaque)))
        d.accent = palette::from_argb(word);
    return d;
}

const desktop_colors& desktop()
{
    if (!g_desktop) g_desktop = read_desktop_colors();
    return *g_desktop;
}

}  // namespace

void forget_theme_colors()
{
    g_desktop.reset();
    for (HBRUSH& b : g_brushes)
        if (b)
        {
            DeleteObject(b);
            b = nullptr;
        }
}

bool dark_mode()
{
    return desktop().dark;
}

bool high_contrast()
{
    return desktop().high_contrast;
}

color accent_fill_default()
{
    const desktop_colors& d = desktop();
    if (!d.shades) return d.accent;
    return d.dark ? d.shades->light(2) : d.shades->dark(1);
}

HBRUSH theme_brush(theme_color role)
{
    HBRUSH& b = g_brushes[size_t(role)];
    if (!b) b = CreateSolidBrush(color_ref(theme_color_of(role)));
    return b;
}

color resolve(const paint& p)
{
    return palette::resolve(p, [](theme_color r) { return theme_color_of(r); });
}

color theme_color_of(theme_color role)
{
    const desktop_colors& d = desktop();
    const bool dark = d.dark;
    if (dark)
    {
        // GetSysColor stays light in dark mode: Explorer's dark colours,
        // and the accent a step lighter, as Fluent fills with it there.
        if (std::optional<color> c = palette::windows_dark_color(role)) return *c;
        const color accent = d.shades ? d.shades->light(2) : d.accent;
        switch (role)
        {
        case theme_color::accent:
        case theme_color::highlight: return accent;
        case theme_color::highlight_text: return palette::text_on(accent);
        case theme_color::link:
            return d.shades ? d.shades->light(3) : palette::derived(role, dark_basics(accent));
        case theme_color::positive:
        case theme_color::negative:
        case theme_color::warning: return palette::fluent_status(role, true);
        default: return palette::derived(role, dark_basics(accent));
        }
    }
    switch (role)
    {
    case theme_color::window: return sys_color(COLOR_BTNFACE);
    case theme_color::window_text: return sys_color(COLOR_BTNTEXT);
    case theme_color::base: return sys_color(COLOR_WINDOW);
    case theme_color::alternate_base:
    {
        color b = sys_color(COLOR_WINDOW);
        auto f = [](uint8_t v) { return uint8_t(v > 128 ? v - 8 : v + 8); };
        return color{f(b.r), f(b.g), f(b.b), 255};
    }
    case theme_color::text: return sys_color(COLOR_WINDOWTEXT);
    case theme_color::placeholder_text: return sys_color(COLOR_GRAYTEXT);
    case theme_color::mid: return sys_color(COLOR_BTNSHADOW);
    case theme_color::neutral: return sys_color(COLOR_GRAYTEXT);
    // Selection follows the accent, as Explorer's does, with white or
    // black text, whichever reads on it; high contrast keeps its own.
    case theme_color::accent: return d.accent;
    case theme_color::highlight: return d.high_contrast ? sys_color(COLOR_HIGHLIGHT) : d.accent;
    case theme_color::highlight_text:
        return d.high_contrast ? sys_color(COLOR_HIGHLIGHTTEXT) : palette::text_on(d.accent);
    case theme_color::link:
        // Fluent's accent text: a darker step of the accent on light, a
        // lighter one on dark.
        if (d.shades) return dark ? d.shades->light(3) : d.shades->dark(2);
        return sys_color(COLOR_HOTLIGHT);
    case theme_color::positive:
    case theme_color::negative:
    case theme_color::warning: return palette::fluent_status(role, dark);
    case theme_color::header_text: return sys_color(COLOR_BTNTEXT);
    case theme_color::card: return sys_color(COLOR_WINDOW);
    // Themed tooltips ignore COLOR_INFOBK (classic yellow) and draw on the
    // window colour; high contrast's are the info colours.
    case theme_color::tooltip: return sys_color(d.high_contrast ? COLOR_INFOBK : COLOR_WINDOW);
    case theme_color::tooltip_text:
        return sys_color(d.high_contrast ? COLOR_INFOTEXT : COLOR_WINDOWTEXT);
    // Fluent's focus ring is the text's colour, not the accent's.
    case theme_color::focus: return sys_color(COLOR_WINDOWTEXT);
    case theme_color::highlight_inactive:
        if (d.high_contrast) return sys_color(COLOR_BTNFACE);
        break;
    case theme_color::highlight_inactive_text:
        if (d.high_contrast) return sys_color(COLOR_BTNTEXT);
        break;
    case theme_color::border: return sys_color(COLOR_3DSHADOW);
    case theme_color::divider: return sys_color(d.high_contrast ? COLOR_WINDOWTEXT : COLOR_3DLIGHT);
    case theme_color::disabled_text: return sys_color(COLOR_GRAYTEXT);
    default: break;
    }
    return palette::derived(role, {sys_color(COLOR_BTNFACE), sys_color(COLOR_BTNTEXT),
                                   sys_color(COLOR_WINDOW), sys_color(COLOR_WINDOWTEXT),
                                   d.high_contrast ? sys_color(COLOR_HIGHLIGHT) : d.accent});
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
    // Through the overrides, so on paper a scheme colour takes its light shade.
    return palette::resolve(p,
                            [this](theme_color role)
                            {
                                if (auto it = overrides_.find(int(role)); it != overrides_.end())
                                    return it->second;
                                return printing_ ? print_color(role) : theme_color_of(role);
                            });
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
    if (format && gdi_classic_)
        dwrite_factory()->CreateGdiCompatibleTextLayout(w.c_str(), UINT32(w.size()), format.get(), max_width,
                                                       100000, scale_, nullptr, FALSE, layout.put());
    else if (format)
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

// Prepared pixels (win32_shell_icons.h), `px` square, drawn into `dst`;
// false for none. Only reads what was prepared: safe from paint.
bool draw_pixels(ID2D1RenderTarget* rt, const std::vector<uint32_t>* pixels, D2D1_RECT_F dst, int px)
{
    if (!pixels) return false;
    com<ID2D1Bitmap> bmp;
    const D2D1_BITMAP_PROPERTIES props =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(UINT32(px), UINT32(px)), pixels->data(), UINT32(px) * 4, props, bmp.put())))
        return false;
    rt->DrawBitmap(bmp.get(), dst, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    return true;
}

// A name Windows has its own icon for drawn from it, if it was prepared at
// that size; false for any other name.
bool draw_shell_icon(ID2D1RenderTarget* rt, const std::string& name, D2D1_RECT_F dst, int px)
{
    return draw_pixels(rt, shell_icon_pixels(name, px), dst, px);
}

// A path's own icon (icon::for_path), once found at that size; false until
// then, and where there's none: the rest of the icon stands in.
bool draw_path_icon(ID2D1RenderTarget* rt, const icon& icon, D2D1_RECT_F dst, int px)
{
    return !icon.path.empty() && draw_pixels(rt, path_icon_pixels(icon.path, px), dst, px);
}

// A WIC bitmap render target of `px` square, cleared to transparent, with
// the icon drawn through a D2DPainter at `scale`.
com<IWICBitmap> icon_bitmap(const icon& icon, int px, float scale, color ink)
{
    if (!icon.draw) prepare_shell_icon(icon.name, px);  // never called from paint
    request_path_icon(icon.path, px);                   // found in the background
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
        if (draw_path_icon(rt.get(), icon, D2D1::RectF(0, 0, px / scale, px / scale), px))
        {
            // the path's own, found
        }
        else if (icon.draw)
            icon.draw(p, {px / scale, px / scale});
        else if (!draw_shell_icon(rt.get(), icon.name, D2D1::RectF(0, 0, px / scale, px / scale), px))
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
    for (const icon* i : icons)
    {
        if (!i->draw) prepare_shell_icon(i->name, px);  // never called from paint
        request_path_icon(i->path, px);                // found in the background
    }
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
            if (draw_path_icon(rt.get(), *icons[i], D2D1::RectF(0, 0, size.w, size.h), px))
            {
                // the path's own, found
            }
            else if (icons[i]->draw)
                icons[i]->draw(p, size);
            else if (!draw_shell_icon(rt.get(), icons[i]->name, D2D1::RectF(0, 0, size.w, size.h), px))
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

void want_icon(const icon& icon, int logical, UINT dpi)
{
    if (!icon.draw && !icon.name.empty()) want_shell_icon(icon.name, logical, dpi);
    want_path_icon(icon.path, logical, dpi);
}

// From paint: an icon of Windows' own is drawn only if it was prepared
// (want_icon), never asked of the shell here; a path's only once found.
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
        const int px = std::min(r.right - r.left, r.bottom - r.top);
        if (draw_path_icon(rt.get(), icon, D2D1::RectF(0, 0, px / scale, px / scale), px))
        {
            // the path's own, found
        }
        else if (icon.draw)
            icon.draw(p, size);
        else if (!draw_shell_icon(rt.get(), icon.name, D2D1::RectF(0, 0, px / scale, px / scale), px))
            builtin::draw_builtin_icon(icon.name, p, size);
    }
    rt->EndDraw();
}

}  // namespace rap::win32
