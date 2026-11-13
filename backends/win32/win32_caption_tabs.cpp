// A window's document tabs in its title bar: see win32_caption_tabs.h.
//
// Transparency. The strip is an ordinary child window, which the compositor
// doesn't see apart from its parent: it draws into the top-level's surface,
// in the part of the client area the frame has been extended over
// (DwmExtendFrameIntoClientArea). There a pixel's alpha counts, and where
// it's zero the backdrop shows. GDI writes no alpha (its pixels come out at
// zero, glassy, whatever their colour), so the strip draws with Direct2D
// into a 32-bit buffer with premultiplied alpha (BufferedPaint's, zeroed
// first), and only what it fills is opaque. The window paints the rest of
// the row, under the caption buttons, the same way (paint_caption_band).
//
// The mouse. Where the strip is empty it answers HTTRANSPARENT, and the
// system asks the window under it instead, which on the same thread (it is)
// is the top-level: that says HTCAPTION there, and dragging, double-clicking
// and the system menu are the caption's own. Over a tab, its close button or
// "+", the strip has the mouse itself, and while a tab's dragged everywhere
// (HTCLIENT, the mouse captured).
//
// UI Automation. A child window keeps the strip's elements under its own
// HWND (the provider's root is the strip, not the top-level window), so the
// window's native controls stay where UIA's HWND proxy puts them, untouched
// by the strip's provider.
#include "win32_caption_tabs.h"

