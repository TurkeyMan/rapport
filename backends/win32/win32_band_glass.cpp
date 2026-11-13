// The toolbar band's native controls over the extended frame: see
// win32_caption_tabs.h (the toolbar band) for the band itself.
//
// The band is inside the frame extended into the client area, where the
// compositor reads every pixel's alpha (Mica Alt shows through the band's
// translucent layer). GDI writes alpha 0, so whatever a native control draws
// there with it would be a hole: premultiplied, a pixel at alpha 0 adds its
// colour to the backdrop, black text vanishing into it, a white field
// saturating. Each child window wholly in the band (in_toolbar_band) gets one
// of two treatments by its class, as a subclass, installed as the band's
// laid out (glass_band_controls) and doing nothing while the window has no
// band, or in high contrast (where the frame isn't extended over it).
//
// Layer controls, which sit on the band and show it through (a toolbar,
// buttons, labels, a slider, a progress bar, a link, a drop-down list): never
// drawn straight onto the screen. Each paint (WM_PAINT, and WM_PRINTCLIENT,
// so a printed copy is the same) prints the control twice, its background
// the band's (surface_color_of, surface_brush_of, paint_toolbar_band) black
// in the first pass and white in the second (glass_matte), and works out
// each pixel's alpha from the two: 255 less the difference, its colour the
// black pass's, premultiplied. That's exact for whatever GDI, the theme or
// an image list blends: antialiased icons, a theme part's alpha, a hover
// fill mixed with the background (that colour at that alpha). The result
// goes over the band's layer, premultiplied, through a 32-bit buffer: the
// background's the band's, the content at least as opaque, nothing at 0.
// What a control draws outside WM_PAINT (a button pressed, focused, the
// mouse coming and going) is painted again at once the same way, within the
// message that drew it. Their text is grayscale (CONTRIBUTING.md): ClearType
// can't be worked out from two passes, and it's over what the compositor
// fills in. Their font is a grayscale copy of the one they're given.
//
// Opaque controls, fields kept native and as they look (an edit, a combo
// box with one, a list, anything inside one of those, and any class not
// known here): they draw outside WM_PAINT all the time (typing, selecting,
// the caret blinking), so they're let draw, and after any message they may
// have drawn on their client area's pixels are read back and written again
// at alpha 255, the caret's as drawn; their border (the theme's, partly
// translucent) is printed through the two passes, as a layer control. Their
// text stays ClearType: the ground is opaque and their own.
//
// Tooltips are top-level windows with their own surface: nothing to do.
#include "win32_caption_tabs.h"

