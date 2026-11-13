// The toolbar band's text fields: see win32_band_field.h.
#include "win32_band_field.h"

#include "win32_chrome_palette.h"

#include <uxtheme.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace rap::win32
{

int band_px(float logical, UINT dpi)
{
    return int(std::lround(logical * float(dpi) / 96.f));
}

int band_field_side(UINT dpi)
{
    return band_px(band_metrics::border, dpi) + band_px(band_metrics::button_inset, dpi);
}

namespace
{

using winui::argb;
using winui::over;

// WinUI's TextBox colours, by resource, as its Common_themeresources_any.xaml
// has them for the light and dark themes (#AARRGGBB), each drawn opaque over
// what's under it. The fills by state: TextControlBackground (rest),
// ...PointerOver, ...Focused and ...Disabled, which are ControlFillColorDefault,
// ControlFillColorSecondary, ControlFillColorInputActive and
// ControlFillColorDisabled; the border's ControlStrokeColorDefault (hovered,
// focused, disabled: Explorer's boxes have none at rest); the text
// TextFillColorPrimary, the placeholder TextFillColorSecondary (Tertiary
// focused: TextControlPlaceholderForegroundFocused), disabled
// TextFillColorDisabled; a button inside, hovered and pressed,
// SubtleFillColorSecondary and SubtleFillColorTertiary.
struct field_colors
{
    color rest, hover, focus, disabled;
    color stroke;
    color text, placeholder, placeholder_focused, text_disabled;
    color button_hover, button_pressed;
};
// Light: #B3FFFFFF (Explorer's boxes at rest measure #FDFDFD on the band's
// #F8F8F8: 70% white), #80F9F9F9, #FFFFFFFF, #4DF9F9F9; #0F000000; #E4000000,
// #9E000000, #72000000, #5C000000; #09000000, #06000000.
constexpr field_colors light_fields{argb(0xB3FFFFFF), argb(0x80F9F9F9), argb(0xFFFFFFFF), argb(0x4DF9F9F9),
                                    argb(0x0F000000), argb(0xE4000000), argb(0x9E000000), argb(0x72000000),
                                    argb(0x5C000000), argb(0x09000000), argb(0x06000000)};
// Dark: #0FFFFFFF, #15FFFFFF, #B31E1E1E, #0BFFFFFF; #12FFFFFF; #FFFFFFFF,
// #C5FFFFFF, #87FFFFFF, #5DFFFFFF; #0FFFFFFF, #0AFFFFFF.
constexpr field_colors dark_fields{argb(0x0FFFFFFF), argb(0x15FFFFFF), argb(0xB31E1E1E), argb(0x0BFFFFFF),
                                   argb(0x12FFFFFF), argb(0xFFFFFFFF), argb(0xC5FFFFFF), argb(0x87FFFFFF),
                                   argb(0x5DFFFFFF), argb(0x0FFFFFFF), argb(0x0AFFFFFF)};

constexpr UINT_PTR subclass_id = 0xBA4D;
constexpr UINT_PTR hover_timer = 0xBA4D;

LRESULT CALLBACK field_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data);

struct field_state
{
    bool hot = false;      // the pointer's over it, or over a button in it
    bool focused = false;  // it has the keyboard focus (WM_SETFOCUS, WM_KILLFOCUS)
    HBRUSH brush = nullptr;
    COLORREF brush_color = CLR_INVALID;
    // Under high contrast, the native frame's inside (what's left of the
    // window inside its edge), in the window's coordinates: what's filled
    // around the client area that centres the text.
    RECT inside{};
};

field_state* state_of(HWND h)
{
    DWORD_PTR data = 0;
    return h && GetWindowSubclass(h, field_subclass, subclass_id, &data) ? reinterpret_cast<field_state*>(data)
                                                                         : nullptr;
}

// WinUI's text size in the UI face, scaled with the desktop's text size as
// the message font is.
HFONT field_font(UINT dpi)
{
    return ui_font(dpi, false, false, band_metrics::font / std::max(1.f, ui_font_size_dip()));
}

// What the text's line takes: its height, ascent and capitals' height.
struct font_box
{
    int height = 0, ascent = 0, cap = 0;
};
font_box box_of(HWND h)
{
    font_box b;
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    b.height = int(tm.tmHeight);
    b.ascent = int(tm.tmAscent);
    b.cap = int(tm.tmAscent - tm.tmInternalLeading);
    if (const UINT n = GetOutlineTextMetricsW(dc, 0, nullptr))
    {
        std::vector<char> buffer(n);
        auto* otm = reinterpret_cast<OUTLINETEXTMETRICW*>(buffer.data());
        if (GetOutlineTextMetricsW(dc, n, otm) && otm->otmsCapEmHeight > 0) b.cap = int(otm->otmsCapEmHeight);
    }
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return b;
}

// Where a line `box` goes in `height`: its top, so its capitals' ink is
// centred (the text sits as Explorer's does in its boxes), kept inside
// `margin` each side where it fits.
int text_top(const font_box& box, int height, int margin)
{
    int top = (height + box.cap) / 2 - box.ascent;
    const int lowest = height - margin - box.height;
    if (lowest < margin) return std::max(0, (height - box.height) / 2);
    return std::clamp(top, margin, lowest);
}

// The field's look in its state now.
struct look
{
    color behind;  // the band under it
    color face;    // its fill, opaque
    color stroke;  // its border, opaque (when ringed)
    color accent;  // the focused line
    bool ringed = false, focused = false;
    color text, placeholder;
    const field_colors* colors = nullptr;
};
look look_of(HWND h, const field_state& st)
{
    look l;
    const field_colors& p = dark_mode() ? dark_fields : light_fields;
    l.colors = &p;
    const bool enabled = IsWindowEnabled(h) != FALSE;
    l.focused = enabled && st.focused;
    const bool hot = enabled && st.hot;
    l.behind = surface_color_of(h);
    l.behind.a = 255;
    l.face = over(!enabled ? p.disabled : l.focused ? p.focus : hot ? p.hover : p.rest, l.behind);
    l.stroke = over(p.stroke, l.behind);
    l.accent = accent_fill_default();
    l.accent.a = 255;
    l.ringed = !enabled || l.focused || hot;
    l.text = over(enabled ? p.text : p.text_disabled, l.face);
    l.placeholder = over(!enabled ? p.text_disabled : l.focused ? p.placeholder_focused : p.placeholder, l.face);
    return l;
}

HBRUSH face_brush(field_state& st, color face)
{
    const COLORREF c = color_ref(face);
    if (!st.brush || st.brush_color != c)
    {
        if (st.brush) DeleteObject(st.brush);
        st.brush = CreateSolidBrush(c);
        st.brush_color = c;
    }
    return st.brush;
}

color mix(color under, color top, float a)
{
    auto m = [a](uint8_t u, uint8_t t) { return uint8_t(std::lround(float(u) * (1 - a) + float(t) * a)); };
    return color{m(under.r, top.r), m(under.g, top.g), m(under.b, top.b), 255};
}

// The window's rectangle and its client area's, in the window's coordinates.
void frame_rects(HWND h, RECT& window, RECT& client)
{
    GetWindowRect(h, &window);
    GetClientRect(h, &client);
    MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&client), 2);
    OffsetRect(&client, -window.left, -window.top);
    OffsetRect(&window, -window.left, -window.top);
}