#include "document_tabs.h"
#include "rapport/i18n.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace rap::win32
{

namespace
{

// Windows 11's frame attributes (build 22000 on), by number: older SDKs
// don't name them all.
constexpr DWORD dwmwa_caption_button_bounds = 5;
constexpr DWORD dwmwa_systembackdrop_type = 38;
constexpr DWORD dwmsbt_none = 1, dwmsbt_mainwindow = 2;

// UIA's ids for the strip's elements: the Tab, "New tab", then each tab and
// its close button from its key.
constexpr int tabs_id = 1, add_id = 2;
int item_id(unsigned key)
{
    return 16 + int(key % 0x3FFFFFF0u) * 2;
}

int scaled(float logical, UINT dpi)
{
    return int(std::lround(logical * float(dpi) / 96.f));
}
// A line's width: a pixel, scaled.
int line_px(UINT dpi)
{
    return std::max(1, scaled(1, dpi));
}

// What the strip and the row beside it are drawn in. Its empty part is
// clear (Mica's) in the caption; opaque, the window colour, in fullscreen;
// high contrast's window colour there. The current tab is the colour of the
// band under the strip, which is the window's surface (its base colour on a
// base surface), so the two are one; in high contrast it's the selection.
struct strip_colors
{
    bool clear = true;
    color empty, band, line, text, hover, pressed, current, current_text;
};
strip_colors colors_for(HWND h, bool transparent)
{
    strip_colors c;
    const theme_color surface = surface_of(h);
    c.band = theme_color_of(surface);
    c.line = theme_color_of(theme_color::divider);
    c.hover = theme_color_of(theme_color::hover);
    c.pressed = theme_color_of(theme_color::pressed);
    if (high_contrast())
    {
        c.clear = false;
        c.empty = sys_color(COLOR_WINDOW);
        c.text = sys_color(COLOR_WINDOWTEXT);
        c.current = sys_color(COLOR_HIGHLIGHT);
        c.current_text = sys_color(COLOR_HIGHLIGHTTEXT);
        return c;
    }
    c.clear = transparent;
    c.empty = theme_color_of(theme_color::window);
    c.text = theme_color_of(surface_text_of(h));
    c.current = c.band;
    c.current_text = c.text;
    return c;
}

// The caption font at `dpi`, as the title bar's text was.
LOGFONTW caption_font(UINT dpi)
{
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    using fn_t = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static fn_t fn = reinterpret_cast<fn_t>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi")));
    if (!fn || !fn(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi))
    {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        ncm.lfCaptionFont.lfHeight = MulDiv(ncm.lfCaptionFont.lfHeight, int(dpi), 96);
    }
    return ncm.lfCaptionFont;
}

const wchar_t* strip_class = L"rapportCaptionTabs";

}  // namespace

// -- the strip ----------------------------------------------------------------

struct caption_tabs::impl
{
    caption_tabs& self;
    caption_tabs_owner& owner;
    HWND tip = nullptr;
    std::unique_ptr<uia_provider> uia;

    // Laid out (layout()): each tab's rectangle (empty when out of sight),
    // its close button's, and "+"'s.
    std::vector<RECT> tab_rects, close_rects;
    RECT add_rect{};
    int first = 0;          // the first tab in sight, when they don't all fit
    int focus = -1;         // the focused tab (count(): "+") while the strip has the focus
    hit hot, pressed;       // under the mouse; pressed (close, "+") until let go
    bool tracking = false;  // TrackMouseEvent for the leave
    bool transparent = true;
    int resize_band = 0;

    // A tab dragged along the strip. Pressed, it's current (as a click
    // makes it) and the mouse is captured; moved past the drag threshold, it
    // follows the pointer, held where it was grabbed, and moves to the place
    // in sight nearest to it as it goes, the others making room: the
    // strip's tabs, the owner's (caption_moved) and UIA's TabItems in their
    // new order. Let go, the owner's told once (caption_reordered), if it
    // moved; Escape, or the capture lost, puts it back. Past either end of
    // the row it stays at that end: it never leaves the strip (a later
    // tearing off would start from there).
    struct drag_state
    {
        unsigned key = 0;  // the tab's
        int from = -1;     // where it was
        POINT press{};
        int grab = 0;  // the pointer's x in the tab, as pressed
        int x = 0;     // the pointer's x
        bool active = false;  // past the threshold
    };
    std::optional<drag_state> drag;
    // The row's places in sight, from layout(): the first's left, their
    // width, and the first and last tabs there.
    int slot_left = 0, slot_width = 1, slot_first = 0, slot_last = -1;

    // Direct2D, on the CPU: the strip is small, and a GPU target costs more
    // to make than it saves. Made again when lost, or for a new DPI.
    com<ID2D1DCRenderTarget> rt;
    com<ID2D1SolidColorBrush> brush;
    com<ID2D1StrokeStyle> dotted;
    com<IDWriteTextFormat> text;
    UINT text_dpi = 0;
    std::map<HICON, com<ID2D1Bitmap>> icons;  // the tabs' icons on rt, until set_tabs

    impl(caption_tabs& s, caption_tabs_owner& o) : self(s), owner(o) {}

    UINT dpi() const { return dpi_of(self.hwnd_); }
    int px(float logical) const { return scaled(logical, dpi()); }

    void layout()
    {
        RECT c;
        GetClientRect(self.hwnd_, &c);
        const int n = self.count();
        const int left = px(caption_metrics::first_tab), top = px(caption_metrics::tab_top);
        const int add = px(caption_metrics::add_size), gap = px(caption_metrics::add_gap);
        const int line = line_px(dpi());
        const int end = std::max(left, int(c.right) - px(caption_metrics::end_gap) - add - gap);
        const int available = end - left;
        // As wide as Explorer's while they fit, narrower as they're added
        // (as stage 1's), then that and the row cut at its end.
        const int w = std::max(1, doc_tabs::tab_width(available, n, px(doc_tabs::narrowest_tab),
                                                      px(caption_metrics::widest_tab)));
        const int whole = std::max(1, available / w);
        // The current tab in sight, and the focused one while it's focused.
        if (n <= whole)
            first = 0;
        else
        {
            auto keep = [&](int i)
            {
                if (i < 0 || i >= n) return;
                if (i < first) first = i;
                if (i >= first + whole) first = i - whole + 1;
            };
            keep(self.current_);
            if (self.has_focus()) keep(focus);
            first = std::clamp(first, 0, n - whole);
        }
        tab_rects.assign(size_t(n), RECT{});
        close_rects.assign(size_t(n), RECT{});
        int last = left;
        const int close = px(caption_metrics::close_size);
        slot_left = left;
        slot_width = w;
        slot_first = first;
        slot_last = first - 1;
        for (int i = first; i < n; ++i)
        {
            const int x = left + (i - first) * w;
            if (x >= end) break;
            RECT r{x, top, std::min(x + w, end), c.bottom};
            tab_rects[size_t(i)] = r;
            last = r.right;
            slot_last = i;
            // Its close button at its end, centred on the tab's body (above
            // the line); none on a tab cut short before it.
            const int right = x + w - px(caption_metrics::close_inset);
            const int y = (top + int(c.bottom) - line - close) / 2;
            if (right <= end) close_rects[size_t(i)] = RECT{right - close, y, right, y + close};
        }
        const int y = (top + int(c.bottom) - line - add) / 2;
        add_rect = RECT{last + gap, y, last + gap + add, y + add};
        // The dragged tab under the pointer, as it was grabbed, along the
        // places in sight.
        if (const int i = lifted(); i >= 0 && !IsRectEmpty(&tab_rects[size_t(i)]))
        {
            const int dx = dragged_left() - tab_rects[size_t(i)].left;
            OffsetRect(&tab_rects[size_t(i)], dx, 0);
            if (!IsRectEmpty(&close_rects[size_t(i)])) OffsetRect(&close_rects[size_t(i)], dx, 0);
        }
        update_tools();
    }

    // -- dragging --------------------------------------------------------

    int index_of_key(unsigned key) const
    {
        for (size_t i = 0; i < self.tabs_.size(); ++i)
            if (self.tabs_[i].key == key) return int(i);
        return -1;
    }
    // The tab dragged past the threshold, -1 for none.
    int lifted() const { return drag && drag->active ? index_of_key(drag->key) : -1; }
    // Where the dragged tab's left is: the pointer's, less where it was
    // grabbed, kept to the places in sight.
    int dragged_left() const
    {
        const int last = std::max(slot_left, slot_left + (slot_last - slot_first) * slot_width);
        return std::clamp(drag->x - drag->grab, slot_left, last);
    }
    // The tab at `from` moved to `to`: the strip's, and the owner's.
    void move_tab(int from, int to)
    {
        doc_tabs::move_element(self.tabs_, from, to);
        if (self.current_ >= 0) self.current_ = doc_tabs::index_after_move(self.current_, from, to);
        if (focus >= 0 && focus < self.count()) focus = doc_tabs::index_after_move(focus, from, to);
        owner.caption_moved(from, to);
    }
    // Laid out and drawn anew, and UIA's tree with it: `reordered` if its
    // tabs' order changed.
    void moved(bool reordered)
    {
        layout();
        update_uia();
        if (reordered && uia) uia->raise_structure_changed(StructureChangeType_ChildrenReordered, tabs_id);
        InvalidateRect(self.hwnd_, nullptr, FALSE);
    }
    void press(POINT p, int i)
    {
        cancel();
        const RECT& r = tab_rects[size_t(i)];
        drag = drag_state{self.tabs_[size_t(i)].key, i, p, p.x - r.left, p.x};
        SetCapture(self.hwnd_);
    }
    void drag_to(POINT p)
    {
        if (!drag->active)
        {
            if (std::abs(p.x - drag->press.x) <= GetSystemMetricsForDpi(SM_CXDRAG, dpi()) &&
                std::abs(p.y - drag->press.y) <= GetSystemMetricsForDpi(SM_CYDRAG, dpi()))
                return;
            drag->active = true;
            hot = {};
        }
        drag->x = p.x;
        const int at = index_of_key(drag->key);
        if (at < 0 || slot_last < slot_first) return;
        // The place whose left is nearest the dragged tab's.
        const int to = std::clamp(slot_first + (dragged_left() - slot_left + slot_width / 2) / slot_width, slot_first,
                                  slot_last);
        const bool reordered = to != at;
        if (reordered) move_tab(at, to);
        moved(reordered);
    }
    // Let go: the owner's told, if the tab moved; nothing's done after.
    void let_go()
    {
        const std::optional<drag_state> d = std::exchange(drag, std::nullopt);
        if (GetCapture() == self.hwnd_) ReleaseCapture();  // the drag's gone: its WM_CAPTURECHANGED is nothing
        if (!d || !d->active) return;
        moved(false);  // into its place
        const int at = index_of_key(d->key);
        if (at >= 0 && at != d->from) owner.caption_reordered(at);  // last: it may change the tabs
    }
    void cancel()
    {
        const std::optional<drag_state> d = std::exchange(drag, std::nullopt);
        if (!d) return;
        if (GetCapture() == self.hwnd_) ReleaseCapture();
        if (!d->active) return;
        const int at = index_of_key(d->key);
        const bool back = at >= 0 && at != d->from && d->from < self.count();
        if (back) move_tab(at, d->from);
        moved(back);
    }

    // The hover's and the hit's shape of tab i: a tab not current leaves
    // its edges to the caption, so there's somewhere to drag by between two.
    RECT hit_rect(int i) const
    {
        RECT r = tab_rects[size_t(i)];
        if (i != self.current_ && !IsRectEmpty(&r)) InflateRect(&r, -px(2), 0);
        return r;
    }

    // -- drawing ---------------------------------------------------------

    bool ready()
    {
        if (text_dpi != dpi())
        {
            text.reset();
            rt.reset();
            icons.clear();
        }
        if (!rt)
        {
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            if (!d2d_factory() || FAILED(d2d_factory()->CreateDCRenderTarget(&props, rt.put()))) return false;
            brush.reset();
            icons.clear();
        }
        if (!brush && FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush.put()))) return false;
        if (!dotted && d2d_factory())
        {
            // The system's focus rectangle's dots: one on, one off.
            const float dashes[] = {1, 1};
            d2d_factory()->CreateStrokeStyle(
                D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                            D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0),
                dashes, 2, dotted.put());
        }
        if (!text && dwrite_factory())
        {
            const LOGFONTW f = caption_font(dpi());
            const float size = float(f.lfHeight < 0 ? -f.lfHeight : std::max<LONG>(f.lfHeight, px(12)));
            if (SUCCEEDED(dwrite_factory()->CreateTextFormat(
                    f.lfFaceName, nullptr, DWRITE_FONT_WEIGHT(f.lfWeight ? f.lfWeight : 400),
                    f.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    size, L"", text.put())))
            {
                text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                // Cut short with an ellipsis, as the caption's title was.
                com<IDWriteInlineObject> ellipsis;
                dwrite_factory()->CreateEllipsisTrimmingSign(text.get(), ellipsis.put());
                const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                text->SetTrimming(&trim, ellipsis.get());
            }
            text_dpi = dpi();
        }
        return true;
    }

    ID2D1Bitmap* icon_bitmap(HICON icon)
    {
        if (!icon || !wic_factory()) return nullptr;
        if (auto it = icons.find(icon); it != icons.end()) return it->second.get();
        com<IWICBitmap> wic;
        com<IWICFormatConverter> premultiplied;
        com<ID2D1Bitmap> bitmap;
        if (SUCCEEDED(wic_factory()->CreateBitmapFromHICON(icon, wic.put())) &&
            SUCCEEDED(wic_factory()->CreateFormatConverter(premultiplied.put())) &&
            SUCCEEDED(premultiplied->Initialize(wic.get(), GUID_WICPixelFormat32bppPBGRA,
                                                WICBitmapDitherTypeNone, nullptr, 0,
                                                WICBitmapPaletteTypeCustom)))
            rt->CreateBitmapFromWicBitmap(premultiplied.get(), nullptr, bitmap.put());
        return icons.emplace(icon, bitmap).first->second.get();
    }

    void fill(const RECT& r, color c)
    {
        brush->SetColor(d2d_color(c));
        rt->FillRectangle(D2D1::RectF(float(r.left), float(r.top), float(r.right), float(r.bottom)), brush.get());
    }
    void fill_round(const RECT& r, float radius, color c)
    {
        brush->SetColor(d2d_color(c));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(float(r.left), float(r.top), float(r.right), float(r.bottom)), radius,
                              radius),
            brush.get());
    }
    // The current tab: its top corners rounded, its sides and top a line,
    // open at the bottom, where it's one with the band under the strip.
    void current_tab(const RECT& r, const strip_colors& c)
    {
        com<ID2D1PathGeometry> shape, edge;
        const float w = float(line_px(dpi())), half = w / 2, radius = float(px(caption_metrics::corner));
        auto trace = [&](ID2D1PathGeometry* g, float inset, bool closed)
        {
            com<ID2D1GeometrySink> sink;
            if (FAILED(g->Open(sink.put()))) return false;
            const float l = float(r.left) + inset, t = float(r.top) + inset, rr = float(r.right) - inset,
                        b = float(r.bottom);
            const float k = std::min(radius, (rr - l) / 2);
            sink->BeginFigure(D2D1::Point2F(l, b), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(l, t + k));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(l + k, t), D2D1::SizeF(k, k), 0,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(rr - k, t));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(rr, t + k), D2D1::SizeF(k, k), 0,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(rr, b));
            sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
            return SUCCEEDED(sink->Close());
        };
        if (SUCCEEDED(d2d_factory()->CreatePathGeometry(shape.put())) && trace(shape.get(), 0, true))
        {
            brush->SetColor(d2d_color(c.current));
            rt->FillGeometry(shape.get(), brush.get());
        }
        // Not in high contrast, where the selection's colour is its edge.
        if (!high_contrast() && SUCCEEDED(d2d_factory()->CreatePathGeometry(edge.put())) &&
            trace(edge.get(), half, false))
        {
            brush->SetColor(d2d_color(c.line));
            rt->DrawGeometry(edge.get(), brush.get(), w);
        }
    }
    void cross(const RECT& r, color c)
    {
        const float x = (r.left + r.right) / 2.f, y = (r.top + r.bottom) / 2.f, k = float(px(5)),
                    w = float(line_px(dpi()));
        brush->SetColor(d2d_color(c));
        rt->DrawLine(D2D1::Point2F(x - k, y - k), D2D1::Point2F(x + k, y + k), brush.get(), w);
        rt->DrawLine(D2D1::Point2F(x - k, y + k), D2D1::Point2F(x + k, y - k), brush.get(), w);
    }
    void plus(const RECT& r, color c)
    {
        // On whole pixels, so its strokes are sharp.
        const float w = float(line_px(dpi()));
        const float x = std::floor((r.left + r.right) / 2.f) + w / 2, y = std::floor((r.top + r.bottom) / 2.f) + w / 2,
                    k = float(px(6));
        brush->SetColor(d2d_color(c));
        rt->DrawLine(D2D1::Point2F(x - k, y), D2D1::Point2F(x + k, y), brush.get(), w);
        rt->DrawLine(D2D1::Point2F(x, y - k), D2D1::Point2F(x, y + k), brush.get(), w);
    }
    void focus_rect(const RECT& r, color c)
    {
        if (!dotted) return;
        RECT f = r;
        InflateRect(&f, -px(3), -px(3));
        brush->SetColor(d2d_color(c));
        rt->DrawRectangle(D2D1::RectF(f.left + 0.5f, f.top + 0.5f, f.right - 0.5f, f.bottom - 0.5f), brush.get(), 1,
                          dotted.get());
    }

    // The strip onto `dc` (a 32-bit one: BufferedPaint's, or a bitmap's).
    void render(HDC dc)
    {
        RECT c;
        GetClientRect(self.hwnd_, &c);
        if (IsRectEmpty(&c) || !ready()) return;
        if (FAILED(rt->BindDC(dc, &c))) return;
        const strip_colors colors = colors_for(self.hwnd_, transparent);
        const int line = line_px(dpi()), n = self.count();
        const bool focused = self.has_focus() &&
                             !(SendMessageW(self.hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        // Grey, not ClearType: most of it is drawn over nothing (Mica).
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt->Clear(colors.clear ? D2D1::ColorF(0, 0, 0, 0) : d2d_color(colors.empty));
        // The line along the bottom, between the strip and the band under
        // it, broken only by the current tab.
        fill(RECT{0, c.bottom - line, c.right, c.bottom}, colors.line);
        const RECT clip{0, 0, add_rect.left, c.bottom};
        rt->PushAxisAlignedClip(D2D1::RectF(0, 0, float(clip.right), float(clip.bottom)), D2D1_ANTIALIAS_MODE_ALIASED);
        const int up = lifted();
        for (int k = 0; k < n; ++k)
        {
            // The dragged tab last, over the others.
            const int i = up < 0 || k < up ? k : k + 1 < n ? k + 1 : up;
            const RECT& r = tab_rects[size_t(i)];
            if (IsRectEmpty(&r)) continue;
            const bool current = i == self.current_;
            const bool hovered = hot.what != part::none && hot.what != part::add && hot.index == i;
            if (current)
                current_tab(r, colors);
            else if (hovered)
            {
                RECT h = hit_rect(i);
                h.bottom -= line + px(3);
                fill_round(h, float(px(caption_metrics::corner) / 2), colors.hover);
            }
            const color ink = current ? colors.current_text : colors.text;
            RECT body{r.left + px(caption_metrics::text_inset), r.top, r.right, c.bottom - line};
            const caption_tab& t = self.tabs_[size_t(i)];
            if (ID2D1Bitmap* icon = icon_bitmap(t.icon))
            {
                const float s = float(px(caption_metrics::icon)), y = std::floor((body.top + body.bottom - s) / 2);
                rt->DrawBitmap(icon, D2D1::RectF(float(body.left), y, body.left + s, y + s));
                body.left += int(s) + px(caption_metrics::text_gap);
            }
            const RECT& x = close_rects[size_t(i)];
            body.right = IsRectEmpty(&x) ? r.right - px(caption_metrics::close_inset) : x.left - px(4);
            if (text && body.right > body.left)
            {
                brush->SetColor(d2d_color(ink));
                rt->DrawText(t.title.c_str(), UINT32(t.title.size()), text.get(),
                             D2D1::RectF(float(body.left), float(body.top), float(body.right), float(body.bottom)),
                             brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            // Its close button on the current tab, and on one hovered.
            if (!IsRectEmpty(&x) && (current || hovered))
            {
                const bool on = hot.what == part::close && hot.index == i;
                const bool down = on && pressed.what == part::close && pressed.index == i;
                if (on) fill_round(x, float(px(4)), down ? colors.pressed : colors.hover);
                cross(x, on && high_contrast() ? colors.text : ink);
            }
            if (focused && focus == i) focus_rect(RECT{r.left, r.top, r.right, c.bottom - line}, ink);
        }
        rt->PopAxisAlignedClip();
        if (hot.what == part::add)
            fill_round(add_rect, float(px(4)), pressed.what == part::add ? colors.pressed : colors.hover);
        plus(add_rect, colors.text);
        if (focused && focus == n) focus_rect(add_rect, colors.text);
        if (rt->EndDraw() == D2DERR_RECREATE_TARGET)
        {
            rt.reset();
            InvalidateRect(self.hwnd_, nullptr, FALSE);
        }
    }

    void paint()
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(self.hwnd_, &ps);
        RECT c;
        GetClientRect(self.hwnd_, &c);
        // Zeroed first (BPPF_ERASE): transparent wherever nothing's drawn.
        BP_PAINTPARAMS params{sizeof params, BPPF_ERASE, nullptr, nullptr};
        HDC mem = nullptr;
        if (HPAINTBUFFER buffer = BeginBufferedPaint(dc, &c, BPBF_TOPDOWNDIB, &params, &mem))
        {
            render(mem);
            EndBufferedPaint(buffer, TRUE);
        }
        else
            render(dc);
        EndPaint(self.hwnd_, &ps);
    }

    // -- the mouse, the keys ---------------------------------------------

    void set_hot(hit h)
    {
        if (h.what == hot.what && h.index == hot.index) return;
        hot = h;
        InvalidateRect(self.hwnd_, nullptr, FALSE);
    }
    void set_focus(int i)
    {
        const int n = self.count();
        i = std::clamp(i, 0, n);
        if (i == focus) return;
        focus = i;
        layout();  // in sight
        update_uia();
        InvalidateRect(self.hwnd_, nullptr, FALSE);
        if (self.has_focus() && uia) uia->raise_focus_changed(focus == n ? add_id : item_id(self.tabs_[size_t(focus)].key));
    }
    // Enter, Space: what's focused, as clicked. Delete: its tab closed.
    bool key(WPARAM vk)
    {
        const int n = self.count();
        switch (vk)
        {
        case VK_LEFT: set_focus(focus - 1); return true;
        case VK_RIGHT: set_focus(focus + 1); return true;
        case VK_HOME: set_focus(0); return true;
        case VK_END: set_focus(n); return true;
        case VK_RETURN:
        case VK_SPACE:
            if (focus == n)
                owner.caption_new();
            else if (focus >= 0 && focus < n)
                owner.caption_select(focus);
            return true;
        case VK_DELETE:
            if (focus >= 0 && focus < n) owner.caption_close(focus);
            return true;
        default: return false;
        }
    }

    // -- tooltips, UI Automation -----------------------------------------

    void update_tools()
    {
        if (!tip) return;
        while (SendMessageW(tip, TTM_GETTOOLCOUNT, 0, 0) > 0)
        {
            TTTOOLINFOW t{};
            t.cbSize = sizeof t;
            if (!SendMessageW(tip, TTM_ENUMTOOLSW, 0, reinterpret_cast<LPARAM>(&t))) break;
            SendMessageW(tip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&t));
        }
        auto add = [&](UINT_PTR id, const RECT& r)
        {
            if (IsRectEmpty(&r)) return;
            TTTOOLINFOW t{};
            t.cbSize = sizeof t;
            t.uFlags = TTF_SUBCLASS;
            t.hwnd = self.hwnd_;
            t.uId = id;
            t.rect = r;
            t.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&t));
        };
        // The close buttons first: a tool's rectangle inside another's.
        for (size_t i = 0; i < close_rects.size(); ++i) add(0x10000 + i, close_rects[i]);
        for (size_t i = 0; i < tab_rects.size(); ++i) add(i, tab_rects[i]);
        add(0xFFFF, add_rect);
    }
    std::wstring tool_text(UINT_PTR id) const
    {
        if (id == 0xFFFF) return wide(rap::i18n::tr("rapport", "New tab"));
        if (id >= 0x10000) return wide(rap::i18n::tr("rapport", "Close tab"));
        if (id >= self.tabs_.size()) return {};
        const caption_tab& t = self.tabs_[id];
        return t.tooltip.empty() ? t.title : t.tooltip;
    }

    int index_of_id(int id, bool& close) const
    {
        for (size_t i = 0; i < self.tabs_.size(); ++i)
        {
            const int item = item_id(self.tabs_[i].key);
            if (id == item || id == item + 1)
            {
                close = id == item + 1;
                return int(i);
            }
        }
        return -1;
    }
    void update_uia()
    {
        if (!uia) return;
        RECT c;
        GetClientRect(self.hwnd_, &c);
        const bool focused = self.has_focus();
        const int n = self.count();
        std::vector<uia_element> e;
        uia_element tabs;
        tabs.id = tabs_id;
        tabs.control_type = UIA_TabControlTypeId;
        tabs.name = wide(rap::i18n::tr("rapport", "Tabs"));
        tabs.bounds = c;
        tabs.patterns = ::rap::win32::uia_selection;
        tabs.selection_required = true;
        e.push_back(tabs);
        for (int i = 0; i < n; ++i)
        {
            const caption_tab& t = self.tabs_[size_t(i)];
            uia_element item;
            item.id = item_id(t.key);
            item.parent = tabs_id;
            item.control_type = UIA_TabItemControlTypeId;
            item.name = t.title;
            item.bounds = tab_rects[size_t(i)];
            item.offscreen = IsRectEmpty(&item.bounds);
            item.patterns = ::rap::win32::uia_selection_item;
            item.focusable = true;
            item.focused = focused && focus == i;
            item.selected = i == self.current_;
            e.push_back(item);
            uia_element close;
            close.id = item.id + 1;
            close.parent = item.id;
            close.control_type = UIA_ButtonControlTypeId;
            close.name = wide(rap::i18n::tr("rapport", "Close tab"));
            close.bounds = close_rects[size_t(i)];
            close.offscreen = IsRectEmpty(&close.bounds);
            close.patterns = ::rap::win32::uia_invoke;
            e.push_back(close);
        }
        uia_element add;
        add.id = add_id;
        add.parent = tabs_id;
        add.control_type = UIA_ButtonControlTypeId;
        add.name = wide(rap::i18n::tr("rapport", "New tab"));
        add.bounds = add_rect;
        add.patterns = ::rap::win32::uia_invoke;
        add.focusable = true;
        add.focused = focused && focus == n;
        e.push_back(add);
        uia->set_elements(std::move(e));
    }

    // -- the window ------------------------------------------------------

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* s = reinterpret_cast<caption_tabs*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!s || !s->d_) return DefWindowProcW(h, msg, wp, lp);
        impl& d = *s->d_;
        LRESULT result = 0;
        if (d.uia && d.uia->handle_message(msg, wp, lp, result)) return result;
        if (msg == query_message())
        {
            const int what = LOWORD(wp), i = HIWORD(wp);
            auto* out = reinterpret_cast<RECT*>(lp);
            switch (what)
            {
            case 0: return s->count();
            case 1: return s->current_;
            case 2: return s->has_focus() ? d.focus : -1;
            case 3: return out && s->part_rect(part::tab, i, *out);
            case 4: return out && s->part_rect(part::close, i, *out);
            case 5: return out && s->part_rect(part::add, 0, *out);
            default: return 0;
            }
        }
        switch (msg)
        {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: d.paint(); return 0;
        case WM_PRINTCLIENT:
            if (lp & PRF_CLIENT) d.render(reinterpret_cast<HDC>(wp));
            return 0;
        case WM_SIZE:
            d.layout();
            d.update_uia();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        // Its empty part, and the band the window resizes by, are the
        // window's: the caption there.
        case WM_NCHITTEST:
        {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            // A tab being dragged has the mouse wherever it goes.
            if (d.drag) return HTCLIENT;
            if (p.y < d.resize_band || s->hit_test(p).what == part::none) return HTTRANSPARENT;
            return HTCLIENT;
        }
        case WM_MOUSEMOVE:
        {
            if (!d.tracking)
            {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
                d.tracking = TrackMouseEvent(&t) != FALSE;
            }
            if (d.drag)
            {
                if (!(wp & MK_LBUTTON))  // let go where it wasn't heard
                {
                    d.let_go();
                    return 0;
                }
                d.drag_to(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                if (d.drag->active) return 0;
            }
            d.set_hot(s->hit_test(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            return 0;
        }
        case WM_MOUSELEAVE:
            d.tracking = false;
            d.set_hot({});
            return 0;
        // A tab is current as it's pressed, as the tab control's are; a
        // button acts as it's let go over it.
        case WM_LBUTTONDOWN:
        {
            const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const hit at = s->hit_test(p);
            if (at.what == part::tab)
            {
                d.press(p, at.index);  // first: it may be dragged
                s->d_->owner.caption_select(at.index);  // last: it may change the tabs
                return 0;
            }
            if (at.what == part::close || at.what == part::add)
            {
                d.pressed = at;
                SetCapture(h);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP:
        {
            if (d.drag)
            {
                d.let_go();
                return 0;
            }
            const hit was = d.pressed;
            if (was.what == part::none) return 0;
            d.pressed = {};
            ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
            const hit at = s->hit_test(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (at.what != was.what || at.index != was.index) return 0;
            if (at.what == part::close)
                d.owner.caption_close(at.index);
            else
                d.owner.caption_new();
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (d.drag && reinterpret_cast<HWND>(lp) != h) d.cancel();
            if (d.pressed.what != part::none)
            {
                d.pressed = {};
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        // A middle click closes the tab under it, as in a browser.
        case WM_MBUTTONDOWN: return 0;
        case WM_MBUTTONUP:
        {
            const hit at = s->hit_test(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (at.what == part::tab || at.what == part::close) d.owner.caption_close(at.index);
            return 0;
        }
        // New tab, and Close tab for the tab clicked (or the focused one,
        // from the keyboard), as the keys do.
        case WM_CONTEXTMENU:
        {
            POINT at{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int i = -1;
            if (at.x == -1 && at.y == -1)
            {
                i = d.focus >= 0 && d.focus < s->count() ? d.focus : s->current_;
                RECT r{};
                if (!s->part_rect(part::tab, i, r)) r = d.add_rect;
                at = POINT{r.left, r.bottom};
                ClientToScreen(h, &at);
            }
            else
            {
                POINT p = at;
                ScreenToClient(h, &p);
                const hit under = s->hit_test(p);
                if (under.what == part::tab || under.what == part::close) i = under.index;
            }
            d.owner.caption_menu(i, at);
            return 0;
        }
        case WM_GETDLGCODE:
        {
            LRESULT code = DLGC_WANTARROWS | DLGC_WANTCHARS;
            const auto* m = reinterpret_cast<const MSG*>(lp);
            if (m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN) code |= DLGC_WANTMESSAGE;
            return code;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && d.drag)
            {
                d.cancel();
                return 0;
            }
            if (d.key(wp)) return 0;
            break;
        case WM_CHAR:
            if (wp == L' ' || wp == L'\r') return 0;  // done as keys
            break;
        case WM_SETFOCUS:
            d.focus = std::clamp(s->current_ >= 0 ? s->current_ : s->count(), 0, s->count());
            d.layout();
            d.update_uia();
            InvalidateRect(h, nullptr, FALSE);
            if (d.uia)
                d.uia->raise_focus_changed(d.focus == s->count() ? add_id
                                                                 : item_id(s->tabs_[size_t(d.focus)].key));
            return 0;
        case WM_KILLFOCUS:
            d.update_uia();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_UPDATEUISTATE: InvalidateRect(h, nullptr, FALSE); break;
        case WM_NOTIFY:
        {
            auto* n = reinterpret_cast<NMHDR*>(lp);
            if (n->code == TTN_GETDISPINFOW && d.tip && n->hwndFrom == d.tip)
            {
                static std::wstring shown;
                shown = d.tool_text(n->idFrom);
                reinterpret_cast<NMTTDISPINFOW*>(lp)->lpszText = shown.data();
                return 0;
            }
            break;
        }
        default: break;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};

UINT caption_tabs::query_message()
{
    static const UINT m = RegisterWindowMessageW(L"rapport-caption-tabs");
    return m;
}

int caption_tabs::height(UINT dpi)
{
    return scaled(caption_metrics::height, dpi);
}

caption_tabs::caption_tabs(HWND parent, caption_tabs_owner& owner, HWND tooltip)
    : d_(std::make_unique<impl>(*this, owner))
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpfnWndProc = impl::proc;
        wc.lpszClassName = strip_class;
        return RegisterClassExW(&wc) != 0;
    }();
    (void)registered;
    static const bool buffered = SUCCEEDED(BufferedPaintInit());
    (void)buffered;
    hwnd_ = CreateWindowExW(0, strip_class, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) return;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    d_->tip = tooltip;
    if (tooltip) SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, scaled(400, dpi_of(hwnd_)));
    d_->uia = std::make_unique<uia_provider>(hwnd_, *this);
    d_->layout();
    d_->update_uia();
}

caption_tabs::~caption_tabs()
{
    if (d_->tip) DestroyWindow(d_->tip);
    if (hwnd_) DestroyWindow(hwnd_);  // its WM_DESTROY disconnects UIA's elements
    d_.reset();
}

void caption_tabs::set_tabs(std::vector<caption_tab> tabs, int current)
{
    std::vector<unsigned> before;
    for (const caption_tab& t : tabs_) before.push_back(t.key);
    tabs_ = std::move(tabs);
    std::vector<unsigned> after;
    for (const caption_tab& t : tabs_) after.push_back(t.key);
    const bool reordered = before != after && std::is_permutation(before.begin(), before.end(), after.begin(), after.end());
    current_ = current >= 0 && current < count() ? current : -1;
    d_->icons.clear();  // the caller's handles may be new ones at the old values
    d_->focus = std::clamp(d_->focus, 0, count());
    d_->hot = d_->pressed = {};
    // A tab pressed or dragged stays so while it's there; gone, so's the drag.
    if (d_->drag)
    {
        const int i = d_->index_of_key(d_->drag->key);
        if (i < 0)
        {
            d_->drag.reset();
            if (GetCapture() == hwnd_) ReleaseCapture();
        }
        else if (!d_->drag->active)
            d_->drag->from = i;
    }
    d_->layout();
    d_->update_uia();
    if (d_->uia)
    {
        if (before.size() != tabs_.size())
            d_->uia->raise_structure_changed(StructureChangeType_ChildrenInvalidated, tabs_id);
        else if (reordered)
            d_->uia->raise_structure_changed(StructureChangeType_ChildrenReordered, tabs_id);
        if (current_ >= 0)
            d_->uia->raise_event(item_id(tabs_[size_t(current_)].key), UIA_SelectionItem_ElementSelectedEventId);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void caption_tabs::set_current(int current)
{
    if (current < 0 || current >= count() || current == current_) return;
    current_ = current;
    d_->layout();
    d_->update_uia();
    if (d_->uia) d_->uia->raise_event(item_id(tabs_[size_t(current_)].key), UIA_SelectionItem_ElementSelectedEventId);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool caption_tabs::dragging() const
{
    return d_->drag.has_value();
}

void caption_tabs::cancel_drag()
{
    d_->cancel();
}

void caption_tabs::set_frame(bool transparent, int resize_band)
{
    if (transparent == d_->transparent && resize_band == d_->resize_band) return;
    d_->transparent = transparent;
    d_->resize_band = resize_band;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void caption_tabs::refresh()
{
    d_->text.reset();
    d_->text_dpi = 0;
    d_->layout();
    d_->update_uia();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void caption_tabs::take_focus()
{
    SetFocus(hwnd_);
}

caption_tabs::hit caption_tabs::hit_test(POINT p) const
{
    if (PtInRect(&d_->add_rect, p)) return {part::add, count()};
    for (int i = 0; i < count(); ++i)
    {
        const RECT r = d_->hit_rect(i);
        if (!PtInRect(&r, p)) continue;
        if (PtInRect(&d_->close_rects[size_t(i)], p)) return {part::close, i};
        return {part::tab, i};
    }
    return {};
}

bool caption_tabs::part_rect(part what, int index, RECT& out) const
{
    out = RECT{};
    if (what == part::add)
        out = d_->add_rect;
    else if (index >= 0 && index < count())
        out = what == part::tab ? d_->tab_rects[size_t(index)] : d_->close_rects[size_t(index)];
    return !IsRectEmpty(&out);
}

void caption_tabs::uia_invoke(int id)
{
    if (id == add_id)
    {
        d_->owner.caption_new();
        return;
    }
    bool close = false;
    const int i = d_->index_of_id(id, close);
    if (i >= 0 && close) d_->owner.caption_close(i);
}

void caption_tabs::uia_select(int id)
{
    bool close = false;
    const int i = d_->index_of_id(id, close);
    if (i >= 0 && !close) d_->owner.caption_select(i);
}

void caption_tabs::uia_focus(int id)
{
    bool close = false;
    const int i = id == add_id ? count() : d_->index_of_id(id, close);
    if (i < 0) return;
    SetFocus(hwnd_);
    d_->set_focus(i);
}

// -- the window's side --------------------------------------------------------

bool caption_tabs_supported()
{
    return windows_build() >= 22000;
}

int caption_resize_band(HWND top)
{
    const UINT dpi = dpi_of(top);
    return GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

int caption_inset(HWND top)
{
    return IsZoomed(top) ? caption_resize_band(top) : 0;
}

RECT caption_buttons(HWND top)
{
    RECT c;
    GetClientRect(top, &c);
    RECT b{};
    if (SUCCEEDED(DwmGetWindowAttribute(top, dwmwa_caption_button_bounds, &b, sizeof b)) && b.right > b.left)
    {
        // The window's coordinates, to the client area's.
        RECT w;
        POINT origin{0, 0};
        GetWindowRect(top, &w);
        ClientToScreen(top, &origin);
        OffsetRect(&b, w.left - origin.x, w.top - origin.y);
        return b;
    }
    // Not yet known (the window not shown): Windows 11's, each button 46
    // wide and 32 tall at 100%, close alone where there's no minimise or
    // maximise.
    const UINT dpi = dpi_of(top);
    const LONG_PTR style = GetWindowLongPtrW(top, GWL_STYLE);
    const int n = (style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) ? 3 : 1;
    return RECT{c.right - n * scaled(46, dpi), 0, c.right, scaled(32, dpi)};
}

void extend_caption_frame(HWND top, int height)
{
    // Down to the strip's bottom, which maximised is the frame's thickness
    // further down (caption_inset).
    const MARGINS m{0, 0, height ? height + caption_inset(top) : 0, 0};
    DwmExtendFrameIntoClientArea(top, &m);
    // Mica behind the strip (22621 on, where it's named): none in high
    // contrast, whose colours are the system's own.
    if (windows_build() >= 22621)
    {
        const DWORD backdrop = high_contrast() ? dwmsbt_none : dwmsbt_mainwindow;
        DwmSetWindowAttribute(top, dwmwa_systembackdrop_type, &backdrop, sizeof backdrop);
    }
}

void release_caption_frame(HWND top, bool dialog, bool fullscreen)
{
    const MARGINS m{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(top, &m);
    frame_top_window(top, dialog, fullscreen);
}

bool caption_frame_message(HWND top, UINT msg, WPARAM wp, LPARAM lp, int strip_height, LRESULT& result)
{
    // The caption buttons are the compositor's, drawn over the extended
    // frame: it answers for them first (their hit tests, so Windows 11's
    // snap layouts open from maximise, their hover and press).
    if (DwmDefWindowProc(top, msg, wp, lp, &result)) return true;
    switch (msg)
    {
    // The title bar's area is the client's, from the window's top: the
    // frame's left, right and bottom as they were. Maximised too: the
    // window then hangs past the monitor's edges by the frame's thickness,
    // and the client area's top with it, above the monitor; the strip and
    // the content are laid out that far down instead (caption_inset). With
    // the client area's top moved down to the monitor's, the compositor no
    // longer answers for its caption buttons (DwmDefWindowProc handled
    // nothing anywhere over them, maximised, on 22621 and 26100 alike).
    case WM_NCCALCSIZE:
    {
        if (!wp) return false;
        auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
        const LONG top_edge = p->rgrc[0].top;
        result = DefWindowProcW(top, msg, wp, lp);
        p->rgrc[0].top = top_edge;
        return true;
    }
    // The strip's row where the strip lets the mouse through (its empty
    // part, between tabs, past "+"): the caption, or along the top the
    // frame's edge, to resize by. Over a tab, its close button and "+" the
    // strip's own (HTCLIENT) answers before the window is asked.
    case WM_NCHITTEST:
    {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(top, &p);
        RECT c;
        GetClientRect(top, &c);
        // Maximised, the band above the strip is past the monitor's top.
        if (p.x < c.left || p.x >= c.right || p.y < c.top || p.y >= c.top + caption_inset(top) + strip_height)
            return false;
        if (!IsZoomed(top) && (GetWindowLongPtrW(top, GWL_STYLE) & WS_THICKFRAME))
        {
            const int band = caption_resize_band(top);
            if (p.y < c.top + band)
            {
                result = p.x < c.left + band ? HTTOPLEFT : p.x >= c.right - band ? HTTOPRIGHT : HTTOP;
                return true;
            }
        }
        result = HTCAPTION;
        return true;
    }
    default: return false;
    }
}

void paint_caption_band(HWND top, HDC dc, int strip_height, bool transparent)
{
    RECT c;
    GetClientRect(top, &c);
    const int inset = caption_inset(top);
    const RECT band{0, inset, c.right, std::min<LONG>(inset + strip_height, c.bottom)};
    if (IsRectEmpty(&band)) return;
    const strip_colors colors = colors_for(top, transparent);
    const RECT line{0, band.bottom - line_px(dpi_of(top)), band.right, band.bottom};
    // Through a 32-bit buffer, as the strip: GDI's own pixels would have
    // no alpha, and look like glass.
    BP_PAINTPARAMS params{sizeof params, BPPF_ERASE, nullptr, nullptr};
    HDC mem = nullptr;
    HPAINTBUFFER buffer = BeginBufferedPaint(dc, &band, BPBF_TOPDOWNDIB, &params, &mem);
    HDC to = buffer ? mem : dc;
    auto fill = [to](const RECT& r, color k)
    {
        HBRUSH b = CreateSolidBrush(color_ref(k));
        FillRect(to, &r, b);
        DeleteObject(b);
    };
    if (!colors.clear) fill(band, colors.empty);
    fill(line, colors.line);
    if (buffer)
    {
        BufferedPaintSetAlpha(buffer, colors.clear ? &line : &band, 255);
        EndBufferedPaint(buffer, TRUE);
    }
}

}  // namespace rap::win32
