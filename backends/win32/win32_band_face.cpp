// The toolbar band's control faces: see win32_band_face.h.
#include "win32_band_face.h"

#include "win32_caption_tabs.h"
#include "win32_chrome_palette.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace rap::win32
{

namespace
{

using winui::argb;
using winui::over;

// WinUI's Button, AppBarButton and focus visual colours, by resource, as
// Common_themeresources_any.xaml has them (#AARRGGBB): the fills
// ControlFillColorDefault (rest), ...Secondary (hovered), ...Tertiary
// (pressed) and ...Disabled; SubtleFillColorSecondary (hovered) and
// ...Tertiary (pressed); the border, ControlElevationBorderBrush (rest and
// hovered: a gradient over its first 3 px from ControlStrokeColorSecondary,
// at 1, to ControlStrokeColorDefault: up from the bottom edge in the light
// theme, down from the top in the dark) or ControlStrokeColorDefault
// (pressed and disabled: ButtonBorderBrushPressed, ...Disabled); the text
// TextFillColorPrimary, ...Secondary (pressed) and ...Disabled;
// FocusStrokeColorOuter and ...Inner; TextOnAccentFillColorPrimary and
// AccentFillColorDisabled; AccentFillColorSecondary and ...Tertiary are the
// accent at 90% and 80%. AccentButtonStyle's: TextOnAccentFillColorSecondary
// (pressed) and ...Disabled, and its border, AccentControlElevationBorderBrush
// (rest and hovered; none pressed or disabled): a gradient over its first
// 3 px from ControlStrokeColorOnAccentSecondary, at 1, to ...OnAccentDefault,
// up from the bottom edge in both themes.
struct face_colors
{
    color fill, fill_hover, fill_pressed, fill_disabled;
    color subtle_hover, subtle_pressed;
    color stroke, stroke_secondary;
    color text, text_pressed, text_disabled;
    color focus_outer, focus_inner;
    color on_accent, accent_disabled;
    color on_accent_pressed, on_accent_disabled;
    color accent_stroke, accent_stroke_secondary;
};
constexpr face_colors light_faces{
    argb(0xB3FFFFFF), argb(0x80F9F9F9), argb(0x4DF9F9F9), argb(0x4DF9F9F9), argb(0x09000000),
    argb(0x06000000), argb(0x0F000000), argb(0x29000000), argb(0xE4000000), argb(0x9E000000),
    argb(0x5C000000), argb(0xE4000000), argb(0xB3FFFFFF), argb(0xFFFFFFFF), argb(0x37000000),
    argb(0xB3FFFFFF), argb(0xFFFFFFFF), argb(0x14FFFFFF), argb(0x66000000)};
constexpr face_colors dark_faces{
    argb(0x0FFFFFFF), argb(0x15FFFFFF), argb(0x08FFFFFF), argb(0x0BFFFFFF), argb(0x0FFFFFFF),
    argb(0x0AFFFFFF), argb(0x12FFFFFF), argb(0x18FFFFFF), argb(0xFFFFFFFF), argb(0xC5FFFFFF),
    argb(0x5DFFFFFF), argb(0xFFFFFFFF), argb(0xB3000000), argb(0xFF000000), argb(0x28FFFFFF),
    argb(0x80000000), argb(0x87FFFFFF), argb(0x14FFFFFF), argb(0x23000000)};

const face_colors& faces()
{
    return dark_mode() ? dark_faces : light_faces;
}

// A straight-alpha colour `k` of the way from `a` to `b`.
color lerp(color a, color b, float k)
{
    auto m = [k](uint8_t x, uint8_t y)
    { return uint8_t(std::lround(float(x) + (float(y) - float(x)) * k)); };
    return color{m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

// How much of pixel (x, y) a rounded rectangle covers (4x4 samples).
float cover(int x, int y, float l, float t, float r, float b, float radius)
{
    radius = std::max(0.f, radius);
    if (r <= l || b <= t) return 0;
    int hits = 0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
        {
            const float px = float(x) + (float(i) + .5f) / 4.f,
                        py = float(y) + (float(j) + .5f) / 4.f;
            if (px < l || py < t || px > r || py > b) continue;
            const float cx = std::clamp(px, l + radius, r - radius),
                        cy = std::clamp(py, t + radius, b - radius);
            hits += (px - cx) * (px - cx) + (py - cy) * (py - cy) <= radius * radius;
        }
    return float(hits) / 16.f;
}

// Opaque colours mixed by weights summing to 1.
struct mix
{
    float r = 0, g = 0, b = 0;
    void add(color c, float k)
    {
        r += float(c.r) * k;
        g += float(c.g) * k;
        b += float(c.b) * k;
    }
    uint32_t pixel() const
    {
        auto v = [](float f) { return uint32_t(std::clamp(std::lround(f), 0L, 255L)); };
        return 0xFF000000u | v(r) << 16 | v(g) << 8 | v(b);
    }
};

// A 32-bit bitmap `w` by `h` filled by `shade(x, y)` and drawn at `at` on `dc`.
template <class Shade>
void blit_shaded(HDC dc, const RECT& at, Shade&& shade)
{
    const int w = at.right - at.left, h = at.bottom - at.top;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = mem ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bmp || !bits)
    {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) px[size_t(y) * size_t(w) + size_t(x)] = shade(x, y);
    GdiFlush();
    BitBlt(dc, at.left, at.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// What's round the face: the surface, opaque (the matte while printed).
color ground_of(HWND h)
{
    color c = surface_color_of(h);
    c.a = 255;
    return c;
}

// What a face's fill goes over: in the band, the band's opaque colour (as a
// band field's fill does, win32_band_field.cpp: Explorer's boxes measure
// opaque), else the surface. A subtle face (transparent at rest) instead
// goes over the ground, at its own alpha.
color under_of(HWND h, face_kind kind)
{
    if (kind == face_kind::subtle) return ground_of(h);
    return band_face_ground(h);
}

struct face_paint
{
    color fill;  // straight alpha
    bool border = false;
    bool elevated = false;  // ControlElevationBorderBrush, else ControlStrokeColorDefault
    bool on_accent = false;  // AccentControlElevationBorderBrush, for `elevated`
};
face_paint paint_of(face_kind kind, const face_state& s)
{
    const face_colors& p = faces();
    face_paint f;
    switch (kind)
    {
    case face_kind::button:
        f.fill = s.disabled  ? p.fill_disabled
                 : s.pressed ? p.fill_pressed
                 : s.hot     ? p.fill_hover
                             : p.fill;
        f.border = true;
        f.elevated = !s.disabled && !s.pressed;
        break;
    case face_kind::subtle:
        f.fill = s.disabled  ? color{0, 0, 0, 0}
                 : s.pressed ? p.subtle_pressed
                 : s.hot     ? p.subtle_hover
                             : color{0, 0, 0, 0};
        break;
    case face_kind::accent:
    case face_kind::default_button:
    {
        color a = accent_fill_default();
        a.a = s.pressed ? 204 : s.hot ? 230 : 255;
        f.fill = s.disabled ? p.accent_disabled : a;
        if (kind == face_kind::default_button)
        {
            f.border = f.elevated = f.on_accent = !s.disabled && !s.pressed;
        }
        break;
    }
    }
    return f;
}

// Fluent's icon font: Segoe Fluent Icons (Windows 11), else Segoe MDL2
// Assets; grayscale, as on glass. One per size.
HFONT glyph_font(int size)
{
    static std::map<int, HFONT> fonts;
    static const wchar_t* face = []
    {
        HDC dc = GetDC(nullptr);
        LOGFONTW want{};
        want.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(want.lfFaceName, L"Segoe Fluent Icons");
        bool found = false;
        EnumFontFamiliesExW(
            dc, &want,
            [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int
            { return *reinterpret_cast<bool*>(lp) = true, 0; },
            reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(nullptr, dc);
        return found ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }();
    HFONT& f = fonts[size];
    if (!f)
    {
        LOGFONTW lf{};
        lf.lfHeight = -size;
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = ANTIALIASED_QUALITY;
        wcscpy_s(lf.lfFaceName, face);
        f = CreateFontIndirectW(&lf);
    }
    return f;
}

// -- band_painted ------------------------------------------------------------

constexpr UINT_PTR painted_id = 0xBAFA;
constexpr UINT_PTR leave_timer = 0xBAFA;

struct painted_state
{
    band_painted* painter = nullptr;
    bool hot = false;
    bool focused = false;  // WM_SETFOCUS, WM_KILLFOCUS
};

LRESULT CALLBACK painted_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data);

painted_state* painted_of(HWND h)
{
    DWORD_PTR data = 0;
    return h && GetWindowSubclass(h, painted_proc, painted_id, &data)
               ? reinterpret_cast<painted_state*>(data)
               : nullptr;
}

void set_hot(HWND h, painted_state& st, bool hot)
{
    if (st.hot == hot) return;
    st.hot = hot;
    InvalidateRect(h, nullptr, FALSE);
}

LRESULT CALLBACK painted_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
{
    auto* st = reinterpret_cast<painted_state*>(data);
    if (msg == band_face_query_message() && msg) return 1 | (st->hot ? 2 : 0);
    if (msg == WM_NCDESTROY)
    {
        KillTimer(h, leave_timer);
        RemoveWindowSubclass(h, painted_proc, painted_id);
        delete st;
        return DefSubclassProc(h, msg, wp, lp);
    }
    if (msg == WM_WINDOWPOSCHANGED && st->painter)
    {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        st->painter->band_resized(h);
        return r;
    }
    if (!faces_on()) return DefSubclassProc(h, msg, wp, lp);
    LRESULT result = 0;
    if (st->painter && st->painter->band_message(h, msg, wp, lp, result)) return result;
    switch (msg)
    {
    case WM_ERASEBKGND: return 1;  // all in WM_PAINT
    case WM_PAINT:
        // On the band's glass, its glass paints it (printing it on its
        // mattes, WM_PRINTCLIENT, below), whichever subclass came first.
        if (glass_paints(h)) break;
        {
            PAINTSTRUCT ps;
            if (HDC dc = BeginPaint(h, &ps))
            {
                RECT c;
                GetClientRect(h, &c);
                st->painter->band_paint(h, dc, c);
                EndPaint(h, &ps);
            }
            return 0;
        }
    case WM_PRINTCLIENT:
        // On the band's glass, printed through it (its mattes), whichever
        // subclass came first: a control out of the band and back in has
        // this one over the glass's, which then prints it, its print
        // reaching here again.
        if (glass_paints(h) && !glass_printing()) break;
        if (lp & PRF_CLIENT)
        {
            RECT c;
            GetClientRect(h, &c);
            st->painter->band_paint(h, reinterpret_cast<HDC>(wp), c);
            return 0;
        }
        break;
    // Hovered. Over a child (a combo box's edit) it has a WM_MOUSELEAVE and
    // no more moves, so a short timer watches for the pointer leaving it.
    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        KillTimer(h, leave_timer);
        set_hot(h, *st, true);
        break;
    }
    case WM_MOUSELEAVE:
    case WM_TIMER:
    {
        if (msg == WM_TIMER && wp != leave_timer) break;
        POINT pt{};
        GetCursorPos(&pt);
        RECT box{};
        GetWindowRect(h, &box);
        const HWND under = WindowFromPoint(pt);
        if (PtInRect(&box, pt) && under && under != h && IsChild(h, under))
        {
            SetTimer(h, leave_timer, 50, nullptr);
            if (msg == WM_TIMER) return 0;
            break;
        }
        KillTimer(h, leave_timer);
        if (under != h) set_hot(h, *st, false);
        if (msg == WM_TIMER) return 0;
        break;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS: st->focused = msg == WM_SETFOCUS; [[fallthrough]];
    case WM_ENABLE:
    case WM_UPDATEUISTATE:
    {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    default: break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

}  // namespace

UINT band_face_query_message()
{
    static const UINT m = RegisterWindowMessageW(L"rapport-band-face");
    return m;
}

bool band_focus_cues(HWND h)
{
    return !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
}

HFONT band_text_font(HWND h, const band_density& density)
{
    return glass_font(h, band_font(dpi_of(h), density));
}

int band_text_top(HDC dc, HFONT font, int top, int height)
{
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    int cap = int(tm.tmAscent - tm.tmInternalLeading);
    GLYPHMETRICS gm{};
    const MAT2 identity{{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    if (GetGlyphOutlineW(dc, L'H', GGO_METRICS, &gm, 0, nullptr, &identity) != GDI_ERROR &&
        gm.gmptGlyphOrigin.y > 0)
        cap = int(gm.gmptGlyphOrigin.y);
    SelectObject(dc, old);
    const int line = int(tm.tmHeight);
    if (line >= height) return top + (height - line) / 2;
    return top + std::clamp((height + cap) / 2 - int(tm.tmAscent), 0, height - line);
}

void draw_band_glyph(HDC dc, const RECT& box, wchar_t glyph, int size, color ink)
{
    const int saved = SaveDC(dc);
    SelectObject(dc, glyph_font(size));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color_ref(ink));
    RECT r = box;
    DrawTextW(dc, &glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    RestoreDC(dc, saved);
}

color band_face_ground(HWND h)
{
    color band{};
    if (in_toolbar_band(h, band))
    {
        band.a = 255;
        return band;
    }
    return ground_of(h);
}

void paint_band_face(HWND h, HDC dc, const RECT& r, face_kind kind, const face_state& s)
{
    const color ground = ground_of(h);
    const face_colors& p = faces();
    const face_paint f = paint_of(kind, s);
    const color inside = over(f.fill, under_of(h, kind));
    const float scale = float(dpi_of(h)) / 96.f;
    const float w = float(r.right - r.left), ht = float(r.bottom - r.top);
    const float radius = band_metrics::corner * scale;
    const float border = std::max(1.f, std::floor(band_metrics::border * scale));
    const float outer_ring = std::max(1.f, std::round(band_metrics::focus_outer * scale));
    const float inner_ring = std::max(1.f, std::round(band_metrics::focus_inner * scale));
    const bool light = !dark_mode();
    // The elevation border's colour at row `y`: its gradient over its
    // first 3 px from the edge it starts at (the bottom, light; the top,
    // dark), ControlStrokeColorSecondary for the first of them.
    auto stroke_at = [&](int y)
    {
        if (f.on_accent)
        {
            // Up from the bottom edge, in both themes.
            const float from = (ht - (float(y) + .5f)) / scale;
            const float k = std::clamp((from - 1.f) / 2.f, 0.f, 1.f);
            return over(lerp(p.accent_stroke_secondary, p.accent_stroke, k), inside);
        }
        if (!f.elevated) return over(p.stroke, inside);
        const float from = (light ? ht - (float(y) + .5f) : float(y) + .5f) / scale;
        const float k = std::clamp((from - 1.f) / 2.f, 0.f, 1.f);
        return over(lerp(p.stroke_secondary, p.stroke, k), inside);
    };
    const color focus_outer = over(p.focus_outer, ground),
                focus_inner = over(p.focus_inner, inside);
    blit_shaded(dc, r,
                [&](int x, int y)
                {
                    const float outer = cover(x, y, 0, 0, w, ht, radius);
                    mix m;
                    m.add(ground, 1.f - outer);
                    if (s.focus_ring)
                    {
                        // The rings over the edge, the face inside them.
                        const float a = outer_ring, b = outer_ring + inner_ring;
                        const float c1 = cover(x, y, a, a, w - a, ht - a, radius - a);
                        const float c2 = cover(x, y, b, b, w - b, ht - b, radius - b);
                        m.add(focus_outer, outer - c1);
                        m.add(focus_inner, std::max(0.f, c1 - c2));
                        m.add(inside, c2);
                    }
                    else if (f.border)
                    {
                        const float in =
                            cover(x, y, border, border, w - border, ht - border, radius - border);
                        m.add(stroke_at(y), std::max(0.f, outer - in));
                        m.add(inside, in);
                    }
                    else
                        m.add(inside, outer);
                    return m.pixel();
                });
}

void paint_band_field_face(HWND h, HDC dc, const RECT& r, const band_field_look& l,
                           const RECT* button)
{
    const color ground = ground_of(h);
    const float scale = float(dpi_of(h)) / 96.f;
    const float w = float(r.right - r.left), ht = float(r.bottom - r.top);
    const float radius = band_metrics::corner * scale;
    const float border = std::max(1.f, std::floor(band_metrics::border * scale));
    const float line = float(std::max(1, band_px(band_metrics::focus_line, dpi_of(h))));
    const float bottom = l.focused ? line : border;
    RECT b{};
    if (button)
    {
        b = *button;
        OffsetRect(&b, -r.left, -r.top);
    }
    blit_shaded(dc, r,
                [&](int x, int y)
                {
                    const float outer = cover(x, y, 0, 0, w, ht, radius);
                    mix m;
                    m.add(ground, 1.f - outer);
                    const bool in_button =
                        button && x >= b.left && x < b.right && y >= b.top && y < b.bottom;
                    const color face = in_button ? l.button : l.face;
                    if (!l.ringed)
                        m.add(face, outer);
                    else
                    {
                        const float in =
                            cover(x, y, border, border, w - border, ht - bottom, radius - border);
                        m.add(face, in);
                        m.add(l.focused && float(y) >= ht - line
                                  ? l.accent
                                  : band_field_ring_at(l.stroke, l.stroke_strong, l.elevated, ht,
                                                       float(y), scale),
                              std::max(0.f, outer - in));
                    }
                    return m.pixel();
                });
}

color band_face_ink(HWND h, face_kind kind, const face_state& s)
{
    const face_colors& p = faces();
    const color inside = over(paint_of(kind, s).fill, under_of(h, kind));
    if (kind == face_kind::accent) return over(s.disabled ? p.text_disabled : p.on_accent, inside);
    if (kind == face_kind::default_button)
        return over(s.disabled ? p.on_accent_disabled : s.pressed ? p.on_accent_pressed : p.on_accent, inside);
    return over(s.disabled ? p.text_disabled : s.pressed ? p.text_pressed : p.text, inside);
}

void set_band_painted(HWND h, band_painted* painter)
{
    painted_state* st = painted_of(h);
    if (painter)
    {
        if (!st)
        {
            st = new painted_state;
            st->focused = GetFocus() == h;
            if (!SetWindowSubclass(h, painted_proc, painted_id, reinterpret_cast<DWORD_PTR>(st)))
            {
                delete st;
                return;
            }
        }
        st->painter = painter;
    }
    else if (st)
    {
        KillTimer(h, leave_timer);
        RemoveWindowSubclass(h, painted_proc, painted_id);
        delete st;
    }
    InvalidateRect(h, nullptr, TRUE);
}

bool is_band_painted(HWND h)
{
    return painted_of(h) != nullptr;
}

bool band_painted_hot(HWND h)
{
    const painted_state* st = painted_of(h);
    return st && st->hot;
}

bool band_painted_focused(HWND h)
{
    const painted_state* st = painted_of(h);
    return st && st->focused;
}

}  // namespace rap::win32