// The drawn frame (everything but the client area) onto `dc`, the window's
// own coordinates: on screen (WM_NCPAINT) or printed (WM_PRINT).
void paint_frame(HWND h, HDC dc)
{
    field_state* st = state_of(h);
    if (!st) return;
    RECT window, client;
    frame_rects(h, window, client);
    const int w = window.right, ht = window.bottom;
    if (w <= 0 || ht <= 0) return;
    const look l = look_of(h, *st);
    const UINT dpi = dpi_of(h);
    const float scale = float(dpi) / 96.f;
    const float radius = band_metrics::corner * scale;
    const float border = std::max(1.f, std::floor(band_metrics::border * scale));
    const float line = float(std::max(1, band_px(band_metrics::focus_line, dpi)));

    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -ht, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits)
    {
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    // How much of pixel (x, y) a rounded rectangle covers (4x4 samples).
    auto cover = [&](int x, int y, float l0, float t0, float r0, float b0, float r)
    {
        r = std::max(0.f, r);
        int hits = 0;
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
            {
                const float px = float(x) + (float(i) + .5f) / 4.f, py = float(y) + (float(j) + .5f) / 4.f;
                if (px < l0 || py < t0 || px > r0 || py > b0) continue;
                const float cx = std::clamp(px, l0 + r, r0 - r), cy = std::clamp(py, t0 + r, b0 - r);
                hits += (px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r;
            }
        return float(hits) / 16.f;
    };
    const float fw = float(w), fh = float(ht);
    const float bottom = l.focused ? line : border;
    auto* pixels = static_cast<uint32_t*>(bits);
    for (int y = 0; y < ht; ++y)
        for (int x = 0; x < w; ++x)
        {
            color c = l.behind;
            if (!(x >= client.left && x < client.right && y >= client.top && y < client.bottom))
            {
                const float outer = cover(x, y, 0, 0, fw, fh, radius);
                if (!l.ringed)
                    c = mix(c, l.face, outer);
                else if (outer > 0)
                {
                    const float inner = cover(x, y, border, border, fw - border, fh - bottom, radius - border);
                    c = mix(c, l.face, inner);
                    c = mix(c, l.focused && float(y) >= fh - line ? l.accent : l.stroke, std::max(0.f, outer - inner));
                }
            }
            pixels[size_t(y) * size_t(w) + size_t(x)] = uint32_t(c.r) << 16 | uint32_t(c.g) << 8 | c.b;
        }
    GdiFlush();
    const int saved = SaveDC(dc);
    ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
    BitBlt(dc, 0, 0, w, ht, mem, 0, 0, SRCCOPY);
    RestoreDC(dc, saved);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// Under high contrast: the native frame, then what's between its edge and
// the client area (the room that centres the text) in the field's own
// background, as the EDIT would fill it.
void paint_native_padding(HWND h, HDC dc)
{
    field_state* st = state_of(h);
    if (!st || IsRectEmpty(&st->inside)) return;
    RECT window, client;
    frame_rects(h, window, client);
    const bool plain = !(GetWindowLongPtrW(h, GWL_STYLE) & ES_READONLY) && IsWindowEnabled(h);
    auto brush = reinterpret_cast<HBRUSH>(SendMessageW(GetParent(h), plain ? WM_CTLCOLOREDIT : WM_CTLCOLORSTATIC,
                                                       reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(h)));
    if (!brush) brush = GetSysColorBrush(plain ? COLOR_WINDOW : COLOR_BTNFACE);
    const int saved = SaveDC(dc);
    ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
    FillRect(dc, &st->inside, brush);
    RestoreDC(dc, saved);
}

void paint_nonclient(HWND h, HDC dc)
{
    if (high_contrast())
        paint_native_padding(h, dc);
    else
        paint_frame(h, dc);
}

// The client area drawn here rather than by the EDIT: while it's empty (its
// placeholder in WinUI's colour; the EDIT's own cue banner is the system's
// grey) and while it's disabled (its text in TextFillColorDisabled, which
// the EDIT draws in the system's grey). Neither has a selection; the caret
// (an empty field focused) is hidden while BeginPaint's painting.
bool paints_client(HWND h)
{
    return !high_contrast() && (GetWindowTextLengthW(h) == 0 || !IsWindowEnabled(h));
}

void paint_client(HWND h, HDC dc)
{
    field_state* st = state_of(h);
    if (!st) return;
    const look l = look_of(h, *st);
    RECT c;
    GetClientRect(h, &c);
    FillRect(dc, &c, face_brush(*st, l.face));
    std::wstring text;
    color ink = l.text;
    int x = 0;
    RECT format{};
    SendMessageW(h, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&format));
    if (GetWindowTextLengthW(h) == 0)
    {
        wchar_t cue[256] = {};
        if (SendMessageW(h, EM_GETCUEBANNER, reinterpret_cast<WPARAM>(cue), 256)) text = cue;
        ink = l.placeholder;
        x = format.left;
    }
    else
    {
        text = window_text(h);
        if (const auto dot = wchar_t(SendMessageW(h, EM_GETPASSWORDCHAR, 0, 0)))
            text.assign(text.size(), dot);
        // Where the EDIT has scrolled it to.
        x = short(LOWORD(SendMessageW(h, EM_POSFROMCHAR, 0, 0)));
    }
    if (text.empty()) return;
    const int saved = SaveDC(dc);
    SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color_ref(ink));
    ExtTextOutW(dc, x, format.top, ETO_CLIPPED, &format, text.c_str(), UINT(text.size()), nullptr);
    RestoreDC(dc, saved);
}