#include <uxtheme.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace rap::win32
{

namespace
{

constexpr UINT_PTR glass_id = 0x676c;  // the subclass's
constexpr UINT wm_systimer = 0x0118;   // the caret's blink

enum class kind
{
    layer,
    opaque,
};

struct glass
{
    kind k = kind::opaque;
    bool fonts = false;     // its font shown grayscale while it's on glass
    HFONT given = nullptr;  // the font it was given
    bool hot = false;       // the mouse over it (a button's hover)
    // Its lifetime. A message passed on (DefSubclassProc, and any message
    // sent) can reach the app's code, which may destroy the control: then
    // WM_NCDESTROY comes inside that call and marks it gone, and it's freed
    // once the last glass_proc on the stack for it returns (`busy`), never
    // under one. Nothing of it, nor the window, is touched after a call that
    // could have destroyed it unless `alive` says it's still there.
    bool alive = true;
    int busy = 0;
};

// glass_proc's hold on a control's state for the length of one message.
class hold
{
public:
    explicit hold(glass* g) : g_(g) { ++g_->busy; }
    ~hold()
    {
        if (--g_->busy == 0 && !g_->alive) delete g_;
    }
    hold(const hold&) = delete;
    hold& operator=(const hold&) = delete;

private:
    glass* g_;
};

// While a layer control prints onto a matte: the matte; and the depth of
// those prints, during which its messages go straight through.
thread_local const color* t_matte = nullptr;
thread_local int t_printing = 0;

bool class_is(HWND h, const wchar_t* name)
{
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return !_wcsicmp(cls, name);
}

kind kind_of(HWND h, HWND top)
{
    // Inside another control (an edit's buttons, a combo box's edit): on
    // that control's opaque ground.
    if (GetParent(h) != top) return kind::opaque;
    if (class_is(h, L"ComboBox"))
        return (GetWindowLongPtrW(h, GWL_STYLE) & 3) == CBS_DROPDOWNLIST ? kind::layer : kind::opaque;
    for (const wchar_t* c : {L"ToolbarWindow32", L"Button", L"Static", L"msctls_trackbar32", L"msctls_progress32",
                             L"SysLink"})
        if (class_is(h, c)) return kind::layer;
    return kind::opaque;
}

// On glass now: in its window's band, and not in high contrast.
bool on_glass(HWND h)
{
    color fill{};
    return !high_contrast() && in_toolbar_band(h, fill);
}

// A font's grayscale copy (ANTIALIASED_QUALITY), one per face, size and
// style, kept for the process's life (one per DPI, in practice).
HFONT grayscale(HFONT f)
{
    struct made
    {
        LOGFONTW from;
        HFONT font;
    };
    static std::vector<made> fonts;
    LOGFONTW lf{};
    if (!f || !GetObjectW(f, sizeof lf, &lf) || lf.lfQuality == ANTIALIASED_QUALITY) return f;
    for (const made& m : fonts)
        if (!std::memcmp(&m.from, &lf, sizeof lf)) return m.font;
    LOGFONTW grey = lf;
    grey.lfQuality = ANTIALIASED_QUALITY;
    HFONT g = CreateFontIndirectW(&grey);
    if (!g) return f;
    fonts.push_back({lf, g});
    return g;
}

// A top-down 32-bit bitmap selected into a memory DC.
struct dib
{
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* bits = nullptr;
    dib(HDC like, int w, int h, uint32_t fill)
    {
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* p = nullptr;
        bmp = CreateDIBSection(like, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
        dc = bmp ? CreateCompatibleDC(like) : nullptr;
        if (!dc) return;
        old = SelectObject(dc, bmp);
        bits = static_cast<uint32_t*>(p);
        std::fill_n(bits, size_t(w) * h, fill);
    }
    ~dib()
    {
        if (dc)
        {
            SelectObject(dc, old);
            DeleteDC(dc);
        }
        if (bmp) DeleteObject(bmp);
    }
    dib(const dib&) = delete;
    dib& operator=(const dib&) = delete;
    explicit operator bool() const { return bits != nullptr; }
};

uint32_t premultiplied(color c)
{
    auto m = [&](uint8_t v) { return uint32_t((v * c.a + 127) / 255); };
    return uint32_t(c.a) << 24 | m(c.r) << 16 | m(c.g) << 8 | m(c.b);
}

// What `draw(into, matte)` draws of `h` into a bitmap `area` big (from 0,0)
// onto `target`: drawn on black and on white (`matte`, the band's colour
// meanwhile: glass_matte), its alpha worked out, over the band's layer.
// False if it couldn't be (no bitmaps, no buffer), for the caller to let it
// draw as it would.
template <class Draw>
bool paint_glass(HWND h, HDC target, const RECT& area, Draw&& draw)
{
    const RECT& c = area;
    const int w = c.right, ht = c.bottom;
    if (w <= 0 || ht <= 0) return true;
    HWND top = GetAncestor(h, GA_ROOT);
    const uint32_t layer = premultiplied(band_colors(top).fill);
    dib black(target, w, ht, 0x00000000), white(target, w, ht, 0x00FFFFFF);
    if (!black || !white) return false;
    static const color mattes[2] = {color{0, 0, 0, 255}, color{255, 255, 255, 255}};
    ++t_printing;
    for (int i = 0; i < 2 && IsWindow(h); ++i)
    {
        t_matte = &mattes[i];
        draw(i ? white.dc : black.dc, mattes[i]);
    }
    t_matte = nullptr;
    --t_printing;
    // Destroyed meanwhile (a print reaching the app): nothing more to draw.
    if (!IsWindow(h)) return true;
    GdiFlush();
    BP_PAINTPARAMS params{sizeof params, 0, nullptr, nullptr};
    HDC mem = nullptr;
    HPAINTBUFFER buffer = BeginBufferedPaint(target, &c, BPBF_TOPDOWNDIB, &params, &mem);
    if (!buffer) return false;
    RGBQUAD* bits = nullptr;
    int row = 0;
    if (FAILED(GetBufferedPaintBits(buffer, &bits, &row)))
    {
        EndBufferedPaint(buffer, FALSE);
        return false;
    }
    const int la = int(layer >> 24);
    for (int y = 0; y < ht; ++y)
    {
        auto* out = reinterpret_cast<uint32_t*>(bits + size_t(y) * row);
        const uint32_t* b = black.bits + size_t(y) * w;
        const uint32_t* wh = white.bits + size_t(y) * w;
        for (int x = 0; x < w; ++x)
        {
            int sum = 0;
            int k[3];
            for (int i = 0; i < 3; ++i)
            {
                const int shift = i * 8;
                k[i] = int((b[x] >> shift) & 0xff);
                sum += std::clamp(255 - (int((wh[x] >> shift) & 0xff) - k[i]), 0, 255);
            }
            const int a = (sum + 1) / 3;
            uint32_t px = uint32_t(a + (la * (255 - a) + 127) / 255) << 24;
            for (int i = 0; i < 3; ++i)
            {
                const int shift = i * 8;
                const int l = int((layer >> shift) & 0xff);
                px |= uint32_t(std::min(255, std::min(k[i], a) + (l * (255 - a) + 127) / 255)) << shift;
            }
            out[x] = px;
        }
    }
    EndBufferedPaint(buffer, TRUE);
    return true;
}

// A layer control's client area onto `target`, as on glass.
bool paint_layer(HWND h, HDC target)
{
    RECT c;
    GetClientRect(h, &c);
    return paint_glass(h, target, c,
                       [h](HDC into, const color&)
                       { SendMessageW(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(into), PRF_CLIENT | PRF_ERASEBKGND); });
}

// A layer control drawn again now (two passes), after it may have drawn
// itself straight onto the screen.
void repaint_layer(HWND h)
{
    if (IsWindowVisible(h)) RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
}

// An opaque control made opaque where it drew, within its visible region
// (not over its children): its client area read back and written again at
// alpha 255, the caret's pixels as drawn. Its border (a field's edge) goes
// through the two passes, as a layer control's client area: the theme draws
// it partly translucent, its bottom and right edges blended over whatever's
// there, which the band's layer should show through, not black.
void make_opaque(HWND h)
{
    if (!IsWindowVisible(h)) return;
    RECT c;
    GetClientRect(h, &c);
    BP_PAINTPARAMS params{sizeof params, 0, nullptr, nullptr};
    HDC mem = nullptr;
    if (!IsRectEmpty(&c))
        if (HDC dc = GetDC(h))
        {
            if (HPAINTBUFFER buffer = BeginBufferedPaint(dc, &c, BPBF_TOPDOWNDIB, &params, &mem))
            {
                BitBlt(mem, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
                BufferedPaintSetAlpha(buffer, nullptr, 255);
                EndBufferedPaint(buffer, TRUE);
            }
            ReleaseDC(h, dc);
        }
    // The border, if it has one: where the window's rectangle is past the
    // client area's.
    RECT w;
    GetWindowRect(h, &w);
    POINT origin{0, 0};
    ClientToScreen(h, &origin);
    const RECT client_in_window{origin.x - w.left, origin.y - w.top, origin.x - w.left + c.right,
                                origin.y - w.top + c.bottom};
    OffsetRect(&w, -w.left, -w.top);
    if (EqualRect(&w, &client_in_window) || IsRectEmpty(&w)) return;
    HDC dc = GetWindowDC(h);
    if (!dc) return;
    ExcludeClipRect(dc, client_in_window.left, client_in_window.top, client_in_window.right,
                    client_in_window.bottom);
    // The theme's own border (WM_PRINT's would be the classic sunken edge):
    // painted on the screen over each matte in turn, read back, and then
    // what's worked out from the two written over it.
    paint_glass(h, dc, w,
                [&](HDC into, const color& matte)
                {
                    FillRect(dc, &w, static_cast<HBRUSH>(GetStockObject(matte.r ? WHITE_BRUSH : BLACK_BRUSH)));
                    SendMessageW(h, WM_NCPAINT, 1, 0);
                    GdiFlush();
                    BitBlt(into, 0, 0, w.right, w.bottom, dc, 0, 0, SRCCOPY);
                });
    ReleaseDC(h, dc);
}

// A message a control may draw on outside WM_PAINT (or, for an opaque one,
// in it).
bool may_draw(UINT msg)
{
    switch (msg)
    {
    case WM_PAINT:
    case WM_NCPAINT:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_SYSCHAR:
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_MOUSELEAVE:
    case WM_CAPTURECHANGED:
    case WM_TIMER:
    case wm_systimer:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_SETTEXT:
    case WM_SETFONT:
    case WM_CUT:
    case WM_PASTE:
    case WM_CLEAR:
    case WM_UNDO:
    case WM_HSCROLL:
    case WM_VSCROLL:
    case WM_STYLECHANGED:
    case WM_THEMECHANGED:
    case WM_UPDATEUISTATE:
    case BM_SETSTATE:
    case BM_SETCHECK:
    case BM_CLICK:
    case BM_SETSTYLE:
    case BM_SETIMAGE: return true;
    default:
        // Edit's EM_ messages (EM_SETSEL, EM_REPLACESEL, EM_UNDO,
        // EM_SETREADONLY...), and the combo box's CB_ ones.
        return (msg >= EM_GETSEL && msg <= EM_GETIMESTATUS) || (msg >= CB_GETEDITSEL && msg <= CB_MSGMAX) ||
               msg == EM_SETCUEBANNER;
    }
}

LRESULT CALLBACK glass_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
{
    auto* g = reinterpret_cast<glass*>(data);
    const hold held(g);  // (freed after this returns, if destroyed meanwhile)
    if (msg == WM_NCDESTROY)
    {
        g->alive = false;
        RemoveWindowSubclass(h, glass_proc, glass_id);
        return DefSubclassProc(h, msg, wp, lp);
    }
    // Its font: shown grayscale on glass (a layer control's text), as given
    // otherwise.
    if (msg == WM_SETFONT && g->fonts)
    {
        g->given = reinterpret_cast<HFONT>(wp);
        if (on_glass(h)) wp = reinterpret_cast<WPARAM>(grayscale(g->given));
    }
    if (t_printing > 0 || !on_glass(h)) return DefSubclassProc(h, msg, wp, lp);
    if (g->k == kind::layer)
    {
        switch (msg)
        {
        case WM_ERASEBKGND: return 1;  // all in WM_PAINT
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            // (Printing sends the control messages, an owner-drawn one's
            // reaching the app: nothing more if that destroyed it.)
            const bool done = paint_layer(h, dc);
            if (!g->alive) return 0;
            if (!done)
            {
                // No bitmaps or buffer: drawn as it would be, rather than not.
                ++t_printing;
                SendMessageW(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
                --t_printing;
                if (!g->alive) return 0;
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            if ((lp & PRF_CLIENT) && paint_layer(h, reinterpret_cast<HDC>(wp))) return 0;
            break;
        case WM_MOUSEMOVE:
        {
            // A button's hover, drawn as the mouse comes: again, on glass.
            // Its state read and set first: the move may reach the app (a
            // slider's on_changed) and destroy it.
            const bool came = !g->hot;
            g->hot = true;
            if (came)
            {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
            }
            const LRESULT r = DefSubclassProc(h, msg, wp, lp);
            if (came && g->alive) repaint_layer(h);
            return r;
        }
        default: break;
        }
        if (msg == WM_MOUSELEAVE) g->hot = false;
        if (msg == WM_NCPAINT || !may_draw(msg)) return DefSubclassProc(h, msg, wp, lp);
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (g->alive) repaint_layer(h);
        return r;
    }
    // Opaque: let it draw, then its pixels opaque.
    if (msg == WM_PRINTCLIENT && (lp & PRF_CLIENT))
    {
        RECT c;
        GetClientRect(h, &c);
        BP_PAINTPARAMS params{sizeof params, BPPF_ERASE, nullptr, nullptr};
        HDC mem = nullptr;
        if (HPAINTBUFFER buffer = BeginBufferedPaint(reinterpret_cast<HDC>(wp), &c, BPBF_TOPDOWNDIB, &params, &mem))
        {
            ++t_printing;
            const LRESULT r = SendMessageW(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), lp);
            --t_printing;
            BufferedPaintSetAlpha(buffer, nullptr, 255);
            EndBufferedPaint(buffer, TRUE);  // (the target's the caller's, the buffer ours)
            return r;
        }
    }
    // Everything of its state read and set before the message is passed on
    // (it may reach the app and destroy the control); after, only `alive`.
    bool drew = may_draw(msg);
    if (msg == WM_MOUSEMOVE)
    {
        // Selecting by dragging, or the mouse just come (a hover look).
        drew = (wp & MK_LBUTTON) || !g->hot;
        if (!g->hot)
        {
            g->hot = true;
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
        }
    }
    if (msg == WM_MOUSELEAVE) g->hot = false;
    const LRESULT r = DefSubclassProc(h, msg, wp, lp);
    if (drew && g->alive) make_opaque(h);
    return r;
}

struct enum_state
{
    HWND top;
};

BOOL CALLBACK glass_child(HWND c, LPARAM lp)
{
    HWND top = reinterpret_cast<enum_state*>(lp)->top;
    color fill{};
    const bool in = in_toolbar_band(c, fill);
    DWORD_PTR data = 0;
    const bool has = GetWindowSubclass(c, glass_proc, glass_id, &data) != FALSE;
    if (!in && !has) return TRUE;
    glass* g = reinterpret_cast<glass*>(data);
    if (!has)
    {
        g = new glass;
        g->k = kind_of(c, top);
        // A layer control's text: grayscale on glass. (Not a toolbar's:
        // it has none, and a new font remakes its icons.)
        g->fonts = g->k == kind::layer && !class_is(c, L"ToolbarWindow32");
        g->given = reinterpret_cast<HFONT>(SendMessageW(c, WM_GETFONT, 0, 0));
        if (!SetWindowSubclass(c, glass_proc, glass_id, reinterpret_cast<DWORD_PTR>(g)))
        {
            delete g;
            return TRUE;
        }
    }
    // Its font as it should be now, grayscale or not.
    if (g->fonts && g->given)
    {
        const HFONT want = in && !high_contrast() ? grayscale(g->given) : g->given;
        if (reinterpret_cast<HFONT>(SendMessageW(c, WM_GETFONT, 0, 0)) != want)
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g->given), TRUE);
    }
    return TRUE;
}

}  // namespace

void glass_band_controls(HWND top)
{
    enum_state s{top};
    EnumChildWindows(top, glass_child, reinterpret_cast<LPARAM>(&s));
}

bool glass_matte(color& matte)
{
    if (!t_matte) return false;
    matte = *t_matte;
    return true;
}

}  // namespace rap::win32