void redraw(HWND h)
{
    RedrawWindow(h, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void frame_changed(HWND h)
{
    SetWindowPos(h, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// The native edge only under high contrast.
void apply_style(HWND h)
{
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    const LONG_PTR want = high_contrast() ? ex | WS_EX_CLIENTEDGE : ex & ~WS_EX_CLIENTEDGE;
    if (want != ex) SetWindowLongPtrW(h, GWL_EXSTYLE, want);
}

LRESULT CALLBACK field_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
{
    auto* st = reinterpret_cast<field_state*>(data);
    if (msg == band_field_query_message() && msg)
        return 1 | (high_contrast() ? 0 : 2) | (st->hot ? 4 : 0) | (st->focused ? 8 : 0);
    switch (msg)
    {
    case WM_NCCALCSIZE:
    {
        RECT* r = wp ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0] : reinterpret_cast<RECT*>(lp);
        const RECT outer = *r;
        const UINT dpi = dpi_of(h);
        LRESULT result = 0;
        int margin = 0;
        if (high_contrast())
        {
            // The native edge first, then the text centred inside it.
            result = DefSubclassProc(h, msg, wp, lp);
            st->inside = RECT{r->left - outer.left, r->top - outer.top, r->right - outer.left, r->bottom - outer.top};
        }
        else
        {
            st->inside = RECT{};
            const int side = band_field_side(dpi);
            r->left += side;
            r->right = std::max(r->left, r->right - side);
            margin = band_px(band_metrics::border, dpi);
        }
        const font_box box = box_of(h);
        const int height = int(r->bottom - r->top);
        const int top = text_top(box, height, margin);
        r->top += top;
        r->bottom = r->top + std::max(0, std::min(box.height, height - top));
        return result;
    }
    case WM_NCPAINT:
    {
        if (high_contrast()) DefSubclassProc(h, msg, wp, lp);
        if (HDC dc = GetWindowDC(h))
        {
            paint_nonclient(h, dc);
            ReleaseDC(h, dc);
        }
        return 0;
    }
    case WM_PRINT:
    {
        // A pane's copy, or a test's: the frame drawn as on screen.
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (lp & PRF_NONCLIENT) paint_nonclient(h, reinterpret_cast<HDC>(wp));
        return r;
    }
    case WM_PAINT:
        if (paints_client(h))
        {
            PAINTSTRUCT ps;
            if (HDC dc = BeginPaint(h, &ps))
            {
                paint_client(h, dc);
                EndPaint(h, &ps);
            }
            return 0;
        }
        break;
    case WM_PRINTCLIENT:
        if (paints_client(h))
        {
            paint_client(h, reinterpret_cast<HDC>(wp));
            return 0;
        }
        break;
    // The padding is the field's: a click there is in the text.
    case WM_NCHITTEST:
    {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        return r == HTNOWHERE || r == HTBORDER ? HTCLIENT : r;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    {
        st->focused = msg == WM_SETFOCUS;
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        redraw(h);
        return r;
    }
    case WM_ENABLE:
    {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        redraw(h);
        return r;
    }
    // Hovered. Over a button inside it the EDIT has a WM_MOUSELEAVE and no
    // more moves, so a short timer watches for the pointer leaving the box.
    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        KillTimer(h, hover_timer);
        if (!st->hot)
        {
            st->hot = true;
            redraw(h);
        }
        break;
    }
    case WM_MOUSELEAVE:
    case WM_TIMER:
    {
        if (msg == WM_TIMER && wp != hover_timer) break;
        POINT pt{};
        GetCursorPos(&pt);
        RECT box{};
        GetWindowRect(h, &box);
        const HWND under = WindowFromPoint(pt);
        if (PtInRect(&box, pt) && under && under != h && IsChild(h, under))
        {
            SetTimer(h, hover_timer, 50, nullptr);
            if (msg == WM_TIMER) return 0;
            break;
        }
        KillTimer(h, hover_timer);
        if (st->hot && under != h)
        {
            st->hot = false;
            redraw(h);
        }
        if (msg == WM_TIMER) return 0;
        break;
    }
    // Whatever font it's given (rapport's at a new DPI, say), WinUI's size
    // at its DPI; the text centred again in it.
    case WM_SETFONT:
    {
        const LRESULT r = DefSubclassProc(h, msg, reinterpret_cast<WPARAM>(field_font(dpi_of(h))), lp);
        frame_changed(h);
        return r;
    }
    case WM_NCDESTROY:
    {
        KillTimer(h, hover_timer);
        RemoveWindowSubclass(h, field_subclass, subclass_id);
        if (st->brush) DeleteObject(st->brush);
        delete st;
        return DefSubclassProc(h, msg, wp, lp);
    }
    default: break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

}  // namespace

UINT band_field_query_message()
{
    static const UINT m = RegisterWindowMessageW(L"rapport-band-field");
    return m;
}

void set_band_field(HWND edit, bool on)
{
    field_state* st = state_of(edit);
    if (on)
    {
        if (!st)
        {
            st = new field_state;
            st->focused = GetFocus() == edit;
            SetWindowSubclass(edit, field_subclass, subclass_id, reinterpret_cast<DWORD_PTR>(st));
        }
        apply_style(edit);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(field_font(dpi_of(edit))), FALSE);  // and its frame
    }
    else if (st)
    {
        KillTimer(edit, hover_timer);
        RemoveWindowSubclass(edit, field_subclass, subclass_id);
        if (st->brush) DeleteObject(st->brush);
        delete st;
        SetWindowLongPtrW(edit, GWL_EXSTYLE, GetWindowLongPtrW(edit, GWL_EXSTYLE) | WS_EX_CLIENTEDGE);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font(dpi_of(edit))), FALSE);
        frame_changed(edit);
    }
    redraw(edit);
}

bool is_band_field(HWND edit)
{
    return state_of(edit) != nullptr;
}

bool band_field_drawn(HWND edit)
{
    return state_of(edit) && !high_contrast();
}

void refresh_band_field(HWND edit)
{
    if (!state_of(edit)) return;
    apply_style(edit);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(field_font(dpi_of(edit))), FALSE);
    redraw(edit);
}

int band_field_height(HWND edit)
{
    const UINT dpi = dpi_of(edit);
    // WinUI's padding above and below the text (TextControlThemePadding's
    // 5 and 6) and the border's: what a larger text takes.
    const int around = band_px(5 + 6, dpi) + 2 * band_px(band_metrics::border, dpi);
    return std::max(band_px(band_metrics::field, dpi), box_of(edit).height + around);
}

bool band_field_ctlcolor(HWND control, HDC dc, LRESULT& result)
{
    field_state* st = state_of(control);
    if (!st || high_contrast()) return false;
    const look l = look_of(control, *st);
    SetTextColor(dc, color_ref(l.text));
    SetBkColor(dc, color_ref(l.face));
    result = reinterpret_cast<LRESULT>(face_brush(*st, l.face));
    return true;
}

bool band_field_button_colors(HWND edit, bool pressed, bool hot, color& fill, color& ink)
{
    field_state* st = state_of(edit);
    if (!st || high_contrast()) return false;
    const look l = look_of(edit, *st);
    fill = pressed ? over(l.colors->button_pressed, l.face) : hot ? over(l.colors->button_hover, l.face) : l.face;
    ink = l.text;
    return true;
}

}  // namespace rap::win32
