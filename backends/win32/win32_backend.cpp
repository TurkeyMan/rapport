// rapport Win32 backend: native controls (common controls v6), Direct2D and
// DirectWrite for canvases and drawn icons, our own box/form layout (Win32
// has none), per-monitor DPI awareness.
#include "container_state.h"
#include "lazy_tree.h"
#include "menu_dismiss.h"
#include "win32_shell_icons.h"
#include "win32_common.h"
#define RAPPORT_TRACE_SPLITTER_IMPLEMENTATION
#include "win32_trace.h"

#include "frame_layer.h"

#include "rapport/gpu_d3d11.h"
#include "rapport/i18n.h"

#include <commdlg.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <format>
#include <ole2.h>
#include <oleacc.h>
#include <propsys.h>
#include <richedit.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <winspool.h>

// SysLink's ideal size (Vista and later; MinGW declares it only for a newer
// _WIN32_WINNT): the same message as LM_GETIDEALHEIGHT.
#ifndef LM_GETIDEALSIZE
#define LM_GETIDEALSIZE (WM_USER + 0x301)
#endif
#include <windowsx.h>

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <future>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace rap::win32
{
namespace
{

constexpr UINT wm_rapport_post = WM_APP + 1;

BOOL traced_set_window_pos(HWND h, HWND after, int x, int y, int w, int height, UINT flags)
{
    trace::scope timing(trace::phase::window_move, uintptr_t(h));
    return SetWindowPos(h, after, x, y, w, height, flags);
}

// GTK-style mnemonics ("_Save", "__" for a literal underscore) to Windows
// ones ("&Save"); a literal '&' doubles.
std::wstring mnemonic(const std::string& s)
{
    std::wstring w = wide(s), out;
    for (size_t i = 0; i < w.size(); ++i)
    {
        if (w[i] == L'&')
            out += L"&&";
        else if (w[i] == L'_' && i + 1 < w.size() && w[i + 1] == L'_')
        {
            out += L'_';
            ++i;
        }
        else if (w[i] == L'_')
            out += L'&';
        else
            out += w[i];
    }
    return out;
}
constexpr UINT wm_rapport_frame = WM_APP + 2;
constexpr UINT wm_rapport_relayout = WM_APP + 3;
constexpr UINT wm_rapport_selection = WM_APP + 4;  // a list's coalesced selection change
constexpr UINT wm_rapport_fill_last = WM_APP + 5;
constexpr UINT wm_rapport_read_back = WM_APP + 6;  // a pane's canvas's frame, read back  // a list's last column refits, once a header drag lands

HINSTANCE instance()
{
    return GetModuleHandleW(nullptr);
}

// The program's icon: its first icon resource, as Explorer shows it (the
// stock application icon when it has none), at the system size `metric`.
HICON app_icon(int metric)
{
    LPCWSTR name = nullptr;
    EnumResourceNamesW(
        instance(), RT_GROUP_ICON,
        [](HMODULE, LPCWSTR, LPWSTR n, LONG_PTR out) -> BOOL
        {
            *reinterpret_cast<LPCWSTR*>(out) = IS_INTRESOURCE(n) ? n : _wcsdup(n);
            return FALSE;
        },
        reinterpret_cast<LONG_PTR>(&name));
    const int px = GetSystemMetrics(metric);
    if (name)
        if (HICON icon = HICON(LoadImageW(instance(), name, IMAGE_ICON, px, px, LR_DEFAULTCOLOR))) return icon;
    return LoadIconW(nullptr, IDI_APPLICATION);
}

int64_t now_ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double now_seconds()
{
    static LARGE_INTEGER freq = []
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    return double(now_ticks()) / double(freq.QuadPart);
}

unsigned key_modifiers()
{
    unsigned m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= mod_shift;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= mod_ctrl;
    if (GetKeyState(VK_MENU) & 0x8000) m |= mod_alt;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= mod_meta;
    return m;
}

key_event make_key_event(WPARAM vk, LPARAM lp, bool press)
{
    key_event e;
    e.press = press;
    e.repeat = press && (lp & (1 << 30));
    e.modifiers = key_modifiers();
    switch (vk)
    {
    case VK_ESCAPE: e.key = key::escape; break;
    case VK_RETURN: e.key = key::enter; break;
    case VK_TAB: e.key = key::tab; break;
    case VK_BACK: e.key = key::backspace; break;
    case VK_DELETE: e.key = key::delete_; break;
    case VK_INSERT: e.key = key::insert; break;
    case VK_SPACE: e.key = key::space; break;
    case VK_LEFT: e.key = key::left; break;
    case VK_RIGHT: e.key = key::right; break;
    case VK_UP: e.key = key::up; break;
    case VK_DOWN: e.key = key::down; break;
    case VK_HOME: e.key = key::home; break;
    case VK_END: e.key = key::end; break;
    case VK_PRIOR: e.key = key::page_up; break;
    case VK_NEXT: e.key = key::page_down; break;
    default:
        if (vk >= VK_F1 && vk <= VK_F12)
            e.key = key(int(key::f1) + int(vk - VK_F1));
        else if (const UINT ch = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_CHAR) & 0x7fffffff; ch)
        {
            e.key = key::character;
            e.codepoint = char32_t(towlower(wchar_t(ch)));
        }
        else
            e.key = key::other;
    }
    return e;
}

// -- comctl32 v6 --------------------------------------------------------------

// Without an application manifest the process binds comctl32 v5 at load,
// while our activation context makes the controls themselves v6. Image
// lists handed to v6 controls must come from v6 too (v5 ones have no alpha),
// so these functions are resolved from the v6 DLL once it's active.
struct comctl6
{
    decltype(&ImageList_Create) create = ImageList_Create;
    decltype(&ImageList_Destroy) destroy = ImageList_Destroy;
    decltype(&ImageList_ReplaceIcon) replace_icon = ImageList_ReplaceIcon;
    decltype(&ImageList_AddMasked) add_masked = ImageList_AddMasked;

    void resolve()
    {
        HMODULE m = LoadLibraryW(L"comctl32.dll");  // under the activation context: v6
        if (!m) return;
        if (auto f = reinterpret_cast<decltype(&ImageList_Create)>(reinterpret_cast<void*>(GetProcAddress(m, "ImageList_Create")))) create = f;
        if (auto f = reinterpret_cast<decltype(&ImageList_Destroy)>(reinterpret_cast<void*>(GetProcAddress(m, "ImageList_Destroy")))) destroy = f;
        if (auto f = reinterpret_cast<decltype(&ImageList_ReplaceIcon)>(reinterpret_cast<void*>(GetProcAddress(m, "ImageList_ReplaceIcon")))) replace_icon = f;
        if (auto f = reinterpret_cast<decltype(&ImageList_AddMasked)>(reinterpret_cast<void*>(GetProcAddress(m, "ImageList_AddMasked")))) add_masked = f;
    }
};
comctl6 g_comctl;

// -- handlers -----------------------------------------------------------------

// Anything that receives notifications from its HWND via the parent window
// (WM_COMMAND, WM_NOTIFY, WM_HSCROLL, WM_DRAWITEM, WM_CTLCOLOR*).
class handler
{
public:
    virtual ~handler() = default;
    virtual void command(WORD /*code*/) {}
    // With the sender's id, for a control that sends several (a toolbar).
    virtual void command(WORD code, WORD /*id*/) { command(code); }
    virtual LRESULT notify(NMHDR*, bool& handled)
    {
        handled = false;
        return 0;
    }
    virtual void scroll(WORD /*code*/) {}
    virtual bool draw_item(DRAWITEMSTRUCT*) { return false; }
    // The desktop's colours or dark mode changed (after theme_control).
    virtual void theme_changed() {}
};

std::unordered_map<HWND, handler*>& handlers()
{
    static std::unordered_map<HWND, handler*> map;
    return map;
}

handler* handler_of(HWND h)
{
    auto& m = handlers();
    auto it = m.find(h);
    return it == m.end() ? nullptr : it->second;
}

class window_impl;  // forward

// ui::set_busy's state, which every window's WM_SETCURSOR reads.
bool app_busy = false;

// WM_SETCURSOR while the app is busy, over a client area: the app-starting
// pointer, true if it's set. Windows that give their own pointer (a canvas,
// a splitter bar) ask this first, as they don't ask their parent.
bool busy_cursor_here(LPARAM lp)
{
    if (!app_busy || LOWORD(lp) != HTCLIENT) return false;
    SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
    return true;
}

// Messages the containers forward to their children's handlers.
// Paints the frames of the group boxes in window `h` (after its background);
// false if it has none.
bool paint_group_frames(HWND h, HDC dc);
void forget_group_themes(HWND h);

// Controls on a tab control's page sit above it, on its themed body, which
// in light mode isn't the window's colour (Windows 10 and 11: a shade
// lighter): their backgrounds take the body's, or each control is a band of
// the window's on it. Measured once per tab control, from what it prints.
std::map<HWND, HBRUSH>& tab_bodies()
{
    static std::map<HWND, HBRUSH> m;
    return m;
}
unsigned g_tab_bodies_gen = 0;  // bumped as they're forgotten
void forget_tab_bodies()
{
    for (auto& [tab, brush] : tab_bodies()) DeleteObject(brush);
    tab_bodies().clear();
    ++g_tab_bodies_gen;
}
void traced_tab_adjust(HWND, BOOL, RECT*);
HBRUSH tab_body_brush(HWND tab)
{
    auto& m = tab_bodies();
    if (auto it = m.find(tab); it != m.end()) return it->second;
    RECT client;
    GetClientRect(tab, &client);
    RECT body = client;
    traced_tab_adjust(tab, FALSE, &body);
    if (client.right <= 0 || client.bottom <= 0 || body.right <= body.left || body.bottom <= body.top) return nullptr;
    HDC screen = GetDC(tab), mem = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    HGDIOBJ old = SelectObject(mem, bitmap);
    SendMessageW(tab, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT | PRF_ERASEBKGND);
    const COLORREF colour = GetPixel(mem, (body.left + body.right) / 2, (body.top + body.bottom) / 2);
    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(tab, screen);
    if (colour == CLR_INVALID) return nullptr;
    return m[tab] = CreateSolidBrush(colour);
}
// The body brush of a visible tab control `control` lies wholly over (a
// sibling, as its pages' controls are), if any.
void traced_tab_adjust(HWND h, BOOL larger, RECT* rect)
{
    trace::scope timing(trace::phase::tab_adjust, uintptr_t(h));
    TabCtrl_AdjustRect(h, larger, rect);
}

HBRUSH tab_body_under_scan(HWND control);
// Asked by each label as it paints (WM_CTLCOLORSTATIC): found once per
// control and layout pass, not by a walk over its siblings at every paint.
HBRUSH tab_body_under(HWND control)
{
    struct known
    {
        unsigned pass, gen;
        HBRUSH brush;
    };
    static std::unordered_map<HWND, known> cache;
    if (auto it = cache.find(control);
        it != cache.end() && it->second.pass == g_layout_pass && it->second.gen == g_tab_bodies_gen)
        return it->second.brush;
    trace::mark(trace::phase::tab_cache_miss, uintptr_t(control));
    const HBRUSH b = tab_body_under_scan(control);
    cache[control] = {g_layout_pass, g_tab_bodies_gen, b};
    return b;
}
HBRUSH tab_body_under_scan(HWND control)
{
    RECT at;
    GetWindowRect(control, &at);
    for (HWND s = GetWindow(control, GW_HWNDFIRST); s; s = GetWindow(s, GW_HWNDNEXT))
    {
        if (s == control || !IsWindowVisible(s)) continue;
        wchar_t cls[32] = {};
        GetClassNameW(s, cls, 32);
        if (_wcsicmp(cls, WC_TABCONTROLW)) continue;
        RECT body;
        GetClientRect(s, &body);
        traced_tab_adjust(s, FALSE, &body);
        MapWindowPoints(s, nullptr, reinterpret_cast<POINT*>(&body), 2);
        RECT overlap;
        if (IntersectRect(&overlap, &body, &at) && EqualRect(&overlap, &at)) return tab_body_brush(s);
    }
    return nullptr;
}

bool container_tooltip_notify(HWND, LPARAM);

bool forward_child_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result)
{
    if (msg == WM_NOTIFY && container_tooltip_notify(wnd, lp))
    {
        result = 0;
        return true;
    }
    switch (msg)
    {
    case WM_COMMAND:
        if (lp)
            if (handler* h = handler_of(reinterpret_cast<HWND>(lp)))
            {
                h->command(HIWORD(wp), LOWORD(wp));
                result = 0;
                return true;
            }
        return false;
    case WM_NOTIFY:
    {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (handler* h = handler_of(hdr->hwndFrom))
        {
            bool handled = true;
            result = h->notify(hdr, handled);
            return handled;
        }
        // List headers notify via the list's own window.
        if (handler* h = handler_of(GetParent(hdr->hwndFrom)))
        {
            bool handled = true;
            result = h->notify(hdr, handled);
            return handled;
        }
        return false;
    }
    case WM_HSCROLL:
    case WM_VSCROLL:
        if (lp)
            if (handler* h = handler_of(reinterpret_cast<HWND>(lp)))
            {
                h->scroll(LOWORD(wp));
                result = 0;
                return true;
            }
        return false;
    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (handler* h = handler_of(dis->hwndItem))
            if (h->draw_item(dis))
            {
                result = TRUE;
                return true;
            }
        return false;
    }
    case WM_THEMECHANGED:
        forget_group_themes(wnd);
        forget_tab_bodies();
        return false;
    case WM_ERASEBKGND:
    {
        // The window's background, with the frames of its group boxes, in
        // one step (so a frame's line is never erased and redrawn later).
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT c;
        GetClientRect(wnd, &c);
        const int saved = SaveDC(dc);
        FillRect(dc, &c, theme_brush(surface_of(wnd)));
        paint_group_frames(wnd, dc);
        RestoreDC(dc, saved);
        result = 1;
        return true;
    }
    case WM_CTLCOLORSTATIC:
        trace::mark(trace::phase::control_colour, uintptr_t(reinterpret_cast<HWND>(lp)));
        [[fallthrough]];
    case WM_CTLCOLORBTN:
    {
        // Read-only multi-line text keeps the text background; statics and
        // checkboxes sit transparently on the window's.
        HWND c = reinterpret_cast<HWND>(lp);
        if (GetWindowLongPtrW(c, GWL_STYLE) & ES_MULTILINE)
        {
            wchar_t cls[16] = {};
            GetClassNameW(c, cls, 16);
            // A selectable label's edit is on the window's, as a label.
            if (!_wcsicmp(cls, L"Edit") && GetPropW(c, L"rapport-label"))
            {
                SetTextColor(
                    reinterpret_cast<HDC>(wp),
                    color_ref(theme_color_of(IsWindowEnabled(c) ? surface_text_of(wnd)
                                                                : theme_color::disabled_text)));
                SetBkColor(reinterpret_cast<HDC>(wp),
                           color_ref(theme_color_of(surface_of(wnd))));
                result = reinterpret_cast<LRESULT>(theme_brush(surface_of(wnd)));
                return true;
            }
            if (!_wcsicmp(cls, L"Edit"))
            {
                SetTextColor(reinterpret_cast<HDC>(wp),
                             color_ref(theme_color_of(theme_color::text)));
                SetBkColor(reinterpret_cast<HDC>(wp), color_ref(theme_color_of(theme_color::base)));
                result = reinterpret_cast<LRESULT>(theme_brush(theme_color::base));
                return true;
            }
        }
        // (Themed check boxes ignore the text colour: theirs is drawn by
        // check_impl in dark mode. A link takes it, as theme_control asks
        // it to: the link colour, the accent's.)
        wchar_t cls[16] = {};
        GetClassNameW(c, cls, 16);
        const bool link = !_wcsicmp(cls, WC_LINK);
        SetTextColor(reinterpret_cast<HDC>(wp),
                     color_ref(theme_color_of(!IsWindowEnabled(c) ? theme_color::disabled_text
                                              : link              ? theme_color::link
                                                                  : surface_text_of(wnd))));
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        HBRUSH on_tab = dark_mode() ? nullptr : tab_body_under(c);
        result = reinterpret_cast<LRESULT>(on_tab ? on_tab : theme_brush(surface_of(wnd)));
        return true;
    }
    // Fields and drop-down lists, dark in dark mode (their defaults are
    // the light system colours).
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    {
        if (!dark_mode()) return false;
        SetTextColor(reinterpret_cast<HDC>(wp), color_ref(theme_color_of(theme_color::text)));
        SetBkColor(reinterpret_cast<HDC>(wp), color_ref(theme_color_of(theme_color::base)));
        result = reinterpret_cast<LRESULT>(theme_brush(theme_color::base));
        return true;
    }
    }
    return false;
}

// A plain child window that hosts other controls (scroll viewports), passing
// their notifications on.
HWND g_printing_pane = nullptr;  // a pane's panel being drawn off screen (paint_pane)

// The non-client areas (frames, scroll bars) of the controls on a pane's
// panel, as shown, in the panel's coordinates.
struct pane_frames
{
    HWND panel;
    HRGN shown;
    std::vector<HWND> windows;
    bool failed = false;  // the region couldn't be made: no copy, then
};

BOOL CALLBACK collect_pane_frame(HWND h, LPARAM lp)
{
    auto* f = reinterpret_cast<pane_frames*>(lp);
    // Only those a control paints in its WM_NCPAINT (an etched separator's
    // edge is non-client too, but it draws that in its WM_PAINT).
    const bool client_edge = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) != 0;
    const bool scroll_bars = (GetWindowLongPtrW(h, GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL)) != 0;
    if ((!client_edge && !scroll_bars) || !IsWindowVisible(h)) return TRUE;
    RECT outer, inner;
    GetWindowRect(h, &outer);
    GetClientRect(h, &inner);
    MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&inner), 2);
    if (EqualRect(&outer, &inner)) return TRUE;
    // Only what shows: the windows it's inside, up to the panel, cut it off.
    RECT shown = outer;
    for (HWND up = GetParent(h); up; up = up == f->panel ? nullptr : GetParent(up))
    {
        RECT c;
        GetClientRect(up, &c);
        MapWindowPoints(up, nullptr, reinterpret_cast<POINT*>(&c), 2);
        IntersectRect(&shown, &shown, &c);
    }
    if (IsRectEmpty(&shown)) return TRUE;
    IntersectRect(&inner, &inner, &shown);
    f->windows.push_back(h);
    HRGN ring = CreateRectRgnIndirect(&shown);
    HRGN hole = CreateRectRgnIndirect(&inner);
    POINT origin{};
    ClientToScreen(f->panel, &origin);
    if (!f->shown || !ring || !hole || CombineRgn(ring, ring, hole, RGN_DIFF) == ERROR ||
        OffsetRgn(ring, -origin.x, -origin.y) == ERROR || CombineRgn(f->shown, f->shown, ring, RGN_OR) == ERROR)
        f->failed = true;
    if (ring) DeleteObject(ring);
    if (hole) DeleteObject(hole);
    return TRUE;
}

// A scroll pane's panel and everything on it drawn off screen (WM_PRINT: its
// background, then its controls in turn, into the one bitmap), then put on
// screen in one go: its controls, moved and sized by a layout pass without
// painting (see moves_in_one_pass), otherwise showed landing one at a time.
// It's its WM_PAINT, which comes when nothing else is waiting: dragging a
// splitter, the pane's last pixels (moved along with it) show until it's
// drawn, and the panes beside it keep up with the pointer meanwhile.
// True if it drew the panel and its controls are on screen as drawn here;
// false (nothing done: their own paints still due) if the off-screen copy
// couldn't be made.
bool paint_pane(HWND panel)
{
    RECT c;
    GetClientRect(panel, &c);
    if (c.right <= 0 || c.bottom <= 0) return false;
    if (trace::enabled.load(std::memory_order_relaxed))
    {
        trace::mark(trace::phase::pane_area, uint64_t(c.right) * c.bottom);
        RECT host{}, at = c, visible{};
        GetClientRect(GetParent(panel), &host);
        MapWindowPoints(panel, GetParent(panel), reinterpret_cast<POINT*>(&at), 2);
        IntersectRect(&visible, &host, &at);
        trace::mark(trace::phase::pane_visible_area,
                    uint64_t(visible.right - visible.left) * (visible.bottom - visible.top));
    }
    // RAPPORT_PANE_FAIL: as if the off-screen copy couldn't be made, to test
    // the fallback.
    static const bool fail = std::getenv("RAPPORT_PANE_FAIL") != nullptr;
    if (fail) return false;
    // The whole panel copied, through a DC not cut to its update region: its
    // controls' paints are then all done here, wherever they were due.
    HDC screen = GetDC(panel);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bitmap = mem ? CreateCompatibleBitmap(screen, c.right, c.bottom) : nullptr;
    HGDIOBJ old = bitmap ? SelectObject(mem, bitmap) : nullptr;
    bool drawn = false;
    pane_frames frames{panel, CreateRectRgn(0, 0, 0, 0), {}};
    if (old && old != HGDI_ERROR)
        EnumChildWindows(panel, collect_pane_frame, reinterpret_cast<LPARAM>(&frames));
    // Copied with those frames left out, or not at all (they'd show classic).
    if (old && old != HGDI_ERROR && !frames.failed &&
        (frames.windows.empty() || ExtSelectClipRgn(screen, frames.shown, RGN_DIFF) != ERROR))
    {
        g_printing_pane = panel;
        {
            trace::scope timing(trace::phase::pane_print, uintptr_t(panel));
            SendMessageW(panel, WM_PRINT, reinterpret_cast<WPARAM>(mem),
                         PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
        }
        g_printing_pane = nullptr;
        {
            trace::scope timing(trace::phase::pane_blit, uintptr_t(panel));
            // Not over the controls' frames (clipped out above): printed, a
            // themed one comes out as the classic sunken edge, and it's
            // painted for real below.
            drawn = BitBlt(screen, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY) != FALSE;
        }
    }
    if (old && old != HGDI_ERROR) SelectObject(mem, old);
    if (bitmap) DeleteObject(bitmap);
    if (mem) DeleteDC(mem);
    ReleaseDC(panel, screen);
    if (frames.shown) DeleteObject(frames.shown);
    // Frames too (RDW_NOFRAME, else a control moved or resized keeps its
    // frame's paint due and is sent WM_PAINT for it, painting itself again
    // on screen after the copy). Each is then painted by the control itself,
    // which only its WM_NCPAINT can do: a client edge's themed look, its
    // state (focus, hot) and its scroll bars aren't what WM_PRINT draws.
    if (drawn)
    {
        RedrawWindow(panel, nullptr, nullptr, RDW_VALIDATE | RDW_NOERASE | RDW_NOFRAME | RDW_ALLCHILDREN);
        for (HWND h : frames.windows) SendMessageW(h, WM_NCPAINT, 1, 0);
    }
    // Its host, moved without redrawing too (move_pane_window): its scroll
    // bar, and any of it the panel doesn't cover.
    if (HWND host = GetParent(panel)) RedrawWindow(host, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_NOCHILDREN);
    return drawn;
}

LRESULT CALLBACK host_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r = 0;
    auto* cause = (msg == WM_PAINT || msg == WM_ERASEBKGND)
                      ? static_cast<uint64_t*>(GetPropW(h, L"rapport-pane"))
                      : nullptr;
    if (cause)
    {
        if (msg == WM_ERASEBKGND && g_printing_pane != h) return 1;  // drawn whole in WM_PAINT
        if (msg == WM_PAINT)
        {
            PAINTSTRUCT ps;
            trace::scope timing(trace::phase::pane_paint, uintptr_t(h), std::exchange(*cause, 0));
            HDC dc = BeginPaint(h, &ps);
            // Not drawn off screen: its background as usual, and its controls
            // paint themselves, each made to (moved without painting by a
            // layout pass, they may have none due, and the background just
            // filled over them, the panel not clipping its children). The
            // panel's own paint is done: no loop if the copy fails each time.
            if (!paint_pane(h))
            {
                FillRect(dc, &ps.rcPaint, theme_brush(surface_of(h)));
                for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
                    RedrawWindow(c, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
            }
            EndPaint(h, &ps);
            return 0;
        }
    }
    if (forward_child_message(h, msg, wp, lp, r)) return r;
    if (auto* fn = reinterpret_cast<std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>*>(
            GetWindowLongPtrW(h, GWLP_USERDATA)))
    {
        bool handled = false;
        r = (*fn)(h, msg, wp, lp, handled);
        if (handled) return r;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void register_classes()
{
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.hInstance = instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpfnWndProc = host_proc;
    wc.lpszClassName = L"rapportHost";
    RegisterClassExW(&wc);
    // A base surface window's (surface_of).
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"rapportHostBase";
    RegisterClassExW(&wc);
}

void track_container_control(HWND h);

HWND make_host(HWND parent, DWORD style = 0)
{
    register_classes();
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT,
                             surface_of(parent) == theme_color::base ? L"rapportHostBase" : L"rapportHost", L"",
                             WS_CHILD | WS_CLIPCHILDREN | style, 0, 0, 0, 0, parent, nullptr,
                             instance(), nullptr);
    track_container_control(h);
    if (dark_mode()) theme_control(h);  // its scroll bars
    return h;
}

// -- flicker-free painting ----------------------------------------------------

// A control that paints through an off-screen buffer: its erase is skipped
// and WM_PAINT renders it (WM_PRINTCLIENT) into a buffer that reaches the
// screen in one step. For controls that erase and then draw, and so flash
// whenever a layout moves or resizes them: links, list headers.
enum class buffer_bg
{
    own,     // the control fills its own background
    parent,  // the buffer starts as the parent's background (a link)
};

void paint_buffered(HWND h, buffer_bg bg)
{
    static const bool ready = SUCCEEDED(BufferedPaintInit());
    if (!ready) return;
    SetWindowSubclass(
        h,
        [](HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) -> LRESULT
        {
            if (msg == WM_ERASEBKGND) return 1;
            if (msg != WM_PAINT) return DefSubclassProc(h, msg, wp, lp);
            const auto bg = static_cast<buffer_bg>(data);
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT r;
            GetClientRect(h, &r);
            HDC mem = nullptr;
            if (HPAINTBUFFER buffer = BeginBufferedPaint(dc, &r, BPBF_COMPATIBLEBITMAP, nullptr, &mem))
            {
                if (bg != buffer_bg::own) DrawThemeParentBackground(h, mem, &r);
                DefSubclassProc(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT | PRF_ERASEBKGND);
                EndBufferedPaint(buffer, TRUE);
            }
            else
                DefSubclassProc(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
            EndPaint(h, &ps);
            return 0;
        },
        0x42, DWORD_PTR(bg));
}

// -- tooltips -----------------------------------------------------------------

HWND window_tooltip(HWND top)
{
    HWND tip = static_cast<HWND>(GetPropW(top, L"rapport-tooltip"));
    if (!tip)
    {
        tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, top, nullptr, instance(),
                              nullptr);
        SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, 480);
        if (dark_mode()) theme_control(tip);
        frame_popup(tip);
        SetPropW(top, L"rapport-tooltip", tip);
    }
    return tip;
}

void apply_tooltip(HWND control, const std::string& text)
{
    HWND top = GetAncestor(control, GA_ROOT);
    HWND tip = window_tooltip(top);
    std::wstring w = wide(text);
    TOOLINFOW ti{};
    ti.cbSize = TTTOOLINFOW_V2_SIZE;  // also accepted by comctl32 v5
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = top;
    ti.uId = reinterpret_cast<UINT_PTR>(control);
    ti.lpszText = w.data();
    SendMessageW(tip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    if (!text.empty()) SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

// A layout container's children often share an HWND parent with controls
// outside it. Track logical ancestry independently of native parenting.
struct container_state
{
    HWND parent;
    container_state* outer;
    std::vector<container_state*> children;
    struct control
    {
        bool enabled = true;
        std::string tooltip;
    };
    std::map<HWND, control> controls;
    bool enabled = true, shown = false, registered = false;
    std::string tooltip;
    RECT rect{};

    container_state(HWND p, container_state* o) : parent(p), outer(o)
    {
        if (outer) outer->children.push_back(this);
    }
    bool effective_enabled() const { return enabled && (!outer || outer->effective_enabled()); }
    const std::string& effective_tip() const
    {
        return tooltip.empty() && outer ? outer->effective_tip() : tooltip;
    }
    void refresh_enabled()
    {
        const bool on = effective_enabled();
        for (const auto& [h, own] : controls) EnableWindow(h, on && own.enabled);
        for (auto* child : children) child->refresh_enabled();
    }
    TOOLINFOW tool() const
    {
        TOOLINFOW ti{};
        ti.cbSize = TTTOOLINFOW_V2_SIZE;
        ti.hwnd = parent;
        ti.uId = reinterpret_cast<UINT_PTR>(this);
        ti.rect = rect;
        ti.uFlags = TTF_SUBCLASS;
        return ti;
    }
    void update_gap()
    {
        if (!registered && (!shown || effective_tip().empty() || IsRectEmpty(&rect))) return;
        auto ti = tool();
        HWND tip = window_tooltip(GetAncestor(parent, GA_ROOT));
        if (!shown || effective_tip().empty() || IsRectEmpty(&rect))
        {
            SendMessageW(tip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
            registered = false;
            return;
        }
        // Nested rectangular tools can overlap. Whichever tool the tooltip
        // chooses, ask the logical tree for the nearest container at the point.
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        if (registered) SendMessageW(tip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
        else registered = SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti)) != 0;
    }
    const std::string* tip_at(HWND host, POINT pt) const
    {
        if (!shown || (parent == host && !PtInRect(&rect, pt))) return nullptr;
        for (auto* child : children)
            if (auto* hit = child->tip_at(host, pt)) return hit;
        return parent == host ? &effective_tip() : nullptr;
    }
    void refresh_tips()
    {
        update_gap();
        for (const auto& [h, own] : controls)
            if (own.tooltip.empty()) apply_tooltip(h, effective_tip());
        for (auto* child : children)
            if (child->tooltip.empty()) child->refresh_tips();
    }
};

bool container_tooltip_notify(HWND wnd, LPARAM lp)
{
    auto* info = reinterpret_cast<NMTTDISPINFOW*>(lp);
    if (info->hdr.code != TTN_GETDISPINFOW ||
        info->hdr.hwndFrom != GetPropW(GetAncestor(wnd, GA_ROOT), L"rapport-tooltip") ||
        (info->uFlags & TTF_IDISHWND)) return false;
    auto* scope = reinterpret_cast<container_state*>(info->hdr.idFrom);
    while (scope->outer) scope = scope->outer;
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(wnd, &pt);
    static thread_local std::wstring text;
    const auto* found = scope->tip_at(wnd, pt);
    text = found ? wide(*found) : L"";
    info->lpszText = text.data();
    return true;
}

thread_local container_state* building_controls = nullptr;
class control_scope
{
public:
    explicit control_scope(container_state* s) : old_(std::exchange(building_controls, s)) {}
    ~control_scope() { building_controls = old_; }
private:
    container_state* old_;
};

container_state* control_container(HWND h)
{
    return static_cast<container_state*>(GetPropW(h, L"rapport-container"));
}

void track_container_control(HWND h)
{
    if (!h || !building_controls) return;
    auto& scope = *building_controls;
    scope.controls[h].enabled = IsWindowEnabled(h) != FALSE;
    SetWindowSubclass(h, [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) -> LRESULT
    {
        if (msg == WM_NCDESTROY)
        {
            reinterpret_cast<container_state*>(data)->controls.erase(hwnd);
            RemovePropW(hwnd, L"rapport-container");
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }, 0, reinterpret_cast<DWORD_PTR>(&scope));
    SetPropW(h, L"rapport-container", &scope);
    if (!scope.effective_enabled()) EnableWindow(h, FALSE);
    if (!scope.effective_tip().empty()) apply_tooltip(h, scope.effective_tip());
}

void set_control_enabled(HWND h, bool enabled)
{
    if (auto* scope = control_container(h))
    {
        scope->controls.at(h).enabled = enabled;
        enabled = enabled && scope->effective_enabled();
    }
    EnableWindow(h, enabled);
}

void set_tooltip_text(HWND h, const std::string& text)
{
    if (auto* scope = control_container(h))
    {
        scope->controls.at(h).tooltip = text;
        if (text.empty()) return apply_tooltip(h, scope->effective_tip());
    }
    apply_tooltip(h, text);
}

// -- menus --------------------------------------------------------------------

// Items' icons, as bitmaps the menu shows; they go when the menu does.
struct menu_bitmaps
{
    struct free_bitmap
    {
        void operator()(HBITMAP b) const { DeleteObject(b); }
    };
    std::vector<std::unique_ptr<std::remove_pointer_t<HBITMAP>, free_bitmap>> held;
    UINT dpi = 96;
};

// `icon` for a menu at `dpi`: menus blend their bitmaps premultiplied.
HBITMAP menu_bitmap(const icon& icon, UINT dpi)
{
    HBITMAP bmp = render_icon(icon, MulDiv(16, int(dpi), 96), float(dpi) / 96);
    DIBSECTION ds{};
    if (!bmp || GetObjectW(bmp, sizeof ds, &ds) != sizeof ds || !ds.dsBm.bmBits) return bmp;
    auto* p = static_cast<uint8_t*>(ds.dsBm.bmBits);
    for (long i = 0; i < long(ds.dsBm.bmWidth) * std::abs(ds.dsBm.bmHeight); ++i, p += 4)
        for (int c = 0; c < 3; ++c) p[c] = uint8_t((p[c] * p[3] + 127) / 255);
    return bmp;
}

void set_menu_icon(HMENU m, UINT item, BOOL by_position, const icon& icon, menu_bitmaps* bitmaps)
{
    if (!icon || !bitmaps) return;
    HBITMAP bmp = menu_bitmap(icon, bitmaps->dpi);
    if (!bmp) return;
    bitmaps->held.emplace_back(bmp);
    MENUITEMINFOW mi{sizeof mi};
    mi.fMask = MIIM_BITMAP;
    mi.hbmpItem = bmp;
    SetMenuItemInfoW(m, item, by_position, &mi);
}

// Appends `items` to `m`; command id `base + n` runs actions[n - 1]. Icons
// only with `bitmaps`, which keeps them.
void fill_menu(HMENU m, const menu& items, std::vector<std::function<void()>>& actions,
               UINT base = 0, menu_bitmaps* bitmaps = nullptr)
{
    for (const menu_item& it : items)
    {
        if (it.is_separator())
        {
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        std::wstring label = mnemonic(it.label);
        if (!it.shortcut.empty()) label += L"\t" + wide(rap::i18n::shortcut_label(it.shortcut));
        UINT flags = MF_STRING | (it.checked && *it.checked ? MF_CHECKED : 0);
        if (!it.children.empty())
        {
            HMENU sub = CreatePopupMenu();
            fill_menu(sub, it.children, actions, base, bitmaps);
            AppendMenuW(m, flags | (it.enabled ? 0 : MF_GRAYED) | MF_POPUP, reinterpret_cast<UINT_PTR>(sub),
                        label.c_str());
            set_menu_icon(m, UINT(GetMenuItemCount(m) - 1), TRUE, it.icon, bitmaps);
            continue;
        }
        actions.push_back(it.action);
        const UINT id = base + UINT(actions.size());
        AppendMenuW(m, flags | (it.enabled && it.action ? 0 : MF_GRAYED), id, label.c_str());
        if (it.radio)
        {
            MENUITEMINFOW mi{sizeof mi};
            mi.fMask = MIIM_FTYPE;
            mi.fType = MFT_STRING | MFT_RADIOCHECK;
            SetMenuItemInfoW(m, id, FALSE, &mi);
        }
        set_menu_icon(m, id, FALSE, it.icon, bitmaps);
    }
}

HMENU build_menu(const menu& items, std::vector<std::function<void()>>& actions,
                 menu_bitmaps* bitmaps = nullptr)
{
    HMENU m = CreatePopupMenu();
    fill_menu(m, items, actions, 0, bitmaps);
    return m;
}

// `anchor`: the button the menu drops from, if any. The click on it that
// closes the menu is kept from opening it again (menu_dismiss.h), so the
// second click closes the menu as a combo box's does.
void show_popup_menu(HWND owner, const menu& items, POINT screen, HWND anchor = nullptr)
{
    if (items.empty()) return;
    std::vector<std::function<void()>> actions;
    menu_bitmaps bitmaps{.dpi = dpi_of(owner)};
    HMENU m = build_menu(items, actions, &bitmaps);
    UINT id = 0;
    rap::win32::menu_press press;
    {
        rap::win32::menu_press_watch watch;
        id = UINT(TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, owner, nullptr));
        press = watch.last();
    }
    DestroyMenu(m);
    // Only a menu closed without a choice can have been closed by a click
    // outside it.
    if (id == 0) rap::win32::drop_menu_press(anchor, press);
    if (id > 0 && id <= actions.size() && actions[id - 1]) actions[id - 1]();
}

// -- window base --------------------------------------------------------------

// What every widget needs from its window: scale, fonts, relayout.
class win_context
{
public:
    virtual ~win_context() = default;
    virtual HWND hwnd() const = 0;
    virtual void relayout() = 0;
    // Lays out and repaints now, in one go (a different tab page showing).
    virtual void relayout_at_once() = 0;
    // A layout still to come, now: for a size asked for before it.
    virtual void flush_layout() {}
    float scale() const { return scale_of(hwnd()); }
    UINT dpi() const { return dpi_of(hwnd()); }
    int px(float logical) const { return int(std::lround(logical * scale())); }
};

SIZE text_extent(HWND h, const std::wstring& s, bool multiline = false, int max_width = 0)
{
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    RECT r{0, 0, max_width > 0 ? max_width : 100000, 0};
    DrawTextW(dc, s.empty() ? L" " : s.c_str(), -1, &r,
              DT_CALCRECT | DT_NOPREFIX | (multiline ? DT_WORDBREAK : DT_SINGLELINE));
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return {r.right - r.left, r.bottom - r.top};
}

// Moving controls. A control already where it belongs isn't touched (each
// move costs an accessibility event, milliseconds apiece whenever an
// automation client listens); during a layout pass the rest are batched
// per parent and applied together.
struct move_batch
{
    std::map<HWND, std::vector<std::pair<HWND, RECT>>> moves;  // parent -> its children's moves
    std::vector<std::pair<HWND, bool>> shown;                  // controls shown or hidden, once moved
    std::set<HWND> panes;  // scroll panes' panels resized in the pass: drawn off screen when they paint (paint_pane)
    // For a size the window doesn't have yet (see window::resize): nothing
    // moves or shows, and canvases draw for where they'll be.
    bool ahead = false;
};
move_batch* g_moves = nullptr;

// A control that only moves carries its pixels along (no repaint); one that
// changes size, or has a repaint pending (a label whose text changed as it
// moved), repaints whole, or it would show a mix of old and new.
constexpr UINT move_flags = SWP_NOZORDER | SWP_NOACTIVATE;
UINT move_flags_for(HWND h, int w, int hgt)
{
    RECT r;
    GetWindowRect(h, &r);
    const bool same_size = r.right - r.left == w && r.bottom - r.top == hgt;
    return same_size && !GetUpdateRect(h, nullptr, FALSE) ? move_flags : move_flags | SWP_NOCOPYBITS;
}

bool placed_at(HWND h, int x, int y, int w, int hgt)
{
    RECT r;
    GetWindowRect(h, &r);
    MapWindowPoints(HWND_DESKTOP, GetParent(h), reinterpret_cast<POINT*>(&r), 2);
    const bool same = r.left == x && r.top == y && r.right - r.left == w && r.bottom - r.top == hgt;
    trace::mark(trace::phase::move_decision, same ? 0 : 1);
    return same;
}

// Now, for containers that size their contents from their own client area.
void move_now(HWND h, int x, int y, int w, int hgt)
{
    if (g_moves && g_moves->ahead) return;
    // A host that changes size repaints whole: carrying its old pixels
    // along, it kept what lay over it (a splitter's bar, a neighbour's
    // edge) in strips its children never repaint.
    if (!placed_at(h, x, y, w, hgt))
    {
        traced_set_window_pos(h, nullptr, x, y, w, hgt, move_flags_for(h, w, hgt));
    }
}

// Where hidden controls go, kept until they show (show_now): a hidden
// control moved or sized still runs its own handling of that, and a
// trackbar's or a label's is slow. A pane's hidden tab pages hold most of
// its controls, and dragging a splitter moved them all at every step.
std::unordered_map<HWND, RECT> g_parked;

// Set while a window's controls are moved to where they'll lie across a
// resize (window::resize): a canvas taking its size then neither tells the
// app nor draws, keeping the frame it drew for the size it's about to have.
bool g_canvases_quiet = false;

void move_control(HWND h, int x, int y, int w, int hgt)
{
    if (!(GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE))
    {
        g_parked[h] = RECT{x, y, x + w, y + hgt};
        return;
    }
    g_parked.erase(h);
    if (placed_at(h, x, y, w, hgt)) return;
    if (g_moves)
        g_moves->moves[GetParent(h)].push_back({h, RECT{x, y, x + w, y + hgt}});
    else
    {
        traced_set_window_pos(h, nullptr, x, y, w, hgt, move_flags_for(h, w, hgt));
    }
}

void show_now(HWND h, bool shown);

// The scroll pane's panel `h` lies on (or is), if any: what's drawn off
// screen and shown in one go (see paint_pane).
HWND pane_of(HWND h)
{
    for (; h; h = GetParent(h))
        if (GetPropW(h, L"rapport-pane")) return h;
    return nullptr;
}

// Batches the moves of a layout pass (nested passes join the outer one),
// and the shows and hides, made once the moves are.
void redraw_now(HWND h, const RECT* rect, HRGN region, UINT flags)
{
    trace::scope timing(trace::phase::redraw, uintptr_t(h));
    RedrawWindow(h, rect, region, flags);
}

class moves_in_one_pass
{
public:
    explicit moves_in_one_pass(bool ahead = false)
    {
        if (!g_moves) g_moves = &batch_, owner_ = true, batch_.ahead = ahead;
    }
    ~moves_in_one_pass()
    {
        if (!owner_) return;
        g_moves = nullptr;
        if (batch_.ahead) return;
        trace::scope batch_time(trace::phase::child_moves, batch_.moves.size());
        // On a scroll pane's panel, controls move without painting, and the
        // panel's drawn off screen when it next paints, then shown in one go
        // (paint_pane).
        std::set<HWND> buffered;
        for (auto& [parent, list] : batch_.moves)
        {
            const HWND whole = pane_of(parent);
            if (whole) buffered.insert(whole);
            // Once each: a control placed twice in a pass (a scroll pane's
            // content laid out as its host took its size, then again) moved
            // twice, and every move of a shown window costs (its clip and
            // visible regions, its neighbours', the compositor's), more than
            // what the control itself does with its new size.
            std::unordered_map<HWND, size_t> last;
            for (size_t i = 0; i < list.size(); ++i) last[list[i].first] = i;
            HDWP dwp = BeginDeferWindowPos(int(last.size()));
            for (size_t i = 0; i < list.size(); ++i)
            {
                const auto& [h, r] = list[i];
                if (last[h] != i || !dwp) continue;
                trace::scope move_time(trace::phase::window_move, uintptr_t(h));
                dwp = DeferWindowPos(dwp, h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                     move_flags_for(h, r.right - r.left, r.bottom - r.top) | (whole ? SWP_NOREDRAW : 0));
            }
            if (dwp)
            {
                trace::scope timing(trace::phase::end_defer, uintptr_t(parent));
                EndDeferWindowPos(dwp);
            }
        }
        for (auto [h, shown] : batch_.shown) show_now(h, shown);
        buffered.insert(batch_.panes.begin(), batch_.panes.end());
        // Then painted, all of them, before anything else runs: left to their
        // WM_PAINTs, which come only once the queue is idle, they landed a
        // band at a time over several frames (a pane's controls, top to
        // bottom, while the app was busy). Not the panes: nothing on one
        // paints in this update (a trackbar invalidates itself as it takes a
        // size), as a pane is drawn whole, off screen. Their frames' paints
        // too (RDW_NOFRAME): a window moved or resized has one due, which
        // RDW_VALIDATE alone leaves, and it was sent WM_PAINT for it in the
        // update (a pane's canvas rendering there, then again in the print).
        for (HWND whole : buffered) RedrawWindow(whole, nullptr, nullptr, RDW_VALIDATE | RDW_NOERASE | RDW_NOFRAME | RDW_ALLCHILDREN);
        std::set<HWND> roots;
        for (auto& [parent, list] : batch_.moves) roots.insert(GetAncestor(parent, GA_ROOT));
        for (auto [h, shown] : batch_.shown)
            if (shown) roots.insert(GetAncestor(h, GA_ROOT));
        for (HWND root : roots)
            if (IsWindowVisible(root))
                redraw_now(root, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
        // The panes after that, drawn when nothing else is waiting
        // (paint_pane), so the windows beside them keep up with a drag.
        // Invalidated before the update above, they were printed in it: a
        // whole-panel print inside every step.
        for (HWND whole : buffered)
        {
            if (batch_time.id())
                if (auto* cause = static_cast<uint64_t*>(GetPropW(whole, L"rapport-pane")))
                    *cause = batch_time.id();
            trace::mark(trace::phase::pane_invalidated, uintptr_t(whole));
            InvalidateRect(whole, nullptr, FALSE);
        }
    }

private:
    trace::scope pass_time_{trace::phase::move_pass};
    move_batch batch_;
    bool owner_ = false;
};

// Shows or hides a control. While its window is still hidden (being built)
// that's just the style bit: ShowWindow raises an accessibility event per
// control, milliseconds apiece whenever an automation client listens.
void show_now(HWND h, bool shown)
{
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    if (bool(style & WS_VISIBLE) == shown) return;
    if (const auto it = g_parked.find(h); shown && it != g_parked.end())
    {
        const RECT r = it->second;
        g_parked.erase(it);
        if (!placed_at(h, r.left, r.top, r.right - r.left, r.bottom - r.top))
            traced_set_window_pos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                  move_flags | SWP_NOCOPYBITS);
    }
    if (IsWindowVisible(GetAncestor(h, GA_ROOT)))
        ShowWindow(h, shown ? SW_SHOWNA : SW_HIDE);
    else
        SetWindowLongPtrW(h, GWL_STYLE, shown ? style | WS_VISIBLE : style & ~WS_VISIBLE);
}

// In a layout pass, once its moves are made: a control shown where it was
// last put, and painted there, shows somewhere it isn't (a pane coming in
// over the image, before the window has grown to hold it).
void show_control(HWND h, bool shown)
{
    if (!g_moves)
        show_now(h, shown);
    else if (!g_moves->ahead)
        g_moves->shown.push_back({h, shown});
}

HWND create_control(win_context& win, HWND parent, const wchar_t* cls, const std::wstring& text, DWORD style,
                   DWORD ex_style = 0)
{
    HWND h = CreateWindowExW(ex_style, cls, text.c_str(), WS_CHILD | style, 0, 0, 0, 0, parent, nullptr,
                             instance(), nullptr);
    track_container_control(h);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font(win.dpi())), FALSE);
    // Light is each control's own default, but a link's colour is ours (the
    // accent's) in either.
    if (dark_mode() || !_wcsicmp(cls, WC_LINK)) theme_control(h);
    return h;
}

// A widget backed by one HWND, laid out as a leaf.
template <class Base>
class leaf : public Base, public item, public handler
{
public:
    leaf(win_context& win)
        : win_(win)
    {
    }
    ~leaf() override
    {
        if (h_)
        {
            handlers().erase(h_);
            DestroyWindow(h_);
        }
    }

    void set_enabled(bool e) override { set_control_enabled(h_, e); }
    void set_visible(bool v) override
    {
        if (visible == v) return;
        visible = v;
        win_.relayout();
    }
    void set_tooltip(const std::string& t) override { set_tooltip_text(h_, t); }
    // Before the window shows, SetFocus can't land: the window keeps the
    // request and focuses it when it activates.
    bool has_focus() const override
    {
        const HWND f = GetFocus();
        return f && (f == h_ || IsChild(h_, f));
    }
    void focus() override
    {
        HWND root = GetAncestor(h_, GA_ROOT);
        if (IsWindowVisible(root))
            SetFocus(h_);
        else
            SetPropW(root, L"rapport-focus", h_);
    }

    void place(RECT r) override
    {
        move_control(h_, r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
    void show_all(bool shown) override { show_control(h_, shown && visible); }

    HWND hwnd() const { return h_; }

protected:
    void adopt(HWND h)
    {
        h_ = h;
        handlers()[h] = this;
    }
    int px(float v) const { return win_.px(v); }

    win_context& win_;
    HWND h_ = nullptr;
};

// A deferred box's content (layout::deferred), built once: when the window
// is first idle after it paints, or when the box is shown before then.
class deferred_build
{
public:
    explicit deferred_build(std::function<void()> build)
        : build_(std::move(build))
    {
    }
    void run()
    {
        if (!build_) return;
        const auto build = std::move(build_);
        build_ = nullptr;
        build();
    }

private:
    std::function<void()> build_;
};

// The same parent-relative rectangle used to lay out the children covers the
// gaps between them for tooltip hit testing.
class container_item : public item
{
public:
    container_state* state = nullptr;
    void placed(RECT r)
    {
        if (state && !(g_moves && g_moves->ahead) && !EqualRect(&r, &state->rect))
        {
            state->rect = r;
            state->update_gap();
        }
    }
    void showing(bool on)
    {
        if (state && state->shown != (on && visible))
        {
            state->shown = on && visible;
            state->update_gap();
        }
    }
};

class plain_widget : public widget
{
public:
    plain_widget(item& item, container_state* state, win_context& win, std::shared_ptr<deferred_build> pending = nullptr)
        : item_(item)
        , state_(state)
        , win_(win)
        , pending_(std::move(pending))
    {
    }
    void set_enabled(bool on) override
    {
        if (!state_ || state_->enabled == on) return;
        state_->enabled = on;
        if (!state_->outer || state_->outer->effective_enabled()) state_->refresh_enabled();
    }
    void set_visible(bool v) override
    {
        if (v && pending_) pending_->run();
        if (item_.visible == v) return;
        item_.visible = v;
        win_.relayout();
    }
    void set_tooltip(const std::string& text) override
    {
        if (!state_ || state_->tooltip == text) return;
        state_->tooltip = text;
        state_->refresh_tips();
    }
    void focus() override {}
    bool has_focus() const override { return false; }

private:
    item& item_;
    container_state* state_;
    win_context& win_;
    std::shared_ptr<deferred_build> pending_;
};

// -- layout containers --------------------------------------------------------

class box_item : public container_item
{
public:
    box_item(orientation dir, int spacing, int margin = 0)
        : dir_(dir)
        , spacing_(spacing)
        , margin_(margin)
    {
    }
    std::vector<item*> children;

    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }

    SIZE measure() override
    {
        long main = 0, cross = 0;
        int n = 0;
        for (item* c : children)
        {
            if (!c->visible) continue;
            const SIZE s = size_of(c);
            main += along(s);
            cross = std::max(cross, across(s));
            ++n;
        }
        if (n > 1) main += spacing_ * (n - 1);
        main += 2 * margin_;
        cross += 2 * (margin_ / 2);
        return dir_ == orientation::horizontal ? SIZE{main, cross} : SIZE{cross, main};
    }

    // In a column, its width and the room it leaves decide how tall text
    // that wraps is; in a row, each child's share of the width does.
    long measure_height(long width) override
    {
        const std::vector<item*> shown = shown_children();
        if (shown.empty()) return natural().cy;
        if (dir_ == orientation::horizontal)
        {
            const std::vector<long> sizes = shares(shown, width - 2 * margin_, 0);
            long h = 0;
            for (size_t i = 0; i < shown.size(); ++i)
                h = std::max(h, height_of(shown[i], sizes[i]));
            return h + 2 * (margin_ / 2);
        }
        const long inner = width - 2 * (margin_ / 2);
        long h = spacing_ * long(shown.size() - 1) + 2 * margin_;
        for (item* c : shown) h += height_of(c, cross_of(c, inner));
        return h;
    }

    void place(RECT r) override
    {
        placed(r);
        if (dir_ == orientation::horizontal)
            InflateRect(&r, -margin_, -(margin_ / 2));
        else
            InflateRect(&r, -(margin_ / 2), -margin_);
        const std::vector<item*> shown = shown_children();
        if (shown.empty()) return;
        const long total = dir_ == orientation::horizontal ? r.right - r.left : r.bottom - r.top;
        const long cross_total = dir_ == orientation::horizontal ? r.bottom - r.top : r.right - r.left;
        const std::vector<long> sizes = shares(shown, total, cross_total);
        long pos = dir_ == orientation::horizontal ? r.left : r.top;
        for (size_t i = 0; i < shown.size(); ++i)
        {
            item* c = shown[i];
            long cross_size, cross_pos;
            if (dir_ == orientation::horizontal)
            {
                // Single-line things sit centred across a row.
                cross_size =
                    c->fills() ? cross_total : std::min(cross_total, height_of(c, sizes[i]));
                cross_pos = r.top + (cross_total - cross_size) / 2;
            }
            else
            {
                // In a column, left-aligned.
                cross_size = cross_of(c, cross_total);
                cross_pos = r.left;
            }
            RECT cr = dir_ == orientation::horizontal
                          ? RECT{pos, cross_pos, pos + sizes[i], cross_pos + cross_size}
                          : RECT{cross_pos, pos, cross_pos + cross_size, pos + sizes[i]};
            c->place(cr);
            pos += sizes[i] + spacing_;
        }
    }

    void show_all(bool shown) override
    {
        showing(shown);
        for (item* c : children) c->show_all(shown && visible);
    }

private:
    SIZE size_of(item* c)
    {
        SIZE s = c->natural();
        if (c->fixed_w > 0) s.cx = c->fixed_w;
        if (c->fixed_h > 0) s.cy = c->fixed_h;
        return s;
    }
    long along(SIZE s) const { return dir_ == orientation::horizontal ? s.cx : s.cy; }
    long across(SIZE s) const { return dir_ == orientation::horizontal ? s.cy : s.cx; }
    static long height_of(item* c, long width)
    {
        return c->fixed_h > 0 ? c->fixed_h : c->height_at(width);
    }
    std::vector<item*> shown_children() const
    {
        std::vector<item*> shown;
        for (item* c : children)
            if (c->visible) shown.push_back(c);
        return shown;
    }
    // A child's width in a column `cross_total` wide: its natural width, or
    // all of it for what fills or would be wider.
    long cross_of(item* c, long cross_total)
    {
        const long w = size_of(c).cx;
        const bool fill = c->fills() || c->expands_in_form();
        return fill || w > cross_total ? cross_total : w;
    }
    // Each shown child's size along the box, `total` long (in a column,
    // `cross_total` wide, which decides how tall wrapped text is).
    std::vector<long> shares(const std::vector<item*>& shown, long total, long cross_total)
    {
        std::vector<long> sizes;
        long used = spacing_ * long(shown.size() - 1);
        int stretch_sum = 0;
        for (item* c : shown)
        {
            sizes.push_back(dir_ == orientation::horizontal
                                ? size_of(c).cx
                                : height_of(c, cross_of(c, cross_total)));
            used += sizes.back();
            stretch_sum += c->stretch;
        }
        long spare = std::max(0L, total - used);
        if (dir_ == orientation::horizontal)
        {
            // As GTK shares out natural sizes: an even share each, the
            // smallest wants first, so one long label can't starve the rest.
            std::vector<size_t> wanting;
            for (size_t i = 0; i < shown.size(); ++i)
                if (shown[i]->wants_wider() > 0) wanting.push_back(i);
            std::ranges::sort(wanting, {}, [&](size_t i) { return shown[i]->wants_wider(); });
            for (size_t k = 0; k < wanting.size() && spare > 0; ++k)
            {
                const long more =
                    std::min(spare / long(wanting.size() - k), shown[wanting[k]]->wants_wider());
                sizes[wanting[k]] += more;
                spare -= more;
            }
        }
        if (stretch_sum > 0)
        {
            long given = 0;
            int seen = 0;
            for (size_t i = 0; i < shown.size(); ++i)
            {
                if (!shown[i]->stretch) continue;
                seen += shown[i]->stretch;
                const long target = spare * seen / stretch_sum;
                sizes[i] += target - given;
                given = target;
            }
        }
        if (used > total)
        {
            // Too small: what overflows gets clipped, the last item first, but
            // what stretches goes before what doesn't, and a size given
            // (width, height) goes last, as GTK and Qt keep it as a minimum:
            // a picture's box squeezed to a sliver, or a link at a row's end
            // cut off while the label beside it could have given way.
            long over = used - total;
            const bool across = dir_ == orientation::horizontal;
            for (int pass = 0; pass < 3 && over > 0; ++pass)
                for (size_t i = shown.size(); i-- > 0 && over > 0;)
                {
                    const item* c = shown[i];
                    const bool given = across ? c->fixed_w > 0 : c->fixed_h > 0;
                    const int rank = given ? 2 : c->stretch > 0 ? 0 : 1;
                    if (rank != pass) continue;
                    const long cut = std::min(over, sizes[i]);
                    sizes[i] -= cut;
                    over -= cut;
                }
        }
        return sizes;
    }

    orientation dir_;
    int spacing_;
    int margin_;
};

// Labels right-aligned in the first column, fields in the second; an empty
// label lets its field span both. A widget for a label (a check) sits at the
// column's start.
class form_item : public container_item
{
public:
    form_item(int gap, int row_gap)
        : gap_(gap)
        , row_gap_(row_gap)
    {
    }
    struct row
    {
        item* label = nullptr;  // null: field spans both columns
        item* field = nullptr;
        bool label_at_start = false;
    };
    std::vector<row> rows;
    // The window's forms aligned with this one (itself among them), or null.
    const std::vector<form_item*>* group = nullptr;

    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }

    // Its own label column: the widest label of a visible row.
    long own_label_width() const
    {
        long w = 0;
        for (const row& r : rows)
            if (r.label && r.field->visible) w = std::max(w, r.label->natural().cx);
        return w;
    }
    // The column as laid out: the widest of its group's.
    long label_width() const
    {
        if (!group) return own_label_width();
        long w = 0;
        for (const form_item* f : *group) w = std::max(w, f->own_label_width());
        return w;
    }

    SIZE measure() override
    {
        long label_w = 0, field_w = 0, span_w = 0, h = 0;
        int n = 0;
        for (row& r : rows)
        {
            if (!r.field->visible) continue;
            const SIZE f = size_of(r.field);
            long row_h = f.cy;
            if (r.label)
            {
                const SIZE l = r.label->natural();
                label_w = std::max(label_w, l.cx);
                field_w = std::max(field_w, f.cx);
                row_h = std::max(row_h, l.cy);
            }
            else
                span_w = std::max(span_w, f.cx);
            h += row_h;
            ++n;
        }
        if (n > 1) h += row_gap_ * (n - 1);
        if (label_w) label_w = label_width();
        const long both = label_w ? label_w + gap_ + field_w : field_w;
        return {std::max(both, span_w), h};
    }

    void place(RECT r) override
    {
        placed(r);
        const long label_w = label_width();
        long y = r.top;
        for (row& row : rows)
        {
            if (!row.field->visible)
            {
                if (row.label) row.label->place({0, 0, 0, 0});
                continue;
            }
            const SIZE f = size_of(row.field);
            const long fx = row.label ? r.left + label_w + gap_ : r.left;
            long row_h = f.cy;
            if (row.label) row_h = std::max(row_h, row.label->natural().cy);
            long fw = row.field->expands_in_form() || row.field->stretch ? r.right - fx : std::min(f.cx, r.right - fx);
            row.field->place({fx, y + (row_h - f.cy) / 2, fx + fw, y + (row_h - f.cy) / 2 + f.cy});
            if (row.label)
            {
                const SIZE l = row.label->natural();
                const long lx = row.label_at_start ? r.left : r.left + label_w - l.cx;
                row.label->place(
                    {lx, y + (row_h - l.cy) / 2, lx + l.cx, y + (row_h - l.cy) / 2 + l.cy});
            }
            y += row_h + row_gap_;
        }
    }

    void show_all(bool shown) override
    {
        showing(shown);
        for (row& r : rows)
        {
            r.field->show_all(shown && visible);
            if (r.label) r.label->show_all(shown && visible && r.field->visible);
        }
    }

private:
    SIZE size_of(item* c)
    {
        SIZE s = c->natural();
        if (c->fixed_w > 0) s.cx = c->fixed_w;
        if (c->fixed_h > 0) s.cy = c->fixed_h;
        return s;
    }
    int gap_, row_gap_;
};

class spacer_item : public item
{
public:
    spacer_item() { stretch = 1; }
    SIZE measure() override { return {0, 0}; }
    void place(RECT) override {}
    void show_all(bool) override {}
};

// -- simple controls ----------------------------------------------------------

// A label's icon, when it's part of a link: the link's hand pointer.
LRESULT CALLBACK link_icon_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_SETCURSOR)
    {
        if (!busy_cursor_here(lp)) SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

class label_impl : public leaf<rap::label>
{
public:
    label_impl(win_context& win, HWND parent, const spec::label& s)
        : leaf(win)
        , ellipsize_(s.ellipsize)
        , wrap_(s.wrap && !s.link)
        , selectable_(s.selectable && !s.link)
        , link_(s.link)
        , on_click_(s.on_click)
        , icon_(s.icon)
    {
        // The icon before the text: a static of its own (SysLink can't hold
        // one), which on a link takes the link's click and pointer.
        if (icon_)
        {
            icon_h_ = create_control(win, parent, L"STATIC", L"", SS_ICON | SS_CENTERIMAGE | (link_ ? SS_NOTIFY : 0));
            handlers()[icon_h_] = this;
            if (link_) SetWindowSubclass(icon_h_, link_icon_proc, 3, 0);
        }
        if (link_)
        {
            // The system's link control: its colour, focus and Enter.
            adopt(create_control(win, parent, WC_LINK, link_markup(s.text), WS_TABSTOP));
            paint_buffered(h_, buffer_bg::parent);
        }
        else if (s.selectable)
        {
            // A read-only edit, without its frame or margins so its text
            // sits where a label's would; multi-line, so it shows every line
            // and wraps when asked to (a single-line one ran off its pane).
            adopt(create_control(win, parent, L"EDIT", to_crlf(wide(s.text)),
                                 ES_READONLY | ES_MULTILINE | (wrap_ ? 0 : ES_AUTOHSCROLL)));
            SendMessageW(h_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
            SetPropW(h_, L"rapport-label", h_);  // its colours: a label's (WM_CTLCOLORSTATIC)
            SetWindowSubclass(h_, selectable_proc, 4, 0);
        }
        else
            adopt(create_control(win, parent, L"STATIC", wide(s.text),
                                SS_LEFT | SS_NOPREFIX | (s.ellipsize ? SS_ENDELLIPSIS : 0)));
    }
    void set_text(const std::string& t) override
    {
        SetWindowTextW(h_, link_         ? link_markup(t).c_str()
                           : selectable_ ? to_crlf(wide(t)).c_str()
                                         : wide(t).c_str());
        // New markup, new link items: the colour they're to take (the
        // accent's) set on them again.
        if (link_) theme_control(h_);
        if (!ellipsize_) win_.relayout();
    }
    ~label_impl() override
    {
        if (icon_h_)
        {
            handlers().erase(icon_h_);
            DestroyWindow(icon_h_);
        }
        if (hicon_) DestroyIcon(hicon_);
    }
    LRESULT notify(NMHDR* n, bool& handled) override
    {
        handled = link_ && n->hwndFrom == h_ && (n->code == NM_CLICK || n->code == NM_RETURN);
        if (handled && on_click_) on_click_();
        return 0;
    }
    // The icon's click (only a link's icon sends one).
    void command(WORD code) override
    {
        if (code == STN_CLICKED && link_ && on_click_) on_click_();
    }
    void set_enabled(bool e) override
    {
        set_control_enabled(h_, e);
        if (icon_h_) set_control_enabled(icon_h_, e);
    }
    void show_all(bool shown) override
    {
        show_control(h_, shown && visible);
        if (icon_h_) show_control(icon_h_, shown && visible);
    }
    void place(RECT r) override
    {
        if (icon_h_)
        {
            const int size = px(16), h = r.bottom - r.top;
            move_control(icon_h_, r.left, r.top + (h - size) / 2, size, size);
            r.left += size + px(4);
        }
        move_control(h_, r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
    SIZE measure() override
    {
        SIZE s = measure_text();
        if (icon_h_)
        {
            // Drawn for the window's scale, again if that's changed.
            const int size = px(16);
            if (size != icon_px_)
            {
                if (hicon_) DestroyIcon(hicon_);
                hicon_ = render_icon_handle(icon_, size, win_.scale());
                SendMessageW(icon_h_, STM_SETICON, reinterpret_cast<WPARAM>(hicon_), 0);
                icon_px_ = size;
            }
            s.cx += size + px(4);
            s.cy = std::max<long>(s.cy, size);
        }
        return s;
    }
    SIZE measure_text()
    {
        if (link_)
        {
            SIZE ideal{};
            SendMessageW(h_, LM_GETIDEALSIZE, WPARAM(px(2000)), reinterpret_cast<LPARAM>(&ideal));
            return ideal;
        }
        // Line breaks in the text are lines (measured single-line, the
        // label asked for one line's height and showed only the first).
        const std::wstring text = window_text(h_);
        SIZE s = text_extent(h_, text, text.find(L'\n') != std::wstring::npos);
        if (wrap_)
        {
            // About 60 characters across, or the text's own width if less; its
            // height is the lines it takes there. Laid out at least that wide
            // (a box is as wide as its widest child), so it never needs more.
            const long across = std::min<long>(s.cx, font_metrics(win_.dpi()).cx * 60);
            s = text_extent(h_, text, true, int(across));
        }
        full_cx_ = s.cx;
        if (ellipsize_) s.cx = std::min<long>(s.cx, px(40));
        return s;
    }
    // Wrapped text takes more lines narrower than its natural width, fewer wider.
    long measure_height(long width) override
    {
        const SIZE n = natural();
        if (!wrap_ || width == n.cx) return n.cy;
        const int icon = icon_h_ ? px(16) + px(4) : 0;
        const long h =
            text_extent(h_, window_text(h_), true, int(std::max<long>(1, width - icon))).cy;
        return icon_h_ ? std::max<long>(h, px(16)) : h;
    }
    // Measured at a token width so it never widens a window, it still shows
    // its whole text in a row with room for it, as GTK and Qt do.
    long wants_wider() const override { return ellipsize_ ? std::max(0L, full_cx_ - px(40)) : 0; }
    // An ellipsized label's natural width is a token one, so it fills
    // across a column; text that wraps fills it too and wraps at its width.
    bool expands_in_form() const override { return ellipsize_ || wrap_; }

private:
    // SysLink takes its text as markup: <a> around it, & doubled (it marks
    // a mnemonic; SysLink knows no entities).
    static std::wstring link_markup(const std::string& text)
    {
        std::wstring out = L"<a>";
        for (wchar_t c : wide(text)) out += c == L'&' ? std::wstring(L"&&") : std::wstring(1, c);
        return out + L"</a>";
    }
    // A selectable label's edit: Tab and Enter go on to the dialog (a
    // multi-line edit asks for every key, which kept the focus in it), and
    // its margins stay none when its font changes; its label mark goes with it.
    static LRESULT CALLBACK selectable_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                                            DWORD_PTR)
    {
        if (msg == WM_GETDLGCODE)
            return DefSubclassProc(h, msg, wp, lp) & ~(DLGC_WANTALLKEYS | DLGC_WANTTAB);
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (msg == WM_SETFONT) SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        if (msg == WM_NCDESTROY) RemovePropW(h, L"rapport-label");
        return r;
    }
    bool ellipsize_;
    bool wrap_;
    bool selectable_;
    long full_cx_ = 0;
    bool link_;
    std::function<void()> on_click_;
    icon icon_;
    HWND icon_h_ = nullptr;
    HICON hicon_ = nullptr;
    int icon_px_ = 0;
};

class button_impl : public leaf<rap::button>
{
public:
    button_impl(win_context& win, HWND parent, const std::string& text, DWORD extra = 0)
        : leaf(win)
    {
        adopt(create_control(win, parent, L"BUTTON", mnemonic(text), BS_PUSHBUTTON | WS_TABSTOP | extra));
    }
    ~button_impl() override
    {
        if (images_) g_comctl.destroy(images_);
    }
    void set_text(const std::string& t) override
    {
        SetWindowTextW(h_, mnemonic(t).c_str());
        win_.relayout();
    }
    void set_icon(const icon& icon) override
    {
        icon_ = icon;
        if (images_) g_comctl.destroy(images_);
        images_ = nullptr;
        if (!icon) return;
        const int size = px(16);
        images_ = g_comctl.create(size, size, ILC_COLOR32, 1, 0);
        if (HICON hi = render_icon_handle(icon, size, win_.scale()))
        {
            g_comctl.replace_icon(images_, -1, hi);
            DestroyIcon(hi);
        }
        BUTTON_IMAGELIST bil{images_, {px(2), 0, px(2), 0}, BUTTON_IMAGELIST_ALIGN_LEFT};
        SendMessageW(h_, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&bil));
        win_.relayout();
    }
    void on_click(std::function<void()> f) override { click_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == BN_CLICKED && click_) click_();
    }
    SIZE measure() override
    {
        const SIZE t = text_extent(h_, window_text(h_));
        const bool has_text = GetWindowTextLengthW(h_) > 0;
        long w = (has_text ? t.cx : 0) + px(has_text ? 24 : 10) + (images_ ? px(has_text ? 20 : 16) : 0);
        if (has_text) w = std::max<long>(w, px(75));
        return {w, std::max<long>(px(23), t.cy + px(8))};
    }

    bool is_default = false;
    dialog_role role = dialog_role::none;

private:
    std::function<void()> click_;
    icon icon_;
    HIMAGELIST images_ = nullptr;
};

class check_impl : public leaf<rap::check>
{
public:
    check_impl(win_context& win, HWND parent, const spec::check& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, L"BUTTON", mnemonic(s.text), BS_AUTO3STATE | WS_TABSTOP));
        set_state(s.state);
    }
    bool checked() const override { return SendMessageW(h_, BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void set_checked(bool c) override { set_state(c ? check_state::checked : check_state::unchecked); }
    check_state state() const override
    {
        switch (SendMessageW(h_, BM_GETCHECK, 0, 0))
        {
        case BST_CHECKED: return check_state::checked;
        case BST_INDETERMINATE: return check_state::indeterminate;
        default: return check_state::unchecked;
        }
    }
    void set_state(check_state s) override
    {
        SendMessageW(h_, BM_SETCHECK,
                     s == check_state::checked         ? BST_CHECKED
                     : s == check_state::indeterminate ? BST_INDETERMINATE
                                                      : BST_UNCHECKED,
                     0);
    }
    void on_toggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }
    void command(WORD code) override
    {
        if (code != BN_CLICKED) return;
        // Clicks only toggle on/off: a mixed box goes to checked, never back to mixed.
        if (SendMessageW(h_, BM_GETCHECK, 0, 0) == BST_INDETERMINATE) SendMessageW(h_, BM_SETCHECK, BST_UNCHECKED, 0);
        if (toggled_) toggled_(checked());
    }
    SIZE measure() override
    {
        const SIZE t = text_extent(h_, window_text(h_));
        return {t.cx + px(GetWindowTextLengthW(h_) ? 22 : 14), std::max<long>(t.cy, px(17))};
    }
    // Dark mode's check box style draws its label black whatever the text
    // colour: the box from the style and the label in the theme's colour.
    LRESULT notify(NMHDR* hdr, bool& handled) override
    {
        handled = false;
        if (hdr->code != NM_CUSTOMDRAW || !dark_mode()) return 0;
        auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(hdr);
        handled = true;
        if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
        HTHEME theme = OpenThemeData(h_, L"BUTTON");
        if (!theme) return CDRF_DODEFAULT;
        const LRESULT check = SendMessageW(h_, BM_GETCHECK, 0, 0);
        const UINT item = cd->uItemState;
        int state = check == BST_CHECKED         ? CBS_CHECKEDNORMAL
                    : check == BST_INDETERMINATE ? CBS_MIXEDNORMAL
                                                 : CBS_UNCHECKEDNORMAL;
        if (item & CDIS_DISABLED)
            state += 3;
        else if (item & CDIS_SELECTED)
            state += 2;
        else if (item & CDIS_HOT)
            state += 1;
        HDC dc = cd->hdc;
        const RECT r = cd->rc;
        FillRect(dc, &r, theme_brush(surface_of(h_)));
        SIZE box{px(13), px(13)};
        GetThemePartSize(theme, dc, BP_CHECKBOX, state, nullptr, TS_DRAW, &box);
        RECT b{r.left, r.top + (r.bottom - r.top - box.cy) / 2, r.left + box.cx, 0};
        b.bottom = b.top + box.cy;
        DrawThemeBackground(theme, dc, BP_CHECKBOX, state, &b, nullptr);
        const std::wstring label = window_text(h_);
        if (!label.empty())
        {
            HGDIOBJ old =
                SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h_, WM_GETFONT, 0, 0)));
            const LRESULT ui = SendMessageW(h_, WM_QUERYUISTATE, 0, 0);
            DWORD flags = DT_SINGLELINE | DT_VCENTER | DT_LEFT;
            if (ui & UISF_HIDEACCEL) flags |= DT_HIDEPREFIX;
            RECT text{b.right + px(4), r.top, r.right, r.bottom};
            DTTOPTS opts{};
            opts.dwSize = sizeof opts;
            opts.dwFlags = DTT_TEXTCOLOR;
            opts.crText = color_ref(theme_color_of(
                (item & CDIS_DISABLED) ? theme_color::disabled_text : surface_text_of(h_)));
            DrawThemeTextEx(theme, dc, BP_CHECKBOX, state, label.c_str(), -1, flags, &text, &opts);
            if ((item & CDIS_FOCUS) && !(ui & UISF_HIDEFOCUS))
            {
                RECT focus = text;
                DrawThemeTextEx(theme, dc, BP_CHECKBOX, state, label.c_str(), -1,
                                flags | DT_CALCRECT, &focus, &opts);
                focus.top = text.top + (text.bottom - text.top - (focus.bottom - focus.top)) / 2;
                focus.bottom = focus.top + (focus.bottom - focus.top);
                InflateRect(&focus, px(1), 0);
                DrawFocusRect(dc, &focus);
            }
            SelectObject(dc, old);
        }
        CloseThemeData(theme);
        return CDRF_SKIPDEFAULT;
    }

private:
    std::function<void(bool)> toggled_;
};

// Edit controls report Enter only to a subclass.
LRESULT CALLBACK enter_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN)
    {
        auto* fn = reinterpret_cast<std::function<void()>*>(data);
        if (fn && *fn)
        {
            (*fn)();
            return 0;
        }
    }
    if (msg == WM_CHAR && wp == VK_RETURN)
    {
        auto* fn = reinterpret_cast<std::function<void()>*>(data);
        if (fn && *fn) return 0;  // no beep
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// A frameless entry keeps its WS_EX_CLIENTEDGE, so its size and where its
// text sits match a framed one (as GTK's and Qt's frameless entries keep
// their padding); the edge is painted with the background its text has
// instead of the themed border. Themed EDITs repaint that border on focus
// and hover (WM_NCPAINT), so all of its painting comes here.
// Its edge painted on `dc`, the window's own (0, 0 its top left): on screen
// (WM_NCPAINT) or printed (WM_PRINT with PRF_NONCLIENT: a scroll pane drawn
// off screen, which validates the frame's paint as well).
void paint_frameless_edge(HWND h, HDC dc)
{
    RECT window, client;
    GetWindowRect(h, &window);
    GetClientRect(h, &client);
    MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&client), 2);
    OffsetRect(&client, -window.left, -window.top);
    OffsetRect(&window, -window.left, -window.top);
    // What the text's background is: the parent's WM_CTLCOLORSTATIC brush
    // for a read-only EDIT, else the edit colour.
    HBRUSH brush = nullptr;
    if (GetWindowLongPtrW(h, GWL_STYLE) & ES_READONLY || !IsWindowEnabled(h))
        brush = reinterpret_cast<HBRUSH>(
            SendMessageW(GetParent(h), WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc),
                         reinterpret_cast<LPARAM>(h)));
    if (!brush) brush = GetSysColorBrush(COLOR_WINDOW);
    const int saved = SaveDC(dc);
    ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
    FillRect(dc, &window, brush);
    RestoreDC(dc, saved);
}

LRESULT CALLBACK frameless_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_PRINT && (lp & PRF_NONCLIENT))
    {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        paint_frameless_edge(h, reinterpret_cast<HDC>(wp));
        return r;
    }
    if (msg != WM_NCPAINT) return DefSubclassProc(h, msg, wp, lp);
    if (HDC dc = GetWindowDC(h))
    {
        paint_frameless_edge(h, dc);
        ReleaseDC(h, dc);
    }
    return 0;
}

// A button inside an edit control at its start or end (spec::entry_action,
// and the clear button): a child of the edit, owner drawn as the flat tool
// buttons are, with the edit's margin on that side kept clear of text for
// it. The edit passes its WM_COMMAND and WM_DRAWITEM on
// (edit_buttons_subclass). With a menu it's wider, a drop arrow after the
// icon, and the menu drops down under it.
class edit_button : public rap::entry_action, public handler
{
public:
    edit_button(win_context& win, HWND edit, icon i, const std::string& tip,
                std::function<void()> click)
        : win_(win)
        , icon_(std::move(i))
        , click_(std::move(click))
    {
        want_icon(icon_, 16, win_.dpi());  // as draw_item paints it
        h_ = CreateWindowExW(0, L"BUTTON", wide(tip).c_str(), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                             0, 0, 0, 0, edit, nullptr, instance(), nullptr);
        handlers()[h_] = this;
        track_container_control(h_);
        set_tooltip_text(h_, tip);
        SetWindowSubclass(h_, hover_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    ~edit_button() override
    {
        handlers().erase(h_);
        if (IsWindow(h_)) DestroyWindow(h_);
    }
    void set_icon(const icon& i) override
    {
        icon_ = i;
        want_icon(icon_, 16, win_.dpi());
        InvalidateRect(h_, nullptr, TRUE);
    }
    void set_tooltip(const std::string& t) override { set_tooltip_text(h_, t); }
    void set_checked(bool c) override
    {
        checked_ = c;
        InvalidateRect(h_, nullptr, TRUE);
    }
    bool checked() const override { return checked_; }
    void set_enabled(bool e) override { set_control_enabled(h_, e); }
    void on_menu(std::function<menu()> f) override
    {
        menu_ = std::move(f);
        InvalidateRect(h_, nullptr, TRUE);
    }
    bool has_menu() const { return bool(menu_); }
    void command(WORD code) override
    {
        if (code != BN_CLICKED) return;
        if (!menu_)
        {
            if (click_) click_();
            return;
        }
        RECT r{};
        GetWindowRect(h_, &r);
        show_popup_menu(h_, menu_(), POINT{r.left, r.bottom}, h_);
    }
    bool draw_item(DRAWITEMSTRUCT* d) override
    {
        RECT r = d->rcItem;
        FillRect(d->hDC, &r, theme_brush(theme_color::base));
        const bool pressed = d->itemState & ODS_SELECTED;
        const bool disabled = d->itemState & ODS_DISABLED;
        if (pressed || checked_ || hot_)
        {
            const color tint = theme_color_of(theme_color::highlight);
            const float a = pressed ? 0.35f : checked_ ? 0.25f : 0.12f;
            const color face = theme_color_of(theme_color::base);
            auto mix = [a](uint8_t x, uint8_t y) { return uint8_t(x * (1 - a) + y * a); };
            HBRUSH b = CreateSolidBrush(
                RGB(mix(face.r, tint.r), mix(face.g, tint.g), mix(face.b, tint.b)));
            RECT in = r;
            InflateRect(&in, -win_.px(1), -win_.px(1));
            FillRect(d->hDC, &in, b);
            DeleteObject(b);
        }
        const int size = win_.px(16);
        const int arrow = menu_ ? win_.px(8) : 0;
        RECT ir{r.left + (r.right - r.left - size - arrow) / 2,
                r.top + (r.bottom - r.top - size) / 2, 0, 0};
        ir.right = ir.left + size;
        ir.bottom = ir.top + size;
        const color ink = disabled ? theme_color_of(theme_color::placeholder_text)
                                   : theme_color_of(theme_color::text);
        draw_icon_on_dc(d->hDC, ir, icon_, win_.scale(), ink);
        if (menu_)
        {
            // A small down-pointing triangle after the icon.
            const int cx = ir.right + arrow / 2, cy = (r.top + r.bottom) / 2;
            const int half = win_.px(3);
            const POINT tri[3] = {{cx - half, cy - half / 2},
                                  {cx + half + 1, cy - half / 2},
                                  {cx, cy + half - half / 2}};
            HBRUSH b = CreateSolidBrush(RGB(ink.r, ink.g, ink.b));
            HGDIOBJ old_brush = SelectObject(d->hDC, b);
            HGDIOBJ old_pen = SelectObject(d->hDC, GetStockObject(NULL_PEN));
            Polygon(d->hDC, tri, 3);
            SelectObject(d->hDC, old_pen);
            SelectObject(d->hDC, old_brush);
            DeleteObject(b);
        }
        return true;
    }
    HWND hwnd() const { return h_; }

private:
    static LRESULT CALLBACK hover_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                                       DWORD_PTR data)
    {
        auto* self = reinterpret_cast<edit_button*>(data);
        if (msg == WM_MOUSEMOVE && !self->hot_)
        {
            self->hot_ = true;
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        else if (msg == WM_MOUSELEAVE)
        {
            self->hot_ = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    win_context& win_;
    HWND h_ = nullptr;
    icon icon_;
    std::function<void()> click_;
    std::function<menu()> menu_;
    bool checked_ = false, hot_ = false;
};

class entry_impl : public leaf<rap::entry>
{
public:
    entry_impl(win_context& win, HWND parent, const spec::entry& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, L"EDIT", wide(s.text),
                             ES_AUTOHSCROLL | WS_TABSTOP | WS_CLIPCHILDREN |
                                 (s.password ? ES_PASSWORD : 0),
                             WS_EX_CLIENTEDGE));
        set_placeholder(s.placeholder);
        SetWindowSubclass(h_, enter_subclass, 1, reinterpret_cast<DWORD_PTR>(&activate_));
        // The clear button nearest the text, the actions after it.
        if (s.clearable)
            clear_ = std::make_unique<edit_button>(win, h_, icon::named("window-close"),
                                                   rap::i18n::tr("rapport", "Clear"),
                                                   [this]
                                                   {
                                                       SetWindowTextW(h_, L"");
                                                       SetFocus(h_);
                                                   });
        for (const spec::entry_action& a : s.actions)
        {
            auto& side = a.leading ? leading_ : actions_;
            side.push_back(std::make_unique<edit_button>(win, h_, a.icon, a.tooltip, a.on_click));
            if (a.on_menu) side.back()->on_menu(a.on_menu);
            if (a.bind) *a.bind = side.back().get();
        }
        if (clear_ || !actions_.empty() || !leading_.empty())
        {
            SetWindowSubclass(h_, edit_buttons_subclass, 3, reinterpret_cast<DWORD_PTR>(this));
            place_buttons();
        }
    }
    std::string text() const override { return narrow(window_text(h_)); }
    void set_text(const std::string& t) override
    {
        quiet_ = true;
        SetWindowTextW(h_, wide(t).c_str());
        quiet_ = false;
        place_buttons();
    }
    void set_read_only(bool v) override { SendMessageW(h_, EM_SETREADONLY, v, 0); }
    // Frameless: the edge stays, painted as background (frameless_subclass).
    // Read-only, the text then sits on the window's background
    // (WM_CTLCOLORSTATIC).
    void set_frame(bool v) override
    {
        if (v)
            RemoveWindowSubclass(h_, frameless_subclass, 2);
        else
            SetWindowSubclass(h_, frameless_subclass, 2, 0);
        RedrawWindow(h_, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_ERASE);
    }
    void set_placeholder(const std::string& p) override
    {
        SendMessageW(h_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(wide(p).c_str()));
    }
    void on_changed(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void()> f) override { activate_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == EN_CHANGE) place_buttons();  // the clear button comes and goes
        if (code == EN_CHANGE && !quiet_ && changed_) changed_(text());
    }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        const SIZE m = font_metrics(win_.dpi());
        return {m.cx * 20 + px(8), m.cy + px(9)};
    }

private:
    // Square buttons the client area's height (a menu's wider, for its
    // arrow): the leading ones at its left end, the trailing ones at its
    // right end outermost, the clear button (while there's text) inside
    // them; the text kept clear of them.
    void place_buttons()
    {
        if (!clear_ && actions_.empty() && leading_.empty()) return;
        RECT c{};
        GetClientRect(h_, &c);
        const int size = c.bottom - c.top;
        auto width = [&](const edit_button& b) { return size + (b.has_menu() ? px(8) : 0); };
        int left = 0;
        for (const auto& b : leading_)
        {
            MoveWindow(b->hwnd(), left, 0, width(*b), size, TRUE);
            left += width(*b);
        }
        int x = c.right;
        for (auto it = actions_.rbegin(); it != actions_.rend(); ++it)
        {
            x -= width(**it);
            MoveWindow((*it)->hwnd(), x, 0, width(**it), size, TRUE);
        }
        if (clear_)
        {
            const bool any = GetWindowTextLengthW(h_) > 0;
            if (any)
            {
                x -= size;
                MoveWindow(clear_->hwnd(), x, 0, size, size, TRUE);
            }
            ShowWindow(clear_->hwnd(), any ? SW_SHOWNA : SW_HIDE);
        }
        SendMessageW(h_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(left, c.right - x));
    }
    static LRESULT CALLBACK edit_buttons_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                                                  DWORD_PTR data)
    {
        auto* self = reinterpret_cast<entry_impl*>(data);
        switch (msg)
        {
        case WM_COMMAND:
        case WM_DRAWITEM:
        {
            LRESULT result = 0;
            if (forward_child_message(h, msg, wp, lp, result)) return result;
            break;
        }
        case WM_SETCURSOR:
            // Over a button the arrow, not the edit's I-beam.
            if (reinterpret_cast<HWND>(wp) != h)
            {
                if (!busy_cursor_here(lp)) SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_SIZE:
        case WM_SETFONT:
        {
            const LRESULT r = DefSubclassProc(h, msg, wp, lp);
            self->place_buttons();
            return r;
        }
        default: break;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    std::unique_ptr<edit_button> clear_;
    std::vector<std::unique_ptr<edit_button>> leading_, actions_;
    bool quiet_ = false;
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

class drop_down_impl : public leaf<rap::drop_down>
{
public:
    drop_down_impl(win_context& win, HWND parent, const spec::drop_down& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP));
        current_ = s.current;
        set_items(s.items);
    }
    // Until it's opened or focused the box holds just the shown item: each
    // item added costs an accessibility event, milliseconds apiece whenever
    // an automation client listens, and a dialog has many of these.
    void set_items(std::vector<std::string> items) override
    {
        items_ = std::move(items);
        filled_ = false;
        show_current_only();
        win_.relayout();
    }
    int current() const override { return filled_ ? int(SendMessageW(h_, CB_GETCURSEL, 0, 0)) : current_; }
    void set_current(int i) override
    {
        current_ = i;
        if (filled_)
            SendMessageW(h_, CB_SETCURSEL, WPARAM(i), 0);
        else
            show_current_only();
    }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == CBN_DROPDOWN || code == CBN_SETFOCUS) fill();  // the keys choose from all of them too
        if (code == CBN_SELCHANGE)
        {
            current_ = current();
            if (changed_) changed_(current_);
        }
    }
    SIZE measure() override
    {
        long w = px(60);
        for (const std::string& i : items_) w = std::max(w, text_extent(h_, wide(i)).cx);
        return {w + px(32), font_metrics(win_.dpi()).cy + px(10)};
    }

private:
    void show_current_only()
    {
        SendMessageW(h_, CB_RESETCONTENT, 0, 0);
        if (current_ >= 0 && size_t(current_) < items_.size())
        {
            SendMessageW(h_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(items_[size_t(current_)]).c_str()));
            SendMessageW(h_, CB_SETCURSEL, 0, 0);
        }
    }
    void fill()
    {
        if (filled_) return;
        filled_ = true;
        SendMessageW(h_, CB_RESETCONTENT, 0, 0);
        for (const std::string& i : items_) SendMessageW(h_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(i).c_str()));
        SendMessageW(h_, CB_SETCURSEL, WPARAM(current_), 0);
    }

    std::vector<std::string> items_;
    int current_ = -1;
    bool filled_ = false;
    std::function<void(int)> changed_;
};

class combo_impl : public leaf<rap::combo>
{
public:
    combo_impl(win_context& win, HWND parent, const spec::combo& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP));
        set_items(s.items);
        set_text(s.text);
        COMBOBOXINFO info{};
        info.cbSize = sizeof info;
        if (GetComboBoxInfo(h_, &info) && info.hwndItem)
            SetWindowSubclass(info.hwndItem, enter_subclass, 1, reinterpret_cast<DWORD_PTR>(&activate_));
    }
    void set_items(std::vector<std::string> items) override
    {
        const std::wstring keep = window_text(h_);
        SendMessageW(h_, CB_RESETCONTENT, 0, 0);
        for (const std::string& i : items) SendMessageW(h_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(i).c_str()));
        SetWindowTextW(h_, keep.c_str());
    }
    std::string text() const override { return narrow(window_text(h_)); }
    void set_text(const std::string& t) override
    {
        quiet_ = true;
        SetWindowTextW(h_, wide(t).c_str());
        quiet_ = false;
    }
    void on_changed(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void()> f) override { activate_ = std::move(f); }
    void command(WORD code) override
    {
        if (quiet_ || !changed_) return;
        if (code == CBN_EDITCHANGE)
            changed_(text());
        else if (code == CBN_SELCHANGE)
        {
            const int i = int(SendMessageW(h_, CB_GETCURSEL, 0, 0));
            const int n = int(SendMessageW(h_, CB_GETLBTEXTLEN, WPARAM(i), 0));
            std::wstring w(size_t(std::max(n, 0)) + 1, L'\0');
            SendMessageW(h_, CB_GETLBTEXT, WPARAM(i), reinterpret_cast<LPARAM>(w.data()));
            w.resize(size_t(std::max(n, 0)));
            changed_(narrow(w));
        }
    }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        const SIZE m = font_metrics(win_.dpi());
        return {m.cx * 20 + px(32), m.cy + px(10)};
    }

private:
    bool quiet_ = false;
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

// Multi-line text in a RichEdit control, so styled spans (diff colours)
// show. Offsets in the API are UTF-8 bytes; RichEdit counts UTF-16 units
// with each line break as one character.
class text_view_impl : public leaf<rap::text_view>
{
public:
    text_view_impl(win_context& win, HWND parent, const spec::text_view& s)
        : leaf(win)
        , mono_(s.monospace)
    {
        static HMODULE lib = LoadLibraryW(L"Msftedit.dll");
        (void)lib;
        adopt(create_control(win, parent, L"RICHEDIT50W", L"",
                            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP | ES_NOHIDESEL |
                                (s.wrap ? 0 : ES_AUTOHSCROLL | WS_HSCROLL) | (s.read_only ? ES_READONLY : 0),
                            s.frame ? WS_EX_CLIENTEDGE : 0));
        SendMessageW(h_, EM_SETEVENTMASK, 0, ENM_CHANGE);
        // RichEdit holds 32,767 characters unless told otherwise, and cuts
        // longer text off silently (a big diff showed only its start).
        SendMessageW(h_, EM_EXLIMITTEXT, 0, 0x7ffffffe);
        SendMessageW(h_, EM_SETBKGNDCOLOR, 0, LPARAM(color_ref(theme_color_of(theme_color::base))));
        set_text_colour(SCF_DEFAULT);
        if (mono_) SendMessageW(h_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font(win.dpi(), false, true)), FALSE);
        SetWindowSubclass(h_, wheel_proc, 1, reinterpret_cast<DWORD_PTR>(this));
        // No error beep for typing into a read-only view (RichEdit allows
        // beeps by default); that it can't be edited is plain to see.
        SendMessageW(h_, EM_SETEDITSTYLE, 0, SES_ALLOWBEEPS);
        set_text(s.text, {});
    }

    // RichEdit animates wheel scrolling: each notch glides, and quick
    // wheeling queues glides, so the view lags behind the wheel. Scroll by
    // whole lines at once instead, as many per notch as the system setting
    // says (a page when it says so); high-resolution wheels send fractions
    // of a notch, which add up.
    static LRESULT CALLBACK wheel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        if (msg != WM_MOUSEWHEEL || (GET_KEYSTATE_WPARAM(wp) & (MK_CONTROL | MK_SHIFT)))
            return DefSubclassProc(h, msg, wp, lp);  // Ctrl zooms, Shift scrolls sideways
        auto* self = reinterpret_cast<text_view_impl*>(data);
        UINT per_notch = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &per_notch, 0);
        if (per_notch == WHEEL_PAGESCROLL)
        {
            SendMessageW(h, WM_VSCROLL, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
            return 0;
        }
        self->wheel_ += GET_WHEEL_DELTA_WPARAM(wp) * int(per_notch);
        const int lines = self->wheel_ / WHEEL_DELTA;
        self->wheel_ -= lines * WHEEL_DELTA;
        if (lines) SendMessageW(h, EM_LINESCROLL, 0, -lines);
        return 0;
    }
    std::string text() const override
    {
        std::wstring w = window_text(h_);
        for (wchar_t& c : w)
            if (c == L'\r') c = L'\n';
        return narrow(w);
    }
    void set_text(const std::string& t, std::vector<span> spans) override
    {
        text_ = t;
        spans_ = spans;
        appended_.clear();
        quiet_ = true;
        if (spans.empty())
            SetWindowTextW(h_, wide(t).c_str());
        else
            stream_rtf(rtf(t, std::move(spans)));
        SendMessageW(h_, EM_SETSEL, 0, 0);
        quiet_ = false;
    }
    void append(const std::string& t, const text_style& style) override
    {
        appended_.emplace_back(t, style);
        quiet_ = true;
        // Positions from RichEdit itself: GetWindowTextLength counts each
        // line break as CR LF, two to RichEdit's one, which put the style a
        // character late per line ("Success" with a black S).
        CHARRANGE end{-1, -1};
        SendMessageW(h_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
        CHARRANGE at{};
        SendMessageW(h_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&at));
        SendMessageW(h_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(wide(t).c_str()));
        CHARRANGE after{};
        SendMessageW(h_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&after));
        format(at.cpMin, after.cpMax, style);
        quiet_ = false;
    }
    void set_read_only(bool r) override { SendMessageW(h_, EM_SETREADONLY, r, 0); }
    std::pair<size_t, size_t> selection() const override
    {
        CHARRANGE cr{};
        SendMessageW(h_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        const std::string t = text();
        return {bytes_at(t, cr.cpMin), bytes_at(t, cr.cpMax)};
    }
    void select(size_t begin, size_t end) override
    {
        const std::vector<long> pos = units(text(), {begin, end});
        CHARRANGE cr{pos[0], pos[1]};
        SendMessageW(h_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        SendMessageW(h_, EM_SCROLLCARET, 0, 0);
    }
    void on_changed(std::function<void()> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == EN_CHANGE && !quiet_ && changed_) changed_();
    }
    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        const SIZE m = font_metrics(win_.dpi());
        return {m.cx * 40, m.cy * 6 + px(8)};
    }

private:
    // RichEdit positions count UTF-16 units, with CR LF as one.
    static int units_of(const std::string& t, size_t i)
    {
        const auto c = uint8_t(t[i]);
        if ((c & 0xc0) == 0x80) return 0;  // continuation byte
        if (c == '\n' && i > 0 && t[i - 1] == '\r') return 0;
        return c >= 0xf0 ? 2 : 1;  // four-byte sequences are surrogate pairs
    }
    // UTF-8 byte offsets -> RichEdit positions, in one pass.
    static std::vector<long> units(const std::string& t, const std::vector<size_t>& bytes)
    {
        std::vector<size_t> order(bytes.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return bytes[a] < bytes[b]; });
        std::vector<long> out(bytes.size());
        size_t at = 0;
        long n = 0;
        for (size_t i : order)
        {
            const size_t target = std::min(bytes[i], t.size());
            for (; at < target; ++at) n += units_of(t, at);
            out[i] = n;
        }
        return out;
    }
    // A RichEdit position -> the UTF-8 byte offset where it falls.
    static size_t bytes_at(const std::string& t, long pos)
    {
        long n = 0;
        size_t at = 0;
        for (; at < t.size() && n < pos; ++at) n += units_of(t, at);
        while (at < t.size() && (uint8_t(t[at]) & 0xc0) == 0x80) ++at;  // finish the character
        return at;
    }
    // Styled text goes in as one RTF stream. Formatting each span with
    // EM_SETCHARFORMAT made RichEdit lay out again per span: seconds for a
    // diff of a few thousand coloured lines.
    std::string rtf(const std::string& t, std::vector<span> spans) const
    {
        std::sort(spans.begin(), spans.end(), [](const span& a, const span& b) { return a.begin < b.begin; });
        // The control's own font, so styled text looks like plain text.
        LOGFONTW lf{};
        GetObjectW(ui_font(win_.dpi(), false, mono_), sizeof lf, &lf);
        const int half_points = lf.lfHeight ? int(std::lround(std::abs(lf.lfHeight) * 144.0 / win_.dpi())) : 18;
        std::vector<COLORREF> colours;
        auto colour_index = [&](const paint& p)
        {
            const COLORREF c = color_ref(win32::resolve(p));
            auto it = std::find(colours.begin(), colours.end(), c);
            if (it == colours.end()) it = colours.insert(colours.end(), c);
            return int(it - colours.begin()) + 1;  // 0 is the default colour
        };
        std::string body;
        body.reserve(t.size() + t.size() / 4);
        auto text = [&](size_t from, size_t to)
        {
            for (size_t i = from; i < to && i < t.size();)
            {
                const auto c = uint8_t(t[i]);
                if (c < 0x80)
                {
                    ++i;
                    if (c == '\\' || c == '{' || c == '}') body += '\\', body += char(c);
                    else if (c == '\n') body += "\\par\n";
                    else if (c == '\t') body += "\\tab ";
                    else if (c != '\r') body += char(c);
                    continue;
                }
                // UTF-8 -> code point -> \uN with UTF-16 units (signed, as RTF has them).
                const int len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
                char32_t cp = len == 1 ? 0xfffd : char32_t(c & (0x7f >> len));
                for (int k = 1; k < len && i + k < t.size(); ++k) cp = (cp << 6) | (uint8_t(t[i + k]) & 0x3f);
                i += len;
                auto unit = [&](int u) { body += "\\u" + std::to_string(int16_t(u)) + ' '; };
                if (cp >= 0x10000)
                {
                    cp -= 0x10000;
                    unit(0xd800 + int(cp >> 10));
                    unit(0xdc00 + int(cp & 0x3ff));
                }
                else
                    unit(int(cp));
            }
        };
        size_t at = 0;
        for (const span& sp : spans)
        {
            const size_t begin = std::max(sp.begin, at), end = std::min(sp.end, t.size());
            if (end <= begin) continue;
            text(at, begin);
            body += '{';
            if (sp.style.fg) body += "\\cf" + std::to_string(colour_index(*sp.style.fg));
            if (sp.style.bg) body += "\\highlight" + std::to_string(colour_index(*sp.style.bg));
            if (sp.style.bold) body += "\\b";
            if (sp.style.italic) body += "\\i";
            if (sp.style.monospace && !mono_) body += "\\f1";
            body += ' ';
            text(begin, end);
            body += '}';
            at = end;
        }
        text(at, t.size());

        // Unstyled text in the theme's text colour: RTF's automatic colour
        // is the system's, light in dark mode.
        const int plain = colour_index(theme_color::text);
        std::string out = "{\\rtf1\\ansi\\uc0{\\fonttbl{\\f0 " + narrow(lf.lfFaceName) + ";}{\\f1 Consolas;}}{\\colortbl;";
        for (COLORREF c : colours)
            out += "\\red" + std::to_string(GetRValue(c)) + "\\green" + std::to_string(GetGValue(c)) + "\\blue" +
                   std::to_string(GetBValue(c)) + ';';
        out += "}\\f0\\cf" + std::to_string(plain) + "\\fs" + std::to_string(half_points) + ' ' +
               body + '}';
        return out;
    }
    void stream_rtf(const std::string& rtf)
    {
        struct rtf_reader
        {
            const std::string* data;
            size_t at = 0;
        } reader{&rtf};
        EDITSTREAM es{};
        es.dwCookie = reinterpret_cast<DWORD_PTR>(&reader);
        es.pfnCallback = [](DWORD_PTR cookie, LPBYTE buf, LONG size, LONG* got) -> DWORD
        {
            auto* r = reinterpret_cast<rtf_reader*>(cookie);
            const size_t n = std::min<size_t>(size_t(size), r->data->size() - r->at);
            std::memcpy(buf, r->data->data() + r->at, n);
            r->at += n;
            *got = LONG(n);
            return 0;
        };
        SendMessageW(h_, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&es));
    }

    void format(long a, long b, const text_style& st)
    {
        if (b <= a) return;
        CHARRANGE cr{a, b};
        SendMessageW(h_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof cf;
        cf.dwMask = CFM_BOLD | CFM_ITALIC;
        cf.dwEffects = (st.bold ? CFE_BOLD : 0) | (st.italic ? CFE_ITALIC : 0);
        auto colour = [](const paint& p) { return win32::resolve(p); };
        if (st.fg)
        {
            cf.dwMask |= CFM_COLOR;
            cf.crTextColor = color_ref(colour(*st.fg));
        }
        if (st.bg)
        {
            cf.dwMask |= CFM_BACKCOLOR;
            cf.crBackColor = color_ref(colour(*st.bg));
        }
        if (st.monospace && !mono_)
        {
            cf.dwMask |= CFM_FACE;
            wcscpy(cf.szFaceName, L"Consolas");
        }
        SendMessageW(h_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    }

    // Text without a colour of its own in the theme's: RichEdit's
    // automatic colour is the system's, light in dark mode.
    void set_text_colour(WPARAM which)
    {
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof cf;
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = color_ref(theme_color_of(theme_color::text));
        SendMessageW(h_, EM_SETCHARFORMAT, which, reinterpret_cast<LPARAM>(&cf));
    }
    // The colours changed: styled text, whose colours were resolved when
    // it went in, goes in again, where the scroll and selection were. Text
    // the user may have edited keeps its own and takes the text colour.
    void theme_changed() override
    {
        set_text_colour(SCF_DEFAULT);
        const bool styled = !spans_.empty() || !appended_.empty();
        if (!styled || !(GetWindowLongPtrW(h_, GWL_STYLE) & ES_READONLY))
        {
            if (!styled) set_text_colour(SCF_ALL);
            return;
        }
        POINT scroll{};
        SendMessageW(h_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
        CHARRANGE sel{};
        SendMessageW(h_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
        SendMessageW(h_, WM_SETREDRAW, FALSE, 0);
        const std::string t = text_;
        std::vector<span> spans = spans_;
        auto appended = std::move(appended_);
        set_text(t, std::move(spans));
        for (const auto& [text, style] : appended) append(text, style);
        SendMessageW(h_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&sel));
        SendMessageW(h_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
        SendMessageW(h_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(h_, nullptr, TRUE);
    }

    int wheel_ = 0;  // wheel delta × lines per notch not yet scrolled
    bool mono_;
    bool quiet_ = false;
    std::function<void()> changed_;
    // What went in, to put in again in new colours.
    std::string text_;
    std::vector<span> spans_;
    std::vector<std::pair<std::string, text_style>> appended_;
};

class separator_item : public item
{
public:
    separator_item(win_context& win, HWND parent, orientation line_dir)
        : win_(win)
        , dir_(line_dir)
    {
        h_ = create_control(win, parent, L"STATIC", L"",
                           line_dir == orientation::horizontal ? SS_ETCHEDHORZ : SS_ETCHEDVERT);
    }
    ~separator_item() override { DestroyWindow(h_); }
    SIZE measure() override
    {
        const int t = std::max(2, win_.px(2)), pad = win_.px(4);
        return dir_ == orientation::horizontal ? SIZE{pad, t + 2 * pad} : SIZE{t + 2 * pad, pad};
    }
    bool fills() const override { return true; }
    void place(RECT r) override
    {
        const int pad = win_.px(4);
        if (dir_ == orientation::horizontal)
            move_control(h_, r.left, (r.top + r.bottom) / 2, r.right - r.left, 2);
        else
            move_control(h_, (r.left + r.right) / 2, r.top + pad, 2, r.bottom - r.top - 2 * pad);
    }
    void show_all(bool shown) override { show_control(h_, shown && visible); }

private:
    win_context& win_;
    orientation dir_;
    HWND h_;
};

// A titled frame around one item.
// A group box: its frame and title are drawn by the window it's in, as
// part of that window's background (frames_of), rather than by a control of
// its own. A BUTTON group box painted after the controls inside it covered
// them until they repainted, and repainted whole on every layout that
// resized it (about 1.5 ms apiece); this costs microseconds, and never
// paints over its contents.
class group_item : public container_item
{
public:
    group_item(win_context& win, HWND parent, const std::string& title)
        : win_(win)
        , parent_(parent)
        , title_(wide(title))
    {
        frames_of()[parent_].push_back(this);
    }
    ~group_item() override
    {
        forget_theme();
        invalidate();
        auto& list = frames_of()[parent_];
        list.erase(std::remove(list.begin(), list.end(), this), list.end());
        if (list.empty()) frames_of().erase(parent_);
    }
    item* content = nullptr;
    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        const SIZE c = content ? content->natural() : SIZE{0, 0};
        return {std::max(c.cx, title_size().cx + win_.px(16)) + 2 * pad(), c.cy + top() + pad()};
    }
    long measure_height(long width) override
    {
        return (content ? content->height_at(width - 2 * pad()) : 0) + top() + pad();
    }
    void place(RECT r) override
    {
        placed(r);
        if (!EqualRect(&r, &rect_))
        {
            invalidate();
            rect_ = r;
            invalidate();
        }
        if (content) content->place({r.left + pad(), r.top + top(), r.right - pad(), r.bottom - pad()});
    }
    void show_all(bool shown) override
    {
        showing(shown);
        if (shown && visible) shown_ = true, invalidate();
        else invalidate(), shown_ = false;
        if (content) content->show_all(shown && visible);
    }

    // Draws the frame and title over the window's background, if the area
    // being painted (a child's background, say) reaches them.
    void paint(HDC dc)
    {
        if (!shown_ || IsRectEmpty(&rect_)) return;
        RECT clip, overlap;
        if (GetClipBox(dc, &clip) == NULLREGION) return;
        bool reached = false;
        for (const RECT& b : bands()) reached = reached || IntersectRect(&overlap, &b, &clip);
        if (!reached) return;
        HGDIOBJ old_font = SelectObject(dc, ui_font(win_.dpi()));
        SIZE t{};
        GetTextExtentPoint32W(dc, title_.c_str(), int(title_.size()), &t);
        RECT frame = rect_;
        frame.top += t.cy / 2;
        if (!theme_) theme_ = OpenThemeData(parent_, L"BUTTON");
        // In dark mode the style's frame and caption are a light theme's:
        // drawn here in the dark colours instead.
        HTHEME theme = dark_mode() ? nullptr : theme_;
        if (dark_mode())
            FrameRect(dc, &frame, theme_brush(theme_color::border));
        else if (theme)
            DrawThemeBackground(theme, dc, BP_GROUPBOX, GBS_NORMAL, &frame, nullptr);
        else
            DrawEdge(dc, &frame, EDGE_ETCHED, BF_RECT);
        if (!title_.empty())
        {
            // The title sits in a gap in the top line.
            RECT text{rect_.left + win_.px(9), rect_.top, rect_.left + win_.px(9) + t.cx, rect_.top + t.cy};
            RECT gap = text;
            InflateRect(&gap, win_.px(2), 0);
            FillRect(dc, &gap, theme_brush(surface_of(parent_)));
            SetBkMode(dc, TRANSPARENT);
            if (theme)
                DrawThemeText(theme, dc, BP_GROUPBOX, GBS_NORMAL, title_.c_str(), -1, DT_SINGLELINE | DT_LEFT, 0,
                              &text);
            else
            {
                SetTextColor(dc, color_ref(theme_color_of(surface_text_of(parent_))));
                DrawTextW(dc, title_.c_str(), -1, &text, DT_SINGLELINE | DT_LEFT);
            }
        }
        SelectObject(dc, old_font);
    }
    void forget_theme()
    {
        if (theme_) CloseThemeData(theme_);
        theme_ = nullptr;
    }

    // The group boxes each window draws, by window.
    static std::map<HWND, std::vector<group_item*>>& frames_of()
    {
        static std::map<HWND, std::vector<group_item*>> frames;
        return frames;
    }

private:
    int pad() const { return win_.px(9); }
    int top() const { return font_metrics(win_.dpi()).cy + win_.px(4); }
    SIZE title_size() const
    {
        HDC dc = GetDC(parent_);
        HGDIOBJ old = SelectObject(dc, ui_font(win_.dpi()));
        SIZE t{};
        GetTextExtentPoint32W(dc, title_.c_str(), int(title_.size()), &t);
        SelectObject(dc, old);
        ReleaseDC(parent_, dc);
        return t;
    }
    // Asks the window to redraw the frame's lines and title (the inside is
    // the window's to repaint as its controls move).
    void invalidate()
    {
        if (!shown_ || IsRectEmpty(&rect_)) return;
        for (const RECT& b : bands()) InvalidateRect(parent_, &b, TRUE);
    }
    // Where the frame draws: the top (title and line) and the other edges.
    std::array<RECT, 4> bands() const
    {
        const int line = win_.px(3);
        return {{
            {rect_.left, rect_.top, rect_.right, rect_.top + top()},
            {rect_.left, rect_.top, rect_.left + line, rect_.bottom},
            {rect_.right - line, rect_.top, rect_.right, rect_.bottom},
            {rect_.left, rect_.bottom - line, rect_.right, rect_.bottom},
        }};
    }

    win_context& win_;
    HWND parent_;
    std::wstring title_;
    RECT rect_{};
    bool shown_ = false;
    HTHEME theme_ = nullptr;
};

void forget_group_themes(HWND h)
{
    auto& frames = group_item::frames_of();
    if (auto it = frames.find(h); it != frames.end())
        for (group_item* g : it->second) g->forget_theme();
}

// Names `h` for screen readers after the frame around it, as Windows did
// when frames were controls (the control after a group box took its title),
// for the kinds of control that take their names from a label.
void name_from_frame(HWND h, const std::wstring& title)
{
    static const wchar_t* const takes_label[] = {L"Edit", L"RICHEDIT50W", L"SysListView32", L"ComboBox",
                                                 L"SysDateTimePick32", L"SysTreeView32"};
    wchar_t cls[32];
    GetClassNameW(h, cls, 32);
    if (std::none_of(std::begin(takes_label), std::end(takes_label), [&](const wchar_t* c) { return !wcscmp(c, cls); }))
        return;
    // CLSID_AccPropServices, IID_IAccPropServices and PROPID_ACC_NAME:
    // declared here, as MinGW and the SDK put them in different libraries.
    static const GUID clsid{0xb5f8350b, 0x0548, 0x48b1, {0xa6, 0xee, 0x88, 0xbd, 0x00, 0xb4, 0xa5, 0xe7}};
    static const GUID iid{0x6e26e776, 0x04f0, 0x495d, {0x80, 0xe4, 0x33, 0x30, 0x35, 0x2e, 0x31, 0x69}};
    static const GUID name{0x608d3df8, 0x8128, 0x4aa7, {0xa4, 0x28, 0xf5, 0x5e, 0x49, 0x26, 0x72, 0x91}};
    static IAccPropServices* services = []
    {
        IAccPropServices* s = nullptr;
        CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, reinterpret_cast<void**>(&s));
        return s;
    }();
    if (services) services->SetHwndPropStr(h, DWORD(OBJID_CLIENT), CHILDID_SELF, name, title.c_str());
}

bool paint_group_frames(HWND h, HDC dc)
{
    auto& frames = group_item::frames_of();
    auto it = frames.find(h);
    if (it == frames.end()) return false;
    for (group_item* g : it->second) g->paint(dc);
    return true;
}

class progress_bar_impl : public leaf<rap::progress_bar>
{
public:
    progress_bar_impl(win_context& win, HWND parent)
        : leaf(win)
    {
        adopt(create_control(win, parent, PROGRESS_CLASSW, L"", 0));
        SendMessageW(h_, PBM_SETRANGE32, 0, 1000);
    }
    void set_fraction(float f) override
    {
        if (marquee_)
        {
            SetWindowLongPtrW(h_, GWL_STYLE, GetWindowLongPtrW(h_, GWL_STYLE) & ~PBS_MARQUEE);
            SendMessageW(h_, PBM_SETMARQUEE, FALSE, 0);
            marquee_ = false;
        }
        // The themed bar animates every increase, so an instant operation's
        // 100% still crawled across. A decrease shows at once: go one past
        // the value, then back (at 100%, the range grows by one meanwhile).
        const int pos = int(std::lround(std::clamp(f, 0.f, 1.f) * 1000));
        if (pos == 1000) SendMessageW(h_, PBM_SETRANGE32, 0, 1001);
        SendMessageW(h_, PBM_SETPOS, WPARAM(pos + 1), 0);
        SendMessageW(h_, PBM_SETPOS, WPARAM(pos), 0);
        if (pos == 1000) SendMessageW(h_, PBM_SETRANGE32, 0, 1000);
    }
    void pulse() override
    {
        if (marquee_) return;
        SetWindowLongPtrW(h_, GWL_STYLE, GetWindowLongPtrW(h_, GWL_STYLE) | PBS_MARQUEE);
        SendMessageW(h_, PBM_SETMARQUEE, TRUE, 30);
        marquee_ = true;
    }
    SIZE measure() override { return {px(160), px(16)}; }

private:
    bool marquee_ = false;
};

// -- tree ---------------------------------------------------------------------

// The common controls' tree view. Items are found by the app's ids; each
// item's lParam is its index into ids_. Boxes are state images of our own
// (TVS_CHECKBOXES has no mixed state, and flips them itself): 1 unchecked,
// 2 checked, 3 mixed, 0 none. A node with children not loaded yet holds one
// placeholder item, which is what gives it a chevron: its lParam is
// k_placeholder, and it can't be selected.
class tree_impl : public leaf<rap::tree>
{
public:
    tree_impl(win_context& win, HWND parent, const spec::tree& s)
        : leaf(win)
    {
        DWORD style = TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT | WS_TABSTOP;
        // As Explorer's: long names clip, and their tooltips show them whole.
        if (s.navigator && windows_build() >= 22000) style |= TVS_NOHSCROLL;
        adopt(create_control(win, parent, WC_TREEVIEWW, L"", style, s.frame ? WS_EX_CLIENTEDGE : 0));
        // Explorer's look (chevrons, hover, no lines), as Windows' own trees
        // have it (its dark one in dark mode).
        theme_control(h_);
        TreeView_SetExtendedStyle(h_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
        if (s.navigator && windows_build() >= 22000)
        {
            navigator_ = true;
            // A new DPI comes with a new font: the spacing follows it.
            SetWindowSubclass(
                h_,
                [](HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) -> LRESULT
                {
                    const LRESULT result = DefSubclassProc(h, msg, wp, lp);
                    if (msg == WM_SETFONT) reinterpret_cast<tree_impl*>(data)->apply_navigator_spacing();
                    return result;
                },
                1, reinterpret_cast<DWORD_PTR>(this));
            apply_navigator_spacing();
        }
        SetWindowSubclass(h_, key_proc, 2, reinterpret_cast<DWORD_PTR>(this));
        set_items(s.items);
    }
    ~tree_impl() override
    {
        if (state_images_) ImageList_Destroy(state_images_);
        if (icons_) ImageList_Destroy(icons_);
    }

    void theme_changed() override
    {
        if (navigator_) apply_navigator_spacing();
        rebuild_icons();
    }
    // Windows 11 Explorer's own, measured from its tree at 96 DPI.
    void apply_navigator_spacing()
    {
        // Undocumented, and set on Explorer's own tree: the expander keeps its
        // full size when the indent is narrower than it.
        constexpr DWORD tvs_ex_full_size_expanders = 0x1000;
        TreeView_SetExtendedStyle(h_, tvs_ex_full_size_expanders, tvs_ex_full_size_expanders);
        TreeView_SetIndent(h_, px(8));
        TreeView_SetItemHeight(h_, px(32));
    }
    bool navigator_ = false;
    std::unordered_set<std::string> lines_;  // ids with tree_item::separator_before

    void set_items(std::vector<tree_item> items) override
    {
        lazy_tree::tracker::batch b(lazy_);
        // How the user left things, by id.
        const std::optional<std::string> keep = selected();
        std::unordered_map<std::string, bool> open;
        for (auto& [id, h] : by_id_)
            open[id] = (TreeView_GetItemState(h_, h, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
        quiet q(*this);
        SendMessageW(h_, WM_SETREDRAW, FALSE, 0);
        TreeView_DeleteAllItems(h_);
        ids_.clear();
        free_ids_.clear();
        by_id_.clear();
        lines_.clear();
        checks_.clear();
        lazy_.clear();
        if (!state_images_ && has_check(items)) make_state_images();
        insert(items, TVI_ROOT, open);
        if (keep) select_quietly(*keep);
        SendMessageW(h_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(h_, nullptr, TRUE);
    }
    std::optional<std::string> selected() const override
    {
        const HTREEITEM h = TreeView_GetSelection(h_);
        return h ? std::optional<std::string>(id_of(h)) : std::nullopt;
    }
    void select(const std::string& id) override
    {
        lazy_tree::tracker::batch b(lazy_);
        quiet q(*this);
        if (!select_quietly(id)) TreeView_SelectItem(h_, nullptr);
    }
    void set_expanded(const std::string& id, bool open) override
    {
        const auto it = by_id_.find(id);
        if (it == by_id_.end()) return;
        // Asks wait until the control's finished: a handler may destroy it.
        lazy_tree::tracker::batch b(lazy_);
        TreeView_Expand(h_, it->second, open ? TVE_EXPAND : TVE_COLLAPSE);
        // TVM_EXPAND stops notifying after a node's first time.
        if (open) lazy_.opened(id);
    }
    void on_selection_changed(std::function<void()> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void(const std::string&)> f) override { activate_ = std::move(f); }
    void on_context_menu(std::function<menu(std::optional<std::string>)> f) override { context_ = std::move(f); }
    void set_check(const std::string& id, check_state st) override
    {
        const auto c = checks_.find(id);
        if (c == checks_.end()) return;
        c->second = st;
        TreeView_SetItemState(h_, by_id_[id], INDEXTOSTATEIMAGEMASK(int(st) + 1),
                              TVIS_STATEIMAGEMASK);
    }
    void on_toggled(std::function<void(const std::string&)> f) override { toggled_ = std::move(f); }
    void on_expanding(std::function<void(const std::string&)> f) override { lazy_.on_request(std::move(f)); }

    // No WM_SETREDRAW here, unlike set_items: it hides a control from DWM
    // while it's off, which flickers on a live navigator, and a batch from
    // one message repaints afterwards anyway.
    void set_children(const std::string& id, std::vector<tree_item> items) override
    {
        const auto it = by_id_.find(id);
        if (it == by_id_.end()) return;
        lazy_tree::tracker::batch b(lazy_);
        quiet q(*this);
        const HTREEITEM parent = it->second;
        const std::optional<std::string> keep = selected();
        std::unordered_map<std::string, bool> open;
        for (auto& [other, h] : by_id_)
            open[other] = (TreeView_GetItemState(h_, h, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
        const bool was_open = open[id];
        // The new ones go in before the old come out, so the node is never
        // empty and doesn't collapse in between.
        std::vector<HTREEITEM> old;
        for (HTREEITEM c = TreeView_GetChild(h_, parent); c; c = TreeView_GetNextSibling(h_, c)) old.push_back(c);
        for (const HTREEITEM c : old) forget_subtree(c);
        lazy_.forget(id);
        if (!state_images_ && has_check(items)) make_state_images();
        insert(items, parent, open);
        for (const HTREEITEM c : old) TreeView_DeleteItem(h_, c);
        if (items.empty())
        {
            TreeView_SetItemState(h_, parent, 0, TVIS_EXPANDED | TVIS_EXPANDEDONCE);
        }
        else if (was_open)
            TreeView_Expand(h_, parent, TVE_EXPAND);
        restore_selection(keep);
    }
    void set_separator_before(const std::string& id, bool on) override
    {
        const auto it = by_id_.find(id);
        if (it == by_id_.end() || lines_.contains(id) == on) return;
        if (on)
            lines_.insert(id);
        else
            lines_.erase(id);
        RECT r{};
        if (TreeView_GetItemRect(h_, it->second, &r, FALSE)) InvalidateRect(h_, &r, FALSE);
    }
    void update_item(const std::string& id, const std::string& label, const rap::icon& icon, bool has_children) override
    {
        const auto it = by_id_.find(id);
        if (it == by_id_.end()) return;
        lazy_tree::tracker::batch b(lazy_);
        const HTREEITEM h = it->second;
        std::wstring text = wide(label);
        TVITEMW item{};
        item.mask = TVIF_TEXT;
        item.hItem = h;
        item.pszText = text.data();
        if (icon || icons_)
        {
            item.mask |= TVIF_IMAGE | TVIF_SELECTEDIMAGE;
            item.iImage = item.iSelectedImage = image_of(icon);
        }
        TreeView_SetItem(h_, &item);
        const bool waiting = lazy_.unloaded(id);
        if (!has_children && waiting)
            set_children(id, {});
        else if (has_children && !waiting && !TreeView_GetChild(h_, h))
        {
            lazy_.mark(id);
            add_placeholder(h);
        }
    }

    LRESULT notify(NMHDR* n, bool& handled) override
    {
        switch (n->code)
        {
        case NM_CLICK:
        {
            // A click on an item's box: reported, the app sets the state.
            TVHITTESTINFO hit{};
            GetCursorPos(&hit.pt);
            ScreenToClient(h_, &hit.pt);
            const HTREEITEM h = TreeView_HitTest(h_, &hit);
            if (!h || !(hit.flags & TVHT_ONITEMSTATEICON)) return 0;
            const std::string id = id_of(h);
            if (!checks_.contains(id)) return 0;
            if (toggled_) toggled_(id);
            return 1;
        }
        case TVN_SELCHANGINGW:
        {
            // The arrow keys step past a placeholder (key_proc): this stops
            // the mouse and the paging keys, which key_proc then moves on.
            const HTREEITEM to = reinterpret_cast<NMTREEVIEWW*>(n)->itemNew.hItem;
            if (quiet_ || !is_placeholder(to)) return 0;
            refused_ = to;
            return 1;
        }
        case TVN_ITEMEXPANDEDW:
        {
            const auto* tv = reinterpret_cast<NMTREEVIEWW*>(n);
            if (tv->action & TVE_EXPAND) lazy_.opened(id_of(tv->itemNew.hItem));
            return 0;
        }
        case NM_CUSTOMDRAW:
        {
            // tree_item::separator_before: drawn after the row is, so its
            // selection, hover and focus can't cover it.
            auto* cd = reinterpret_cast<NMTVCUSTOMDRAW*>(n);
            const auto item = reinterpret_cast<HTREEITEM>(cd->nmcd.dwItemSpec);
            switch (cd->nmcd.dwDrawStage)
            {
            case CDDS_PREPAINT: return lines_.empty() ? CDRF_DODEFAULT : CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: return draws_line(item) ? CDRF_NOTIFYPOSTPAINT : CDRF_DODEFAULT;
            case CDDS_ITEMPOSTPAINT: draw_line(cd->nmcd.hdc, item); return CDRF_DODEFAULT;
            default: return CDRF_DODEFAULT;
            }
        }
        case TVN_SELCHANGEDW:
            if (!quiet_ && changed_) changed_();
            return 0;
        case NM_DBLCLK:
            if (const HTREEITEM h = item_at_cursor(); h && activate_) activate_(id_of(h));
            return 0;
        case NM_RCLICK:
        {
            // The item under the pointer is selected first, as Explorer does.
            const HTREEITEM h = item_at_cursor();
            if (h && h != TreeView_GetSelection(h_)) TreeView_SelectItem(h_, h);
            POINT pt;
            GetCursorPos(&pt);
            show_menu(h, pt);
            return 1;  // no WM_CONTEXTMENU as well
        }
        case TVN_KEYDOWN:
        {
            const WORD key = reinterpret_cast<NMTVKEYDOWN*>(n)->wVKey;
            const HTREEITEM h = TreeView_GetSelection(h_);
            if (key == VK_RETURN && h && activate_)
            {
                activate_(id_of(h));
                return 1;
            }
            if (key == VK_SPACE && h && checks_.contains(id_of(h)))
            {
                if (toggled_) toggled_(id_of(h));
                return 1;
            }
            if (key == VK_APPS || (key == VK_F10 && GetKeyState(VK_SHIFT) < 0))
            {
                // The menu key: under the selected item, or the control's corner.
                RECT r{};
                if (!h || !TreeView_GetItemRect(h_, h, &r, TRUE)) r = {0, 0, 0, 0};
                POINT pt{r.left, r.bottom};
                ClientToScreen(h_, &pt);
                show_menu(h, pt);
                return 1;
            }
            return 0;
        }
        default: handled = false; return 0;
        }
    }

    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override { return {px(120), font_metrics(win_.dpi()).cy * 8}; }

private:
    // Selection changes the app makes don't call it back.
    struct quiet
    {
        explicit quiet(tree_impl& t)
            : t(t)
            , was(t.quiet_)
        {
            t.quiet_ = true;
        }
        ~quiet() { t.quiet_ = was; }
        tree_impl& t;
        bool was;
    };

    void insert(const std::vector<tree_item>& items, HTREEITEM parent, const std::unordered_map<std::string, bool>& open)
    {
        for (const tree_item& i : items)
        {
            std::wstring text = wide(i.label);
            TVINSERTSTRUCTW in{};
            in.hParent = parent;
            in.hInsertAfter = TVI_LAST;
            in.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
            in.item.pszText = text.data();
            size_t slot = ids_.size();
            if (!free_ids_.empty())
            {
                slot = free_ids_.back();
                free_ids_.pop_back();
            }
            in.item.lParam = LPARAM(slot);
            in.item.stateMask = TVIS_STATEIMAGEMASK;
            in.item.state = INDEXTOSTATEIMAGEMASK(i.check ? int(*i.check) + 1 : 0);
            if (i.icon)
            {
                in.item.mask |= TVIF_IMAGE | TVIF_SELECTEDIMAGE;
                in.item.iImage = in.item.iSelectedImage = image_of(i.icon);
            }
            if (i.check) checks_[i.id] = *i.check;
            const HTREEITEM h = TreeView_InsertItem(h_, &in);
            if (!h)
            {
                if (slot != ids_.size()) free_ids_.push_back(slot);
                continue;
            }
            if (slot == ids_.size())
                ids_.push_back(i.id);
            else
                ids_[slot] = i.id;
            by_id_[i.id] = h;
            if (i.separator_before)
                lines_.insert(i.id);
            else
                lines_.erase(i.id);
            if (i.has_children && i.children.empty())
            {
                lazy_.mark(i.id);
                add_placeholder(h);
            }
            insert(i.children, h, open);
            const auto was = open.find(i.id);
            if ((!i.children.empty() || lazy_.unloaded(i.id)) && (was != open.end() ? was->second : i.expanded))
            {
                TreeView_Expand(h_, h, TVE_EXPAND);
                lazy_.opened(i.id);
            }
        }
    }
    // An item's line, when it has one and a sibling before it.
    bool draws_line(HTREEITEM h) const
    {
        if (!h || is_placeholder(h) || !TreeView_GetPrevSibling(h_, h)) return false;
        return lines_.contains(id_of(h));
    }
    // Along the top of the row, from where the item's icon (else its label)
    // starts, so lines line up with the items at their level; the divider
    // colour (a line between items, as a thin splitter's), a system one in
    // high contrast.
    void draw_line(HDC dc, HTREEITEM h)
    {
        RECT row{}, text{};
        if (!TreeView_GetItemRect(h_, h, &row, FALSE) || !TreeView_GetItemRect(h_, h, &text, TRUE)) return;
        int left = text.left;
        TVITEMW item{};
        item.mask = TVIF_IMAGE;
        item.hItem = h;
        int cx = 0, cy = 0;
        if (icons_ && TreeView_GetItem(h_, &item) && item.iImage > 0 && ImageList_GetIconSize(icons_, &cx, &cy))
            left -= cx + px(3);
        RECT line{std::max(0L, long(left)), row.top, row.right, row.top + std::max(1, px(1))};
        FillRect(dc, &line, theme_brush(theme_color::divider));
    }
    void add_placeholder(HTREEITEM parent)
    {
        std::wstring text = wide(rap::i18n::tr("rapport", "Loading…"));
        TVINSERTSTRUCTW in{};
        in.hParent = parent;
        in.hInsertAfter = TVI_LAST;
        in.item.mask = TVIF_TEXT | TVIF_PARAM;
        in.item.pszText = text.data();
        in.item.lParam = k_placeholder;
        TreeView_InsertItem(h_, &in);
    }
    LPARAM param_of(HTREEITEM h) const
    {
        TVITEMW item{};
        item.mask = TVIF_PARAM;
        item.hItem = h;
        TreeView_GetItem(h_, &item);
        return item.lParam;
    }
    bool is_placeholder(HTREEITEM h) const { return h && param_of(h) == k_placeholder; }
    // Everything we know about an item and what's under it; the control's
    // items stay until they're deleted.
    void forget_subtree(HTREEITEM h)
    {
        for (HTREEITEM c = TreeView_GetChild(h_, h); c; c = TreeView_GetNextSibling(h_, c)) forget_subtree(c);
        const LPARAM slot = param_of(h);
        if (slot == k_placeholder || size_t(slot) >= ids_.size()) return;
        const std::string id = ids_[size_t(slot)];
        by_id_.erase(id);
        checks_.erase(id);
        lines_.erase(id);
        lazy_.forget(id);
        free_ids_.push_back(size_t(slot));
    }
    void restore_selection(const std::optional<std::string>& keep)
    {
        if (!keep) return;
        const auto it = by_id_.find(*keep);
        if (it == by_id_.end())
            TreeView_SelectItem(h_, nullptr);
        else if (TreeView_GetSelection(h_) != it->second)
            TreeView_SelectItem(h_, it->second);
    }
    static LRESULT CALLBACK key_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<tree_impl*>(data);
        if (msg == WM_KEYDOWN && self->step_past_placeholder(wp)) return 0;
        self->refused_ = nullptr;
        const LRESULT result = DefSubclassProc(h, msg, wp, lp);
        if (msg == WM_KEYDOWN && (wp == VK_NEXT || wp == VK_PRIOR) && self->refused_) self->page_past_placeholder(wp);
        if (msg == WM_SETFONT) self->rebuild_icons();
        return result;
    }
    // Where an arrow key would land on a placeholder it goes on past it, or
    // stays where it is: selecting one is refused, and a keyboard stopped by
    // that would be stuck.
    bool step_past_placeholder(WPARAM key)
    {
        // Ctrl with the arrows scrolls: the selection stays, so nothing lands.
        if (GetKeyState(VK_CONTROL) < 0) return false;
        const HTREEITEM from = TreeView_GetSelection(h_);
        if (!from) return false;
        HTREEITEM to = nullptr;
        switch (key)
        {
        case VK_DOWN: to = TreeView_GetNextVisible(h_, from); break;
        case VK_UP: to = TreeView_GetPrevVisible(h_, from); break;
        case VK_END: to = TreeView_GetLastVisible(h_); break;
        case VK_RIGHT:
            if (TreeView_GetItemState(h_, from, TVIS_EXPANDED) & TVIS_EXPANDED) to = TreeView_GetChild(h_, from);
            break;
        default: return false;
        }
        if (!is_placeholder(to)) return false;
        const HTREEITEM past = key == VK_DOWN                  ? TreeView_GetNextVisible(h_, to)
                               : key == VK_UP || key == VK_END ? TreeView_GetPrevVisible(h_, to)
                                                               : nullptr;
        if (past && past != from)
        {
            TreeView_SelectItem(h_, past);
            TreeView_EnsureVisible(h_, past);
        }
        return true;
    }
    // A page key landed on a placeholder and was refused: on to the nearest
    // row in its direction, or back the other way at the end of the tree.
    // Refused and left there, the same press would land there again.
    void page_past_placeholder(WPARAM key)
    {
        const auto nearest = [&](bool down)
        {
            HTREEITEM h = refused_;
            do
                h = down ? TreeView_GetNextVisible(h_, h) : TreeView_GetPrevVisible(h_, h);
            while (h && is_placeholder(h));
            return h;
        };
        const bool down = key == VK_NEXT;
        HTREEITEM to = nearest(down);
        if (!to) to = nearest(!down);
        if (!to || to == TreeView_GetSelection(h_)) return;
        TreeView_SelectItem(h_, to);
        TreeView_EnsureVisible(h_, to);
    }
    // Image 0 is blank, for what has no icon. The list is only made when an
    // item has an icon, so a tree with none is as it always was.
    int image_of(const rap::icon& icon)
    {
        if (!icon) return 0;
        if (!icons_)
        {
            icons_ = ImageList_Create(px(16), px(16), ILC_COLOR32 | ILC_MASK, 8, 8);
            if (!icons_) return 0;
            add_blank(icons_);
            TreeView_SetImageList(h_, icons_, TVSIL_NORMAL);
        }
        const std::string key = icon_key(icon);
        if (!key.empty())
            if (const auto it = image_index_.find(key); it != image_index_.end()) return it->second;
        const int image = ImageList_GetImageCount(icons_);
        add_image(icons_, icon);
        image_sources_.push_back(icon);
        if (!key.empty()) image_index_[key] = image;
        return image;
    }
    static std::string icon_key(const rap::icon& icon)
    {
        if (icon.file.empty()) return icon.name.empty() ? std::string() : "n:" + icon.name;
        if (icon.file.back() == '/') return "f:/";
        const size_t slash = icon.file.find_last_of("/\\");
        const std::string base = icon.file.substr(slash == std::string::npos ? 0 : slash + 1);
        const size_t dot = base.rfind('.');
        return "f:" + (dot == std::string::npos ? base : base.substr(dot));
    }
    void add_image(HIMAGELIST list, const rap::icon& icon)
    {
        HICON h = nullptr;
        if (!icon.file.empty())
        {
            const bool folder = icon.file.back() == '/';
            const std::string key = icon_key(icon).substr(2);
            const std::wstring probe = folder ? L"folder" : key.starts_with('.') ? L"file" + wide(key) : wide(key);
            SHFILEINFOW info{};
            if (SHGetFileInfoW(probe.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &info, sizeof info,
                               SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES))
                h = info.hIcon;
        }
        else
            h = render_icon_handle(icon, px(16), win_.scale());
        if (h)
        {
            ImageList_ReplaceIcon(list, -1, h);
            DestroyIcon(h);
        }
        else
            add_blank(list);  // keeps the numbering
    }
    // By mask: a 32-bit bitmap whose alpha is all zero counts as having none,
    // and draws black.
    void add_blank(HIMAGELIST list)
    {
        const int size = px(16);
        const size_t bytes = size_t((size + 15) / 16 * 2) * size;
        const std::vector<BYTE> transparent(bytes, 0xFF), colour(bytes, 0);
        if (HICON h = CreateIcon(instance(), size, size, 1, 1, transparent.data(), colour.data()))
        {
            ImageList_ReplaceIcon(list, -1, h);
            DestroyIcon(h);
        }
    }
    // At a new DPI or theme: the same images, so the same numbers.
    void rebuild_icons()
    {
        if (!icons_) return;
        HIMAGELIST fresh = ImageList_Create(px(16), px(16), ILC_COLOR32 | ILC_MASK, 8, 8);
        if (!fresh) return;
        add_blank(fresh);
        for (const rap::icon& icon : image_sources_) add_image(fresh, icon);
        TreeView_SetImageList(h_, fresh, TVSIL_NORMAL);
        ImageList_Destroy(icons_);
        icons_ = fresh;
    }
    bool select_quietly(const std::string& id)
    {
        const auto it = by_id_.find(id);
        if (it == by_id_.end()) return false;
        TreeView_SelectItem(h_, it->second);  // opens its ancestors
        TreeView_EnsureVisible(h_, it->second);
        return true;
    }
    static bool has_check(const std::vector<tree_item>& items)
    {
        for (const tree_item& i : items)
            if (i.check || has_check(i.children)) return true;
        return false;
    }
    // The theme's check box in each state (the classic one without a
    // theme), at the window's dpi when the first box is needed.
    void make_state_images()
    {
        const int n = px(13);
        state_images_ = ImageList_Create(n, n, ILC_COLOR32, 4, 0);
        if (!state_images_) return;
        BITMAPINFO bi{};
        bi.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER),
                        .biWidth = n,
                        .biHeight = -n,
                        .biPlanes = 1,
                        .biBitCount = 32,
                        .biCompression = BI_RGB};
        HDC dc = CreateCompatibleDC(nullptr);
        HTHEME theme = OpenThemeData(h_, L"BUTTON");
        // Image 0 stays empty: state image 0 is "none".
        for (int image = 0; image < 4; ++image)
        {
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bmp) break;
            HGDIOBJ old = SelectObject(dc, bmp);
            RECT r{0, 0, n, n};
            const check_state st = check_state(std::max(image - 1, 0));
            if (image > 0 && theme)
                DrawThemeBackground(theme, dc, BP_CHECKBOX,
                                    st == check_state::checked         ? CBS_CHECKEDNORMAL
                                    : st == check_state::indeterminate ? CBS_MIXEDNORMAL
                                                                       : CBS_UNCHECKEDNORMAL,
                                    &r, nullptr);
            else if (image > 0)
            {
                DrawFrameControl(dc, &r, DFC_BUTTON,
                                 DFCS_BUTTONCHECK |
                                     (st != check_state::unchecked ? DFCS_CHECKED : 0) |
                                     (st == check_state::indeterminate ? DFCS_INACTIVE : 0));
                auto* px32 = static_cast<uint32_t*>(bits);
                for (int k = 0; k < n * n; ++k) px32[k] |= 0xFF000000u;  // GDI leaves alpha 0
            }
            GdiFlush();
            SelectObject(dc, old);
            ImageList_Add(state_images_, bmp, nullptr);
            DeleteObject(bmp);
        }
        if (theme) CloseThemeData(theme);
        DeleteDC(dc);
        TreeView_SetImageList(h_, state_images_, TVSIL_STATE);
    }
    std::string id_of(HTREEITEM h) const
    {
        TVITEMW item{};
        item.mask = TVIF_PARAM;
        item.hItem = h;
        TreeView_GetItem(h_, &item);
        return size_t(item.lParam) < ids_.size() ? ids_[size_t(item.lParam)] : std::string();
    }
    HTREEITEM item_at_cursor() const
    {
        TVHITTESTINFO hit{};
        GetCursorPos(&hit.pt);
        ScreenToClient(h_, &hit.pt);
        const HTREEITEM h = TreeView_HitTest(h_, &hit);
        return h && (hit.flags & TVHT_ONITEM) && !is_placeholder(h) ? h : nullptr;
    }
    void show_menu(HTREEITEM h, POINT screen)
    {
        if (!context_) return;
        const menu m = context_(h ? std::optional<std::string>(id_of(h)) : std::nullopt);
        if (!m.empty()) show_popup_menu(GetAncestor(h_, GA_ROOT), m, screen);
    }

    std::function<void()> changed_;
    std::function<void(const std::string&)> activate_;
    std::function<menu(std::optional<std::string>)> context_;
    static constexpr LPARAM k_placeholder = -1;
    std::vector<std::string> ids_;
    std::vector<size_t> free_ids_;  // in ids_, left by what set_children removed
    std::unordered_map<std::string, HTREEITEM> by_id_;
    lazy_tree::tracker lazy_;
    HTREEITEM refused_ = nullptr;  // the placeholder a selection just tried to land on
    HIMAGELIST icons_ = nullptr;
    std::vector<rap::icon> image_sources_;  // image 1 on
    std::unordered_map<std::string, int> image_index_;
    std::unordered_map<std::string, check_state> checks_;  // the items with boxes
    std::function<void(const std::string&)> toggled_;
    HIMAGELIST state_images_ = nullptr;
    bool quiet_ = false;
};

class date_picker_impl : public leaf<rap::date_picker>
{
public:
    date_picker_impl(win_context& win, HWND parent, const spec::date_picker& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, DATETIMEPICK_CLASSW, L"", DTS_SHORTDATEFORMAT | WS_TABSTOP));
        set_date(s.date);
    }
    rap::date date() const override
    {
        SYSTEMTIME st{};
        SendMessageW(h_, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&st));
        return rap::date{st.wYear, st.wMonth, st.wDay};
    }
    void set_date(rap::date d) override
    {
        SYSTEMTIME st{};
        st.wYear = WORD(d.year);
        st.wMonth = WORD(d.month);
        st.wDay = WORD(d.day);
        quiet_ = true;
        SendMessageW(h_, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&st));
        quiet_ = false;
    }
    void on_changed(std::function<void(rap::date)> f) override { changed_ = std::move(f); }
    LRESULT notify(NMHDR* n, bool& handled) override
    {
        handled = n->code == DTN_DATETIMECHANGE;
        if (handled && !quiet_ && changed_) changed_(date());
        return 0;
    }
    SIZE measure() override { return {font_metrics(win_.dpi()).cx * 12 + px(30), font_metrics(win_.dpi()).cy + px(10)}; }

private:
    bool quiet_ = false;
    std::function<void(rap::date)> changed_;
};

// A number in an edit with an up/down control beside it (the edit is the
// HWND laid out; the arrows ride on its right edge).
class spin_impl : public leaf<rap::spin>
{
public:
    spin_impl(win_context& win, HWND parent, const spec::spin& s)
        : leaf(win)
        , min_(s.min)
        , max_(s.max)
        , step_(s.step > 0 ? s.step : 1)
        , digits_(std::max(0, s.digits))
        , value_(s.value)
    {
        adopt(create_control(win, parent, L"EDIT", L"", ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP, WS_EX_CLIENTEDGE));
        updown_ = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | UDS_ARROWKEYS | UDS_ALIGNRIGHT, 0, 0, 0, 0, parent,
                                  nullptr, instance(), nullptr);
        track_container_control(updown_);
        SendMessageW(updown_, UDM_SETBUDDY, reinterpret_cast<WPARAM>(h_), 0);
        SendMessageW(updown_, UDM_SETRANGE32, 0, 100);  // we step ourselves
        SendMessageW(updown_, UDM_SETPOS32, 0, 50);
        handlers()[updown_] = this;
        show();
    }
    ~spin_impl() override
    {
        handlers().erase(updown_);
        DestroyWindow(updown_);
    }
    double value() const override { return value_; }
    void set_value(double v) override
    {
        value_ = std::clamp(v, min_, max_);
        show();
    }
    void set_range(double mn, double mx) override
    {
        min_ = mn;
        max_ = mx;
        set_value(value_);
    }
    void on_changed(std::function<void(double)> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == EN_KILLFOCUS) commit();
    }
    LRESULT notify(NMHDR* n, bool& handled) override
    {
        handled = n->code == UDN_DELTAPOS;
        if (!handled) return 0;
        auto* d = reinterpret_cast<NMUPDOWN*>(n);
        commit();
        set(value_ + (d->iDelta > 0 ? step_ : -step_));
        return 1;  // we moved the value; keep the arrows' own position
    }
    void place(RECT r) override
    {
        const int arrows = px(18);
        leaf::place({r.left, r.top, r.right - arrows, r.bottom});
        move_control(updown_, r.right - arrows, r.top, arrows, r.bottom - r.top);
    }
    void show_all(bool shown) override
    {
        leaf::show_all(shown);
        show_control(updown_, shown && visible);
    }
    void set_enabled(bool e) override
    {
        leaf::set_enabled(e);
        set_control_enabled(updown_, e);
    }
    SIZE measure() override
    {
        const SIZE m = font_metrics(win_.dpi());
        return {m.cx * 8 + px(26), m.cy + px(9)};
    }

private:
    void show()
    {
        wchar_t buf[64];
        swprintf(buf, 64, L"%.*f", digits_, value_);
        quiet_ = true;
        SetWindowTextW(h_, buf);
        quiet_ = false;
    }
    void set(double v)
    {
        v = std::clamp(v, min_, max_);
        const bool changed = v != value_;
        value_ = v;
        show();
        if (changed && changed_) changed_(value_);
    }
    void commit()
    {
        const std::wstring t = window_text(h_);
        wchar_t* end = nullptr;
        const double v = wcstod(t.c_str(), &end);
        if (end != t.c_str()) set(v);
        else show();
    }

    HWND updown_ = nullptr;
    double min_, max_, step_;
    int digits_;
    double value_;
    bool quiet_ = false;
    std::function<void(double)> changed_;
};

// A trackbar works in integers: positions are value / step.
class slider_impl : public leaf<rap::slider>
{
public:
    slider_impl(win_context& win, HWND parent, const spec::slider& s)
        : leaf(win)
        , map_(s.map)
        , min_(map_.track(s.min))
        , max_(map_.track(s.max))
        , step_(s.step > 0 ? s.step : (max_ - min_) / 1000)
        , show_value_(s.show_value)
        , snap_(s.snap)
        , digits_(s.digits)
    {
        adopt(create_control(win, parent, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP));
        SetWindowSubclass(h_, size_without_painting, 5, 0);
        marks_ = s.marks;
        apply_range();
        SendMessageW(h_, TBM_SETLINESIZE, 0, 1);
        SendMessageW(h_, TBM_SETPAGESIZE, 0, LPARAM(std::max(1.0, std::round(s.page / step_))));
        set_value(s.value);
        if (show_value_) value_label_ = create_control(win, parent, L"STATIC", L"", SS_RIGHT);
        update_label();
    }
    ~slider_impl() override
    {
        if (value_label_) DestroyWindow(value_label_);
    }
    // The trackbar's positions are steps along the track from min_; a value
    // set reads back as it was until the slider moves (between two steps,
    // it would drift to the nearer).
    double value() const override
    {
        const LRESULT pos = SendMessageW(h_, TBM_GETPOS, 0, 0);
        return pos == set_.second ? set_.first : map_.value(min_ + double(pos) * step_);
    }
    void set_value(double v) override
    {
        const double t = std::clamp(map_.track(v), min_, max_);
        set_ = {t == map_.track(v) ? v : map_.value(t), std::lround((t - min_) / step_)};
        SendMessageW(h_, TBM_SETPOS, TRUE, set_.second);
        update_label();
    }
    void set_range(double mn, double mx) override
    {
        const double v = value();
        min_ = map_.track(mn);
        max_ = map_.track(mx);
        apply_range();
        set_value(v);
    }
    void set_marks(std::vector<std::pair<double, std::string>> marks) override
    {
        const bool ticks = !marks_.empty();
        const double v = value();
        marks_ = std::move(marks);
        apply_range();
        set_value(v);
        if (ticks != !marks_.empty()) win_.relayout();  // ticks take height
    }
    void on_changed(std::function<void(double)> f) override { changed_ = std::move(f); }
    void scroll(WORD code) override
    {
        if (snap_ && (code == TB_THUMBTRACK || code == TB_THUMBPOSITION)) snap_to_mark();
        update_label();
        if (changed_) changed_(value());
    }
    // A trackbar paints itself there and then as it takes a new size (its
    // WM_SIZE forces its WM_NCPAINT, WM_ERASEBKGND and WM_PAINT): resized in
    // a layout pass's batch, each slider repainted in turn as its move was
    // made, ahead of the rest, and again with them in the pass's one update
    // after. That paint is let go, and it's left to paint with the rest.
    static LRESULT CALLBACK size_without_painting(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR)
    {
        static HWND sizing = nullptr;
        switch (msg)
        {
        case WM_SIZE:
        {
            const HWND outer = sizing;
            sizing = h;
            const LRESULT r = DefSubclassProc(h, msg, wp, lp);
            sizing = outer;
            // On a pane, it's drawn with the pane (paint_pane).
            if (!pane_of(GetParent(h))) RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
            return r;
        }
        case WM_PAINT:
            if (sizing != h) break;
            {
                PAINTSTRUCT ps;
                BeginPaint(h, &ps);
                EndPaint(h, &ps);
            }
            return 0;
        case WM_ERASEBKGND:
            if (sizing == h) return 1;
            break;
        case WM_NCPAINT:
            if (sizing == h) return 0;
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, size_without_painting, id);
            break;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }
    void place(RECT r) override
    {
        const int label = value_label_ ? label_width() : 0;
        leaf::place({r.left, r.top, r.right - label, r.bottom});
        if (value_label_)
            move_control(value_label_, r.right - label, r.top + (r.bottom - r.top - font_metrics(win_.dpi()).cy) / 2, label, font_metrics(win_.dpi()).cy);
    }
    void show_all(bool shown) override
    {
        leaf::show_all(shown);
        if (value_label_) show_control(value_label_, shown && visible);
    }
    SIZE measure() override { return {px(140) + (value_label_ ? label_width() : 0), px(marks_.empty() ? 26 : 30)}; }

private:
    int label_width() const { return font_metrics(win_.dpi()).cx * (digits_ + 5); }
    void apply_range()
    {
        const LPARAM n = LPARAM(std::lround((max_ - min_) / step_));
        SendMessageW(h_, TBM_SETRANGEMIN, FALSE, 0);
        SendMessageW(h_, TBM_SETRANGEMAX, TRUE, n);
        SendMessageW(h_, TBM_CLEARTICS, FALSE, 0);
        const LONG_PTR style = GetWindowLongPtrW(h_, GWL_STYLE) & ~(TBS_NOTICKS | TBS_BOTTOM);
        SetWindowLongPtrW(h_, GWL_STYLE, style | (marks_.empty() ? TBS_NOTICKS : TBS_BOTTOM));
        for (auto& [at, label] : marks_) SendMessageW(h_, TBM_SETTIC, 0, position_of(at));
    }
    LPARAM position_of(double v) const { return LPARAM(std::lround((std::clamp(map_.track(v), min_, max_) - min_) / step_)); }
    // The thumb dragged within a few pixels of a mark lands on it: the
    // mark's value exactly.
    void snap_to_mark()
    {
        RECT channel{};
        SendMessageW(h_, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        const LRESULT positions = SendMessageW(h_, TBM_GETRANGEMAX, 0, 0);
        if (positions <= 0 || channel.right <= channel.left) return;
        const double reach = px(5) * double(positions) / (channel.right - channel.left);
        const LRESULT pos = SendMessageW(h_, TBM_GETPOS, 0, 0);
        const std::pair<double, std::string>* nearest = nullptr;
        for (const auto& m : marks_)
            if (!nearest || std::abs(position_of(m.first) - pos) < std::abs(position_of(nearest->first) - pos)) nearest = &m;
        if (!nearest || std::abs(position_of(nearest->first) - pos) > reach) return;
        set_ = {nearest->first, position_of(nearest->first)};
        SendMessageW(h_, TBM_SETPOS, TRUE, set_.second);
    }
    void update_label()
    {
        if (!value_label_) return;
        wchar_t buf[64];
        swprintf(buf, 64, L"%.*f", digits_, value());
        SetWindowTextW(value_label_, buf);
    }

    slider_map map_;
    double min_, max_, step_;  // along the track
    std::pair<double, LRESULT> set_{0, -1};  // the value last set, and its position
    bool show_value_, snap_;
    int digits_;
    std::vector<std::pair<double, std::string>> marks_;
    HWND value_label_ = nullptr;
    std::function<void(double)> changed_;
};

// An owner-drawn button showing a swatch; clicking opens ChooseColor.
class color_button_impl : public leaf<rap::color_button>
{
public:
    color_button_impl(win_context& win, HWND parent, const spec::color_button& s)
        : leaf(win)
        , color_(s.color)
    {
        adopt(create_control(win, parent, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP));
    }
    rap::color color() const override { return color_; }
    void set_color(rap::color c) override
    {
        color_ = c;
        InvalidateRect(h_, nullptr, TRUE);
    }
    void on_changed(std::function<void(rap::color)> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        if (code != BN_CLICKED) return;
        static COLORREF custom[16];
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof cc;
        cc.hwndOwner = GetAncestor(h_, GA_ROOT);
        cc.rgbResult = color_ref(color_);
        cc.lpCustColors = custom;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        if (!ChooseColorW(&cc)) return;
        color_ = rap::color{GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult), color_.a};
        InvalidateRect(h_, nullptr, TRUE);
        if (changed_) changed_(color_);
    }
    bool draw_item(DRAWITEMSTRUCT* d) override
    {
        RECT r = d->rcItem;
        if (dark_mode())
        {
            // DrawFrameControl's button is the classic light one.
            FillRect(d->hDC, &r,
                     theme_brush((d->itemState & ODS_SELECTED) ? theme_color::pressed
                                                               : theme_color::card));
            FrameRect(d->hDC, &r, theme_brush(theme_color::border));
        }
        else
            DrawFrameControl(d->hDC, &r, DFC_BUTTON,
                             DFCS_BUTTONPUSH | ((d->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        InflateRect(&r, -px(5), -px(5));
        HBRUSH b = CreateSolidBrush(color_ref(color_));
        FillRect(d->hDC, &r, b);
        DeleteObject(b);
        FrameRect(d->hDC, &r, theme_brush(theme_color::border));
        if (d->itemState & ODS_FOCUS)
        {
            InflateRect(&r, px(2), px(2));
            DrawFocusRect(d->hDC, &r);
        }
        return true;
    }
    SIZE measure() override { return {px(44), px(24)}; }

private:
    rap::color color_;
    std::function<void(rap::color)> changed_;
};

class menu_button_impl : public leaf<rap::menu_button>
{
public:
    menu_button_impl(win_context& win, HWND parent, const spec::menu_button& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, L"BUTTON", mnemonic(s.text) + L"  ▾", BS_PUSHBUTTON | WS_TABSTOP));
        text_ = s.text;
        if (s.icon) set_icon(s.icon);
        set_pushed(s.pushed);
    }
    ~menu_button_impl() override
    {
        if (images_) g_comctl.destroy(images_);
    }
    void set_text(const std::string& t) override
    {
        text_ = t;
        SetWindowTextW(h_, (mnemonic(t) + L"  ▾").c_str());
        win_.relayout();
    }
    void set_icon(const icon& icon) override
    {
        if (images_) g_comctl.destroy(images_);
        images_ = nullptr;
        if (!icon) return;
        const int size = px(16);
        images_ = g_comctl.create(size, size, ILC_COLOR32, 1, 0);
        if (HICON hi = render_icon_handle(icon, size, win_.scale()))
        {
            g_comctl.replace_icon(images_, -1, hi);
            DestroyIcon(hi);
        }
        BUTTON_IMAGELIST bil{images_, {px(2), 0, px(2), 0}, BUTTON_IMAGELIST_ALIGN_LEFT};
        SendMessageW(h_, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&bil));
    }
    void set_pushed(bool p) override
    {
        pushed_ = p;
        SendMessageW(h_, BM_SETSTATE, p, 0);
    }
    void on_menu(std::function<menu()> f) override { menu_ = std::move(f); }
    void command(WORD code) override
    {
        if (code != BN_CLICKED || !menu_) return;
        RECT r;
        GetWindowRect(h_, &r);
        SendMessageW(h_, BM_SETSTATE, TRUE, 0);
        show_popup_menu(GetAncestor(h_, GA_ROOT), menu_(), POINT{r.left, r.bottom}, h_);
        SendMessageW(h_, BM_SETSTATE, pushed_, 0);
    }
    SIZE measure() override
    {
        const SIZE t = text_extent(h_, window_text(h_));
        return {t.cx + px(24) + (images_ ? px(20) : 0), std::max<long>(px(23), t.cy + px(8))};
    }

private:
    std::string text_;
    bool pushed_ = false;
    std::function<menu()> menu_;
    HIMAGELIST images_ = nullptr;
};

// Flat, owner-drawn toolbar button: icon only, hover and pressed/checked
// backgrounds, tooltip with the label.
class tool_button_impl : public leaf<rap::tool_button>
{
public:
    tool_button_impl(win_context& win, HWND parent, const spec::tool_button& s)
        : leaf(win)
        , icon_(s.icon)
        , toggle_(s.toggle)
        , checked_(s.checked)
    {
        adopt(create_control(win, parent, L"BUTTON", wide(s.label), BS_OWNERDRAW));
        set_tooltip_text(h_, s.label);
        SetWindowSubclass(h_, hover_proc, 1, reinterpret_cast<DWORD_PTR>(this));
        want_icon(icon_, 16, win_.dpi());  // as draw_item paints it
    }
    void set_icon(const icon& icon) override
    {
        icon_ = icon;
        want_icon(icon_, 16, win_.dpi());
        InvalidateRect(h_, nullptr, TRUE);
    }
    bool checked() const override { return toggle_ && checked_; }
    void set_checked(bool c) override
    {
        if (!toggle_) return;
        checked_ = c;
        InvalidateRect(h_, nullptr, TRUE);
    }
    void on_click(std::function<void()> f) override { click_ = std::move(f); }
    void on_toggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }
    void command(WORD code) override
    {
        if (code != BN_CLICKED) return;
        if (toggle_)
        {
            checked_ = !checked_;
            InvalidateRect(h_, nullptr, TRUE);
            if (toggled_) toggled_(checked_);
        }
        if (click_) click_();
    }
    bool draw_item(DRAWITEMSTRUCT* d) override
    {
        RECT r = d->rcItem;
        FillRect(d->hDC, &r, theme_brush(surface_of(h_)));
        const bool pressed = d->itemState & ODS_SELECTED;
        const bool disabled = d->itemState & ODS_DISABLED;
        if (pressed || checked_ || hot_)
        {
            const color base = theme_color_of(theme_color::highlight);
            const float a = pressed ? 0.35f : checked_ ? 0.25f : 0.12f;
            const color face = theme_color_of(surface_of(h_));
            auto mix = [a](uint8_t x, uint8_t y) { return uint8_t(x * (1 - a) + y * a); };
            HBRUSH b = CreateSolidBrush(RGB(mix(face.r, base.r), mix(face.g, base.g), mix(face.b, base.b)));
            RECT in = r;
            InflateRect(&in, -px(1), -px(1));
            FillRect(d->hDC, &in, b);
            DeleteObject(b);
        }
        const int icon = px(16);
        RECT ir{r.left + (r.right - r.left - icon) / 2, r.top + (r.bottom - r.top - icon) / 2, 0, 0};
        ir.right = ir.left + icon;
        ir.bottom = ir.top + icon;
        draw_icon_on_dc(d->hDC, ir, icon_, win_.scale(),
                     disabled ? theme_color_of(theme_color::placeholder_text) : theme_color_of(surface_text_of(h_)));
        return true;
    }
    SIZE measure() override { return {px(28), px(28)}; }

private:
    static LRESULT CALLBACK hover_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<tool_button_impl*>(data);
        if (msg == WM_MOUSEMOVE && !self->hot_)
        {
            self->hot_ = true;
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        else if (msg == WM_MOUSELEAVE)
        {
            self->hot_ = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    icon icon_;
    bool toggle_, checked_;
    bool hot_ = false;
    std::function<void()> click_;
    std::function<void(bool)> toggled_;
};

// -- toolbar ------------------------------------------------------------------

// A run of a toolbar's buttons and separators, as one native toolbar control
// (one window, not one per button; ~1 ms apiece to make). Other items in a
// toolbar (a slider, a box of them) sit between runs, placed as in a box.
class tool_strip;

// An entry in a strip. Its visibility is its item's, as for any control,
// and the strip applies it when laid out.
class strip_entry : public item
{
public:
    SIZE measure() override { return {0, 0}; }
    void place(RECT) override {}
    void show_all(bool) override {}
};

class strip_button : public rap::tool_button, public strip_entry
{
public:
    strip_button(tool_strip& strip, int id, const spec::tool_button& s)
        : strip_(strip)
        , id_(id)
        , icon_(s.icon)
        , toggle_(s.toggle)
        , checked_(s.toggle && s.checked)
    {
    }
    void set_enabled(bool e) override;
    void set_visible(bool v) override;
    void set_tooltip(const std::string& t) override;
    void focus() override {}
    bool has_focus() const override { return false; }
    void set_icon(const icon& icon) override;
    bool checked() const override { return checked_; }
    void set_checked(bool c) override;
    void on_click(std::function<void()> f) override { click_ = std::move(f); }
    void on_toggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }

    void clicked(bool checked)
    {
        if (toggle_)
        {
            checked_ = checked;
            if (toggled_) toggled_(checked_);
        }
        if (click_) click_();
    }
    int id() const { return id_; }
    const icon& image() const { return icon_; }
    bool toggle() const { return toggle_; }

private:
    tool_strip& strip_;
    int id_;
    icon icon_;
    bool toggle_, checked_;
    std::function<void()> click_;
    std::function<void(bool)> toggled_;
};

class tool_strip : public leaf<widget>
{
public:
    tool_strip(win_context& win, HWND parent)
        : leaf(win)
    {
        // Icons only: a button's text is its tooltip (asked for, below).
        adopt(create_control(win, parent, TOOLBARCLASSNAMEW, L"",
                             TBSTYLE_FLAT | TBSTYLE_TRANSPARENT | TBSTYLE_TOOLTIPS | CCS_NORESIZE | CCS_NOPARENTALIGN |
                                 CCS_NODIVIDER));
        SendMessageW(h_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        SendMessageW(h_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);
        // A DPI change refonts every control: the icons follow.
        SetWindowSubclass(
            h_,
            [](HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) -> LRESULT
            {
                if (msg == WM_SETFONT) reinterpret_cast<tool_strip*>(data)->rescale();
                return DefSubclassProc(h, msg, wp, lp);
            },
            1, reinterpret_cast<DWORD_PTR>(this));
    }
    ~tool_strip() override
    {
        for (HIMAGELIST l : images_)
            if (l) ImageList_Destroy(l);
    }

    void add_separator(strip_entry* e) { entries_.push_back({e, nullptr, L""}); }
    strip_button* add_button(const spec::tool_button& s)
    {
        auto* b = new strip_button(*this, int(entries_.size()) + 1, s);
        entries_.push_back({b, b, wide(s.label)});
        return b;
    }
    // Once its entries are in (their items are owned by the window).
    void finish()
    {
        std::vector<TBBUTTON> buttons;
        int image = 0;
        for (entry& e : entries_)
        {
            TBBUTTON b{};
            if (e.button)
            {
                b.iBitmap = image++;
                b.idCommand = e.button->id();
                b.fsState = TBSTATE_ENABLED | (e.button->checked() ? TBSTATE_CHECKED : 0);
                b.fsStyle = BTNS_BUTTON | (e.button->toggle() ? BTNS_CHECK : 0);
                b.iString = -1;
            }
            else
                b.fsStyle = BTNS_SEP;
            buttons.push_back(b);
        }
        rescale();
        SendMessageW(h_, TB_ADDBUTTONSW, WPARAM(buttons.size()), reinterpret_cast<LPARAM>(buttons.data()));
        size_buttons();
        shown_.assign(entries_.size(), true);
    }

    void set_enabled(int id, bool e) { SendMessageW(h_, TB_ENABLEBUTTON, WPARAM(id), MAKELPARAM(e, 0)); }
    void set_checked(int id, bool c) { SendMessageW(h_, TB_CHECKBUTTON, WPARAM(id), MAKELPARAM(c, 0)); }
    void set_text(int id, const std::string& text) { entries_[size_t(id) - 1].text = wide(text); }
    void set_icon(int id)
    {
        int image = 0;
        for (size_t i = 0; i + 1 < size_t(id); ++i) image += entries_[i].button != nullptr;
        if (!images_[0]) return;
        for (int state = 0; state < 2; ++state)
            if (HBITMAP bmp = render_icon(entries_[size_t(id) - 1].button->image(), icon_px_, win_.scale(), ink(state)))
            {
                ImageList_Replace(images_[state], image, bmp, nullptr);
                DeleteObject(bmp);
            }
        InvalidateRect(h_, nullptr, FALSE);
    }
    void relayout() { win_.relayout(); }

    LRESULT notify(NMHDR* n, bool& handled) override
    {
        if (n->code == NM_CUSTOMDRAW && dark_mode())
        {
            handled = true;
            return dark_draw(reinterpret_cast<NMTBCUSTOMDRAW*>(n));
        }
        // Light, it draws its background in the style's (the window colour)
        // whatever is behind it, double buffered or printed: on a base
        // surface that's filled in the surface's colour first, the buttons'
        // looks (hot, pressed, checked, disabled) left to it.
        if (n->code == NM_CUSTOMDRAW && surface_of(h_) == theme_color::base)
        {
            handled = true;
            auto* cd = reinterpret_cast<NMTBCUSTOMDRAW*>(n);
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
                FillRect(cd->nmcd.hdc, &cd->nmcd.rc, theme_brush(theme_color::base));
            return CDRF_DODEFAULT;
        }
        handled = n->code == TBN_GETINFOTIPW;
        if (!handled) return 0;
        auto* tip = reinterpret_cast<NMTBGETINFOTIPW*>(n);
        if (tip->iItem >= 1 && size_t(tip->iItem) <= entries_.size() && tip->cchTextMax > 0)
        {
            const std::wstring& text = entries_[size_t(tip->iItem) - 1].text;
            const size_t len = std::min(text.size(), size_t(tip->cchTextMax) - 1);
            text.copy(tip->pszText, len);
            tip->pszText[len] = 0;
        }
        return 0;
    }
    // The toolbar has no dark style: its background and its buttons' hover,
    // pressed and checked states in the theme's colours, the icons (drawn
    // in the text colour) left to it.
    LRESULT dark_draw(NMTBCUSTOMDRAW* cd)
    {
        switch (cd->nmcd.dwDrawStage)
        {
        case CDDS_PREPAINT:
            FillRect(cd->nmcd.hdc, &cd->nmcd.rc, theme_brush(surface_of(cd->nmcd.hdr.hwndFrom)));
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
        {
            const UINT st = cd->nmcd.uItemState;
            if (st & (CDIS_SELECTED | CDIS_CHECKED | CDIS_HOT))
                FillRect(cd->nmcd.hdc, &cd->nmcd.rc,
                         theme_brush(st & CDIS_SELECTED  ? theme_color::pressed
                                     : st & CDIS_CHECKED ? theme_color::highlight_inactive
                                                         : theme_color::hover));
            return TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES | TBCDRF_NOOFFSET | TBCDRF_NOMARK |
                   TBCDRF_NOETCHEDEFFECT;
        }
        }
        return CDRF_DODEFAULT;
    }
    // New colours: the icons again in the new text colour.
    void theme_changed() override
    {
        icon_px_ = 0;
        rescale();
        InvalidateRect(h_, nullptr, TRUE);
    }
    void command(WORD, WORD id) override
    {
        if (id < 1 || id > entries_.size() || !entries_[id - 1u].button) return;
        const bool checked = SendMessageW(h_, TB_ISBUTTONCHECKED, id, 0) != 0;
        entries_[id - 1u].button->clicked(checked);
    }

    SIZE measure() override
    {
        sync();
        SIZE s{0, 0};
        SendMessageW(h_, TB_GETMAXSIZE, 0, reinterpret_cast<LPARAM>(&s));
        return {s.cx, std::max<long>(s.cy, button_px())};
    }
    void place(RECT r) override
    {
        sync();
        leaf::place(r);
    }
    void show_all(bool shown) override
    {
        bool any = false;
        for (const entry& e : entries_) any |= e.item->visible;
        show_control(h_, shown && visible && any);
    }

private:
    struct entry
    {
        strip_entry* item;
        strip_button* button;  // null for a separator
        std::wstring text;
    };

    int button_px() const { return px(28); }

    // Hides the entries whose items are hidden.
    void sync()
    {
        for (size_t i = 0; i < entries_.size(); ++i)
            if (shown_[i] != entries_[i].item->visible)
            {
                shown_[i] = entries_[i].item->visible;
                // A separator's id is 0, so it goes by index.
                TBBUTTONINFOW info{sizeof info, TBIF_BYINDEX | TBIF_STATE};
                SendMessageW(h_, TB_GETBUTTONINFOW, i, reinterpret_cast<LPARAM>(&info));
                info.fsState = shown_[i] ? info.fsState & ~TBSTATE_HIDDEN : info.fsState | TBSTATE_HIDDEN;
                SendMessageW(h_, TB_SETBUTTONINFOW, i, reinterpret_cast<LPARAM>(&info));
            }
    }

    // Enabled, then disabled: the toolbar doesn't grey an icon with alpha.
    static color ink(int state)
    {
        return theme_color_of(state ? theme_color::placeholder_text : theme_color::window_text);
    }

    // The icons and buttons at the window's scale.
    void rescale()
    {
        const int size = px(16);
        if (images_[0] && size == icon_px_) return;
        icon_px_ = size;
        std::vector<const icon*> icons;
        for (const entry& e : entries_)
            if (e.button) icons.push_back(&e.button->image());
        for (int state = 0; state < 2; ++state)
        {
            HIMAGELIST old = images_[state];
            images_[state] = ImageList_Create(size, size, ILC_COLOR32, int(icons.size()), 0);
            // All in one bitmap, drawn at once: a render target apiece cost ~0.5 ms each.
            if (HBITMAP bmp = render_icons(icons, size, win_.scale(), ink(state)))
            {
                ImageList_Add(images_[state], bmp, nullptr);
                DeleteObject(bmp);
            }
            SendMessageW(h_, state ? TB_SETDISABLEDIMAGELIST : TB_SETIMAGELIST, 0,
                         reinterpret_cast<LPARAM>(images_[state]));
            if (old) ImageList_Destroy(old);
        }
        size_buttons();
    }
    // Square buttons, sized once they're in (adding them resizes them).
    void size_buttons()
    {
        // The padding is what the icon leaves of a button.
        const int pad = button_px() - px(16);
        SendMessageW(h_, TB_SETPADDING, 0, MAKELPARAM(pad, pad));
        SendMessageW(h_, TB_SETBUTTONSIZE, 0, MAKELPARAM(button_px(), button_px()));
    }

    std::vector<entry> entries_;
    std::vector<bool> shown_;
    HIMAGELIST images_[2] = {};
    int icon_px_ = 0;
};

void strip_button::set_enabled(bool e) { strip_.set_enabled(id_, e); }
void strip_button::set_visible(bool v)
{
    if (visible == v) return;
    visible = v;
    strip_.relayout();
}
void strip_button::set_tooltip(const std::string& t) { strip_.set_text(id_, t); }
void strip_button::set_icon(const icon& icon)
{
    icon_ = icon;
    strip_.set_icon(id_);
}
void strip_button::set_checked(bool c)
{
    if (!toggle_) return;
    checked_ = c;
    strip_.set_checked(id_, c);
}

// -- drag and drop ------------------------------------------------------------

// A COM object of our own with one interface, made with one reference (for
// com<>::put) and gone with its last.
template <class I>
class com_object : public I
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (id != IID_IUnknown && id != __uuidof(I))
        {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        *out = static_cast<I*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --refs_;
        if (!n) delete this;
        return n;
    }

protected:
    virtual ~com_object() = default;

private:
    ULONG refs_ = 1;
};

// The clipboard format a MIME type travels as between programs: text as
// Windows' Unicode text, any other type registered under its own name.
UINT clipboard_format(const std::string& mime)
{
    return mime == "text/plain" ? CF_UNICODETEXT : RegisterClipboardFormatW(wide(mime).c_str());
}
HGLOBAL global_of(const std::string& bytes)
{
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (void* p = g ? GlobalLock(g) : nullptr)
    {
        std::memcpy(p, bytes.data(), bytes.size());
        GlobalUnlock(g);
    }
    return g;
}
// A drag's data in `format` (Unicode text as UTF-8), or nullopt.
std::optional<std::string> data_of(IDataObject* data, UINT format)
{
    FORMATETC f{CLIPFORMAT(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM m{};
    if (FAILED(data->GetData(&f, &m))) return std::nullopt;
    std::optional<std::string> out;
    if (m.tymed == TYMED_HGLOBAL)
    {
        const size_t n = GlobalSize(m.hGlobal);
        if (!n)
            out.emplace();
        else if (const void* p = GlobalLock(m.hGlobal))
        {
            if (format == CF_UNICODETEXT)
            {
                const auto* w = static_cast<const wchar_t*>(p);
                out = narrow(w, int(wcsnlen(w, n / sizeof(wchar_t))));
            }
            else
                out.emplace(static_cast<const char*>(p), n);
            GlobalUnlock(m.hGlobal);
        }
    }
    ReleaseStgMedium(&m);
    return out;
}

// What a drag carries: a data_offer's formats, best first, each handed
// over in memory of its own when asked.
class offer_data final : public com_object<IDataObject>
{
public:
    explicit offer_data(const data_offer& offer)
    {
        for (const auto& [type, bytes] : offer.formats)
        {
            const UINT format = clipboard_format(type);
            if (!format || find(format)) continue;  // the first of a format wins
            if (format != CF_UNICODETEXT)
            {
                formats_.emplace_back(format, bytes);
                continue;
            }
            const std::wstring w = wide(bytes);
            formats_.emplace_back(format, std::string(reinterpret_cast<const char*>(w.c_str()),
                                                      (w.size() + 1) * sizeof(wchar_t)));
        }
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override
    {
        const std::string* bytes = find(*f);
        if (!bytes) return DV_E_FORMATETC;
        *m = {};
        m->tymed = TYMED_HGLOBAL;
        m->hGlobal = global_of(*bytes);
        return m->hGlobal ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override { return find(*f) ? S_OK : DV_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
    {
        out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override
    {
        if (direction != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> all;
        for (const auto& f : formats_) all.push_back({CLIPFORMAT(f.first), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        return SHCreateStdEnumFmtEtc(UINT(all.size()), all.data(), out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    const std::string* find(UINT format) const
    {
        for (const auto& f : formats_)
            if (f.first == format) return &f.second;
        return nullptr;
    }
    const std::string* find(const FORMATETC& f) const
    {
        return f.dwAspect == DVASPECT_CONTENT && (f.tymed & TYMED_HGLOBAL) ? find(f.cfFormat) : nullptr;
    }

    std::vector<std::pair<UINT, std::string>> formats_;
};

// A drag's end: a drop when the button comes up, none on Esc.
// A list's own rows, dragged out and back over it, that would move there:
// the drag only ever offers a copy (anything outside the list, Explorer
// with files say, must never be offered a move), so the plain arrow is
// shown here instead of the copy cursor.
bool g_rows_would_move = false;

class drop_source final : public com_object<IDropSource>
{
public:
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override
    {
        if (escape) return DRAGDROP_S_CANCEL;
        return keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override
    {
        if (!g_rows_would_move || !(effect & DROPEFFECT_COPY)) return DRAGDROP_S_USEDEFAULTCURSORS;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return S_OK;
    }
};

// -- list ---------------------------------------------------------------------

class list_impl;
list_impl* g_dragging = nullptr;  // the list with a row drag in progress, for Esc
list_impl* g_drag_out = nullptr;  // the list whose rows a system drag carries

// A list's end of system drags over it: list_impl does the work, while it's
// there (`list` goes with it; the system may hold this a little longer).
class list_drop_target final : public com_object<IDropTarget>
{
public:
    explicit list_drop_target(list_impl* l)
        : list(l)
    {
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override;

    list_impl* list;
};

// A virtual report-style list_view: the model is asked for visible cells only.
class list_impl : public leaf<rap::list>, public list_observer
{
public:
    list_impl(win_context& win, HWND parent, const spec::list& s)
        : leaf(win)
        , columns_(s.columns)
    {
        DWORD style = LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP;
        if (s.selection == selection_mode::single || s.selection == selection_mode::none) style |= LVS_SINGLESEL;
        if (!s.headers) style |= LVS_NOCOLUMNHEADER;
        adopt(create_control(win, parent, WC_LISTVIEWW, L"", style, s.frame ? WS_EX_CLIENTEDGE : 0));
        ListView_SetExtendedListViewStyle(h_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
        // LVS_EX_DOUBLEBUFFER covers the rows, not the header: it flashed
        // whenever the list was resized.
        if (HWND header = ListView_GetHeader(h_)) paint_buffered(header, buffer_bg::own);
        visible_.assign(columns_.size(), true);
        for (size_t i = 0; i < columns_.size(); ++i)
        {
            const column& c = columns_[i];
            std::wstring title = wide(c.title);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            col.pszText = title.data();
            col.fmt = c.align == align::end ? LVCFMT_RIGHT : c.align == align::center ? LVCFMT_CENTER : LVCFMT_LEFT;
            int w = extent_px(win.hwnd(), c.width, false);
            col.cx = w > 0 ? w : px(100);
            widths_.push_back(col.cx);
            restored_.push_back(false);
            ListView_InsertColumn(h_, int(i), &col);
        }
        set_model(s.model);
        soft_ = s.soft_selection;
        checks_ = s.checkboxes;
        if (checks_) SetWindowSubclass(h_, check_proc, 9, reinterpret_cast<DWORD_PTR>(this));
        if (s.auto_scroll) SetWindowSubclass(h_, auto_scroll_proc, 7, reinterpret_cast<DWORD_PTR>(this));
        if (soft_) SetWindowSubclass(h_, hot_proc, 3, reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(h_, selection_proc, 4, reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(h_, size_proc, 8, reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(h_, tooltip_proc, 10, reinterpret_cast<DWORD_PTR>(this));
        if (s.file_view && windows_build() >= 22000)
        {
            file_view_ = !columns_.empty();
            // The header's first item shows image 0 of its image list: a
            // blank as wide as the title's inset, which moves its text in.
            if (file_view_)
            {
                LVCOLUMNW col{};
                col.mask = LVCF_FMT;
                ListView_GetColumn(h_, 0, &col);
                col.mask = LVCF_FMT | LVCF_IMAGE;
                col.fmt |= LVCFMT_IMAGE;
                col.iImage = 0;
                ListView_SetColumn(h_, 0, &col);
            }
            // A new DPI comes with a new font: the row height and the inset
            // follow it. A new theme can give the header its own margins back.
            SetWindowSubclass(
                h_,
                [](HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) -> LRESULT
                {
                    const LRESULT result = DefSubclassProc(h, msg, wp, lp);
                    auto* self = reinterpret_cast<list_impl*>(data);
                    if (msg == WM_SETFONT)
                        self->apply_file_view_rows();
                    else if (msg == WM_THEMECHANGED || msg == WM_SETTINGCHANGE)
                        self->apply_header_inset();
                    return result;
                },
                11, reinterpret_cast<DWORD_PTR>(this));
            apply_file_view_rows();
        }
    }
    // Windows 11 Explorer's row spacing at 96 DPI. A report view takes its row height
    // from its image list, plus a pixel. Its width is the image slot at the
    // start of column 0, which is the first column's inset: the rows paint
    // their own, so the slot is otherwise empty, and the list_view's own
    // measures of the column (its label rect, its auto-size) count it.
    void apply_file_view_rows()
    {
        HIMAGELIST rows = g_comctl.create(std::max(first_inset(), 1), px(28) - 1, ILC_COLOR32, 1, 0);
        if (!rows) return;  // the spacing it has is better than none
        if (HIMAGELIST old = ListView_SetImageList(h_, rows, LVSIL_SMALL)) g_comctl.destroy(old);
        // The list_view has just given the header that list too, as tall as a
        // row: the header gets its own instead, one blank as wide as its inset.
        apply_header_inset();
    }
    // Explorer's details view starts its first column's content, and the
    // header's title over it, further in than a list_view does. Measured on
    // Windows 11 at 96 DPI, from the list's left edge: the icon at 19 px
    // (a list_view's, and the rows', at 4), the title's text at 31 (a
    // list_view's at 7), between the icon and the name, not over either.
    static constexpr float file_view_first_inset = 15;
    static constexpr float file_view_title_inset = 24;
    int first_inset() const { return file_view_ ? px(file_view_first_inset) : 0; }
    int title_inset() const { return file_view_ ? px(file_view_title_inset) : 0; }
    // The header puts a bitmap margin either side of an item's image, then
    // its text margin and the text: with no bitmap margin the text moves by
    // the image's width alone, at any DPI and font.
    void apply_header_inset()
    {
        if (!file_view_) return;
        HWND header = ListView_GetHeader(h_);
        if (!header) return;
        const int w = std::max(title_inset(), 1);
        if (!header_images_ || header_images_w_ != w)
            if (HIMAGELIST fresh = blank_image_list(w))
            {
                Header_SetImageList(header, fresh);
                if (header_images_) g_comctl.destroy(header_images_);
                header_images_ = fresh;
                header_images_w_ = w;
            }
        // The list the header has (when a new one couldn't be made: the
        // last, or none rather than the rows' row-tall one).
        if (reinterpret_cast<HIMAGELIST>(SendMessageW(header, HDM_GETIMAGELIST, 0, 0)) != header_images_)
            Header_SetImageList(header, header_images_);
        SendMessageW(header, HDM_SETBITMAPMARGIN, 0, 0);
        InvalidateRect(header, nullptr, FALSE);
    }
    // One w x 1 image, masked out: drawn, it leaves nothing. Null if any
    // step fails, with nothing left behind. Its mask is made from its colour
    // (all white, all masked): a mask bitmap handed to ImageList_Add was
    // drawn solid on the header of a process without a manifest. From the
    // comctl32 the controls are, as the button and toolbar lists are.
    static HIMAGELIST blank_image_list(int w)
    {
        HIMAGELIST list = g_comctl.create(w, 1, ILC_COLOR32 | ILC_MASK, 1, 0);
        HBITMAP colour = nullptr;
        if (HDC screen = GetDC(nullptr))
        {
            colour = CreateCompatibleBitmap(screen, w, 1);
            ReleaseDC(nullptr, screen);
        }
        bool made = false;
        if (list && colour)
            if (HDC dc = CreateCompatibleDC(nullptr))
            {
                if (const HGDIOBJ old = SelectObject(dc, colour))
                {
                    made = PatBlt(dc, 0, 0, w, 1, WHITENESS);
                    SelectObject(dc, old);
                }
                DeleteDC(dc);
            }
        made = made && g_comctl.add_masked(list, colour, RGB(255, 255, 255)) == 0;
        if (colour) DeleteObject(colour);
        if (!made && list)
        {
            g_comctl.destroy(list);
            list = nullptr;
        }
        return list;
    }
    ~list_impl() override
    {
        if (header_images_)
        {
            // The header doesn't own it: off the header first, as it may paint again.
            if (HWND header = ListView_GetHeader(h_)) Header_SetImageList(header, nullptr);
            g_comctl.destroy(header_images_);
        }
        if (anchor_) DestroyWindow(anchor_);
        if (tip_) DestroyWindow(tip_);
        if (g_dragging == this) g_dragging = nullptr;
        if (g_drag_out == this) g_drag_out = nullptr;
        if (drop_target_)
        {
            RevokeDragDrop(h_);
            drop_target_->list = nullptr;
        }
        if (model_) model_->remove_observer(this);
    }

    void set_model(list_model* m) override
    {
        if (model_) model_->remove_observer(this);
        model_ = m;
        if (model_) model_->add_observer(this);
        model_reset();
    }
    std::vector<size_t> selection() const override
    {
        std::vector<size_t> out;
        for (int i = ListView_GetNextItem(h_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(h_, i, LVNI_SELECTED))
            out.push_back(size_t(i));
        return out;
    }
    std::optional<size_t> current() const override
    {
        const int i = ListView_GetNextItem(h_, -1, LVNI_FOCUSED);
        return i < 0 ? std::nullopt : std::optional<size_t>(size_t(i));
    }
    void select(const std::vector<size_t>& rows) override
    {
        quiet_ = true;
        ListView_SetItemState(h_, -1, 0, LVIS_SELECTED);
        for (size_t r : rows) ListView_SetItemState(h_, int(r), LVIS_SELECTED, LVIS_SELECTED);
        quiet_ = false;
    }
    void set_current(size_t row) override { ListView_SetItemState(h_, int(row), LVIS_FOCUSED, LVIS_FOCUSED); }
    void scroll_to(size_t row, scroll_align align) override
    {
        if (int(row) >= ListView_GetItemCount(h_)) return;
        ListView_EnsureVisible(h_, int(row), FALSE);
        if (align == scroll_align::nearest) return;
        const int h = row_px(), shown = rows_area_px();
        if (h <= 0) return;
        const int at = align == scroll_align::start ? 0
                       : align == scroll_align::end ? shown - h
                                                    : (shown - h) / 2;
        set_row_offset(row, float(at) / win_.scale());
    }
    // In logical px below the header: the rows area.
    std::optional<float> row_offset(size_t row) const override
    {
        if (int(row) >= ListView_GetItemCount(h_)) return std::nullopt;
        RECT r{};
        if (!ListView_GetItemRect(h_, int(row), &r, LVIR_BOUNDS)) return std::nullopt;
        const int y = r.top - header_px();
        if (r.bottom <= header_px() || y >= rows_area_px()) return std::nullopt;
        return float(y) / win_.scale();
    }
    // ListView_Scroll scrolls report views by whole rows: the nearest.
    void set_row_offset(size_t row, float offset) override
    {
        if (int(row) >= ListView_GetItemCount(h_)) return;
        ListView_EnsureVisible(h_, int(row), FALSE);
        RECT r{};
        if (!ListView_GetItemRect(h_, int(row), &r, LVIR_BOUNDS)) return;
        const int by = r.top - header_px() - int(std::lround(offset * win_.scale()));
        const int h = std::max(row_px(), 1);
        const int rows = by >= 0 ? (by + h / 2) / h : -((-by + h / 2) / h);
        if (rows) ListView_Scroll(h_, 0, rows * h);
    }
    int header_px() const
    {
        RECT hr{};
        HWND header = ListView_GetHeader(h_);
        return header && IsWindowVisible(header) && GetWindowRect(header, &hr) ? hr.bottom - hr.top
                                                                               : 0;
    }
    int rows_area_px() const
    {
        RECT c{};
        GetClientRect(h_, &c);
        return c.bottom - header_px();
    }
    int row_px() const
    {
        RECT r{};
        return ListView_GetItemCount(h_) && ListView_GetItemRect(h_, 0, &r, LVIR_BOUNDS)
                   ? r.bottom - r.top
                   : 0;
    }
    void set_sort_indicator(std::optional<size_t> column, sort_order order) override
    {
        HWND header = ListView_GetHeader(h_);
        for (int i = 0; i < int(columns_.size()); ++i)
        {
            HDITEMW hd{};
            hd.mask = HDI_FORMAT;
            Header_GetItem(header, i, &hd);
            hd.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (column && size_t(i) == *column) hd.fmt |= order == sort_order::ascending ? HDF_SORTUP : HDF_SORTDOWN;
            Header_SetItem(header, i, &hd);
        }
    }
    std::vector<float> column_widths() const override
    {
        std::vector<float> out;
        for (size_t i = 0; i < columns_.size(); ++i)
            out.push_back((visible_[i] ? ListView_GetColumnWidth(h_, int(i)) : widths_[i]) / win_.scale());
        return out;
    }
    std::vector<size_t> column_order() const override
    {
        std::vector<int> order(columns_.size());
        ListView_GetColumnOrderArray(h_, int(order.size()), order.data());
        return {order.begin(), order.end()};
    }
    void set_column_order(const std::vector<size_t>& order) override
    {
        if (!is_column_order(order, columns_.size())) return;
        std::vector<int> o;
        for (size_t i : order) o.push_back(int(i));
        ListView_SetColumnOrderArray(h_, int(o.size()), o.data());
        fill_last();
        InvalidateRect(h_, nullptr, FALSE);
    }
    // A width of 0 (or less) leaves its column alone, as the other
    // backends do: a stretch column left so still gets its share of the
    // width as the list is sized, rather than 0 px. Those given a width
    // keep it (restored widths win over the fitting).
    void set_column_widths(const std::vector<float>& w) override
    {
        for (size_t i = 0; i < w.size() && i < columns_.size(); ++i)
        {
            if (!(w[i] > 0)) continue;
            widths_[i] = px(w[i]);
            restored_[i] = true;
            if (visible_[i]) ListView_SetColumnWidth(h_, int(i), widths_[i]);
        }
    }
    bool column_visible(size_t c) const override { return c < visible_.size() && visible_[c]; }
    void set_column_visible(size_t c, bool v) override
    {
        if (c >= visible_.size() || visible_[c] == v) return;
        if (!v) widths_[c] = ListView_GetColumnWidth(h_, int(c));
        visible_[c] = v;
        ListView_SetColumnWidth(h_, int(c), v ? widths_[c] : 0);
        fill_last();
    }
    void on_selection_changed(std::function<void()> f) override { sel_changed_ = std::move(f); }
    void on_activate(std::function<void(size_t)> f) override { activate_ = std::move(f); }
    void on_sort_requested(std::function<void(size_t, sort_order)> f) override { sort_ = std::move(f); }
    void on_context_menu(std::function<menu(std::optional<size_t>)> f) override { context_ = std::move(f); }
    void on_header_menu(std::function<menu(size_t)> f) override { header_menu_ = std::move(f); }
    void on_cell_context_menu(std::function<menu(size_t, size_t, point_f)> f) override { cell_menu_ = std::move(f); }
    void on_reorder(reorder_fn f) override
    {
        reorder_copy_fn moves;
        if (f)
            moves = [f = std::move(f)](const std::vector<size_t>& rows, size_t at, drop_action)
            { return f(rows, at); };
        on_reorder_or_copy(std::move(moves), 0);
    }
    void on_reorder_or_copy(reorder_copy_fn f, unsigned copy_modifiers) override
    {
        reorder_ = std::move(f);
        copy_modifiers_ = copy_modifiers;
        reorderable_ = bool(reorder_);
        accept_drops();
    }
    void set_reorderable(bool on) override { reorderable_ = on && reorder_; }
    void on_drag_out(drag_out d) override
    {
        drag_out_ = std::move(d);
        accept_drops();
    }
    void on_drop_in(drop_in d) override
    {
        drop_in_ = std::move(d);
        accept_drops();
    }
    void on_cell_drag(cell_drag d) override
    {
        if (!cell_drag_.begin) SetWindowSubclass(h_, cell_press_proc, 5, reinterpret_cast<DWORD_PTR>(this));
        cell_drag_ = std::move(d);
    }

    void model_reset() override
    {
        ListView_SetItemCountEx(h_, model_ ? int(model_->row_count()) : 0, 0);
        InvalidateRect(h_, nullptr, TRUE);
    }
    void rows_changed(size_t first, size_t count) override
    {
        ListView_RedrawItems(h_, int(first), int(first + count - 1));
    }
    void rows_inserted(size_t, size_t) override { model_reset(); }
    void rows_removed(size_t, size_t) override { model_reset(); }

    LRESULT notify(NMHDR* n, bool& handled) override
    {
        handled = true;
        switch (n->code)
        {
        case HDN_ITEMCHANGEDW:
        case HDN_ITEMCHANGEDA:
            // A column before the last resized (live while dragged).
            if (reinterpret_cast<NMHEADERW*>(n)->iItem != last_visible_column()) fill_last();
            handled = false;
            return 0;
        case HDN_BEGINTRACKW:
        case HDN_BEGINTRACKA:
        case HDN_DIVIDERDBLCLICKW:
        case HDN_DIVIDERDBLCLICKA:
            // The user sizes a column: the widths are theirs from now on.
            user_sized_ = true;
            handled = false;
            return 0;
        case HDN_ENDDRAG:
            // A header dragged to a new place: the order is applied after this
            // returns, and it can change which column fills.
            PostMessageW(h_, wm_rapport_fill_last, 0, 0);
            handled = false;
            return 0;
        case LVN_GETDISPINFOW:
        {
            auto* d = reinterpret_cast<NMLVDISPINFOW*>(n);
            if ((d->item.mask & LVIF_TEXT) && model_ && size_t(d->item.iItem) < model_->row_count())
            {
                const std::size_t col = size_t(d->item.iSubItem);
                text_ = col < columns_.size() && columns_[col].painted
                            ? std::wstring()
                            : wide(model_->text(size_t(d->item.iItem), col));
                d->item.pszText = text_.data();
            }
            return 0;
        }
        case LVN_ITEMCHANGED:
        {
            auto* v = reinterpret_cast<NMLISTVIEW*>(n);
            if (!quiet_ && (v->uChanged & LVIF_STATE) && ((v->uOldState ^ v->uNewState) & LVIS_SELECTED))
                selection_changed();
            return 0;
        }
        case LVN_ODSTATECHANGED:
            if (!quiet_) selection_changed();
            return 0;
        case LVN_ITEMACTIVATE:
        {
            auto* a = reinterpret_cast<NMITEMACTIVATE*>(n);
            if (activate_ && a->iItem >= 0) activate_(size_t(a->iItem));
            return 0;
        }
        case LVN_COLUMNCLICK:
        {
            auto* v = reinterpret_cast<NMLISTVIEW*>(n);
            if (sort_ && v->iSubItem >= 0 && columns_[size_t(v->iSubItem)].sortable)
            {
                const size_t c = size_t(v->iSubItem);
                const sort_order o = sort_column_ == int(c) && sort_order_ == sort_order::ascending ? sort_order::descending
                                                                                               : sort_order::ascending;
                sort_column_ = int(c);
                sort_order_ = o;
                sort_(c, o);
            }
            return 0;
        }
        case NM_RCLICK:
            if (n->hwndFrom == ListView_GetHeader(h_))
            {
                if (!header_menu_) return 0;
                POINT pt;
                GetCursorPos(&pt);
                POINT local = pt;
                ScreenToClient(n->hwndFrom, &local);
                HDHITTESTINFO hit{};
                hit.pt = local;
                SendMessageW(n->hwndFrom, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit));
                if (hit.iItem >= 0) show_popup_menu(GetAncestor(h_, GA_ROOT), header_menu_(size_t(hit.iItem)), pt);
                return 1;
            }
            if (auto* a = reinterpret_cast<NMITEMACTIVATE*>(n); cell_menu_ && a->iItem >= 0)
            {
                // Which cell, and where in it (a label the menu is about).
                LVHITTESTINFO hit{};
                hit.pt = a->ptAction;
                ListView_SubItemHitTest(h_, &hit);
                const int col = std::max(hit.iSubItem, 0);
                POINT pt;
                GetCursorPos(&pt);
                show_popup_menu(GetAncestor(h_, GA_ROOT),
                          cell_menu_(size_t(a->iItem), size_t(col), in_cell(a->iItem, col, a->ptAction)),
                          pt);
            }
            else if (context_)
            {
                auto* a = reinterpret_cast<NMITEMACTIVATE*>(n);
                POINT pt;
                GetCursorPos(&pt);
                show_popup_menu(GetAncestor(h_, GA_ROOT),
                          context_(a->iItem >= 0 ? std::optional<size_t>(size_t(a->iItem)) : std::nullopt), pt);
            }
            return 0;
        case NM_CUSTOMDRAW:
            return custom_draw(reinterpret_cast<NMLVCUSTOMDRAW*>(n));
        case LVN_BEGINDRAG:
            // Past the drag threshold: a press `begin` accepted drags its
            // label, any other the rows: reordered within the list, or out
            // of it (at once without on_reorder).
            if (cell_.pressed)
            {
                cell_.dragging = true;
                g_dragging = this;
                SetCapture(h_);
                SetWindowSubclass(h_, cell_drag_proc, 6, reinterpret_cast<DWORD_PTR>(this));
                return 0;
            }
            asked_out_ = false;
            if (reorderable_)
            {
                dragging_ = true;
                g_dragging = this;
                SetCapture(h_);
                SetWindowSubclass(h_, drag_proc, 2, reinterpret_cast<DWORD_PTR>(this));
            }
            else if (drag_out_.begin)
            {
                asked_out_ = true;
                start_drag_out();  // the list may be gone after
            }
            return 0;
        }
        handled = false;
        return 0;
    }

    void place(RECT r) override
    {
        leaf::place(r);
        fit_stretch();
    }
    // A stretching last column keeps the list's right edge: it takes what
    // the others leave as the list resizes (on its own WM_SIZE: placing is
    // deferred, and a scroll bar coming or going changes the client width
    // too) and as they're dragged wider.
    // The last as shown, which a dragged header order can make any of them.
    int last_visible_column() const
    {
        std::vector<int> order(columns_.size());
        if (order.empty() || !ListView_GetColumnOrderArray(h_, int(order.size()), order.data()))
            return -1;
        for (auto it = order.rbegin(); it != order.rend(); ++it)
            if (*it >= 0 && size_t(*it) < visible_.size() && visible_[size_t(*it)]) return *it;
        return -1;
    }
    int filled_column() const
    {
        const int last = last_visible_column();
        return last >= 0 && columns_[size_t(last)].stretch ? last : -1;
    }
    // The client width a list_view of client w x h will have once it's added
    // the vertical scroll bar its rows need at that height (and before it
    // has: on its way to a new size).
    long width_after(long w, long h) const
    {
        if (GetWindowLongPtrW(h_, GWL_STYLE) & WS_VSCROLL) return w;  // in the width already
        const int rows = ListView_GetItemCount(h_);
        RECT item{}, header{};
        if (rows <= 0 || !ListView_GetItemRect(h_, 0, &item, LVIR_BOUNDS)) return w;
        if (HWND hd = ListView_GetHeader(h_); hd && IsWindowVisible(hd)) GetWindowRect(hd, &header);
        const long needed = long(rows) * (item.bottom - item.top) + (header.bottom - header.top);
        return needed > h ? w - GetSystemMetricsForDpi(SM_CXVSCROLL, win_.dpi()) : w;
    }
    // `width`: the client width to fill, if not the one it has now (a
    // WM_SIZE on its way in).
    void fill_last(long width = -1)
    {
        const int last = filled_column();
        if (last < 0) return;
        RECT c;
        GetClientRect(h_, &c);
        if (width >= 0) c.right = width;
        if (c.right <= 0) return;
        long others = 0;
        for (size_t i = 0; i < columns_.size(); ++i)
            if (visible_[i] && int(i) != last) others += ListView_GetColumnWidth(h_, int(i));
        const int w = int(std::max<long>(px(40), c.right - others));
        if (ListView_GetColumnWidth(h_, last) != w) ListView_SetColumnWidth(h_, last, w);
    }
    // Stretch columns share the width the others leave whenever the list's
    // size changes, until the user sizes a column: from then on only they
    // change column widths, resizing one never resizes the others, and a
    // list wider than its window scrolls sideways. (Fitted only once, a list
    // first sized narrow, in a pane still taking its width, kept those
    // widths at any size.) `width`: the client width to fit, if not the one
    // it has now (a size on its way in).
    void fit_stretch(long width = -1)
    {
        RECT c;
        GetClientRect(h_, &c);
        if (width >= 0) c.right = width;
        if (user_sized_ || c.right <= 0) return;
        long fixed = 0;
        int stretchers = 0;
        const int filled = filled_column();  // has its share, then fill_last's rest
        // A stretch column given its width (set_column_widths) keeps it.
        auto stretches = [this](size_t i) { return columns_[i].stretch && !restored_[i]; };
        for (size_t i = 0; i < columns_.size(); ++i)
        {
            if (!visible_[i]) continue;
            if (stretches(i))
                ++stretchers;
            else
                fixed += ListView_GetColumnWidth(h_, int(i));
        }
        if (!stretchers) return;
        const long each = std::max<long>(px(40), (c.right - fixed) / stretchers);
        for (size_t i = 0; i < columns_.size(); ++i)
            if (visible_[i] && stretches(i) && int(i) != filled)
                ListView_SetColumnWidth(h_, int(i), each);
    }
    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        // A modest minimum, not the columns' total: that grows with every
        // column the user widens and, restored, would widen the window.
        return {px(200), font_metrics(win_.dpi()).cy * 8};
    }

private:
    // The list paints all its rows itself, in one Direct2D pass per paint:
    // a software DC target bound once to the area being repainted (binding
    // one per cell cost ~0.5 ms apiece, a hardware one ~2.5 ms for the GPU
    // readback). The list_view keeps scrolling, selection, the header,
    // keyboard and accessibility; its own item drawing is skipped. Rows look
    // the same with or without focus.
    LRESULT custom_draw(NMLVCUSTOMDRAW* cd)
    {
        if (cd->nmcd.dwDrawStage != CDDS_PREPAINT || !model_) return CDRF_DODEFAULT;
        paint_rows(cd->nmcd.hdc);
        return CDRF_SKIPDEFAULT;
    }

    struct row_colors
    {
        color background;
        std::optional<color> text;  // replaces theme_color::text (solid selection)
    };
    // Soft selection is a tint of the accent with normal text
    // (palette::soft_selection: Windows' default #0078D7 becomes
    // Explorer's selected #CCE8FF and hovered #E5F3FF); solid selection
    // the accent with white or black text.
    row_colors colors_for_row(bool selected, bool hot) const
    {
        const color base = theme_color_of(theme_color::base);
        if (!selected && (!hot || !soft_)) return {base, std::nullopt};
        if (!soft_)
            return {theme_color_of(theme_color::highlight),
                    theme_color_of(theme_color::highlight_text)};
        const palette::soft_colors soft =
            palette::soft_selection(base, theme_color_of(theme_color::accent));
        return {!selected ? soft.hover : hot ? soft.hover_selected : soft.selected, std::nullopt};
    }

    IDWriteTextFormat* text_format(const text_style& st, align align)
    {
        const int key = (st.bold ? 1 : 0) | (st.italic ? 2 : 0) | (st.monospace ? 4 : 0) | (int(align) << 3);
        com<IDWriteTextFormat>& f = formats_[key];
        if (!f)
        {
            dwrite_factory()->CreateTextFormat(st.monospace ? L"Consolas" : ui_font_family().c_str(), nullptr,
                                              st.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                              st.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, ui_font_size_dip(), L"", f.put());
            if (!f) return nullptr;
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetTextAlignment(align == align::end      ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                : align == align::center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                         : DWRITE_TEXT_ALIGNMENT_LEADING);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            com<IDWriteInlineObject> ellipsis;
            dwrite_factory()->CreateEllipsisTrimmingSign(f.get(), ellipsis.put());
            f->SetTrimming(&trim, ellipsis.get());
        }
        return f.get();
    }

    // The shell's small icon for a kind of file (Explorer's), as a bitmap
    // for the row target: per extension ("/" for folders), made once and
    // dropped with the target. Shell icons are straight-alpha BGRA, so WIC
    // converts them to premultiplied first.
    // A row's checkbox: the theme's own, drawn once per state into a bitmap
    // (or the classic one without a theme). `size` and `gap` in device px.
    int check_size() const { return px(13); }
    int check_lead() const { return checks_ ? px(4) + check_size() + px(3) : 0; }
    ID2D1Bitmap* check_bitmap(ID2D1RenderTarget* rt, check_state state)
    {
        com<ID2D1Bitmap>& out = check_bitmaps_[size_t(state)];
        if (out) return out.get();
        const int n = check_size();
        BITMAPINFO bi{};
        bi.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER),
                        .biWidth = n,
                        .biHeight = -n,
                        .biPlanes = 1,
                        .biBitCount = 32,
                        .biCompression = BI_RGB};
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp)
        {
            DeleteDC(dc);
            return nullptr;
        }
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT r{0, 0, n, n};
        auto* px32 = static_cast<uint32_t*>(bits);
        if (HTHEME theme = OpenThemeData(h_, L"BUTTON"))
        {
            const int part_state = state == check_state::checked         ? CBS_CHECKEDNORMAL
                                   : state == check_state::indeterminate ? CBS_MIXEDNORMAL
                                                                         : CBS_UNCHECKEDNORMAL;
            DrawThemeBackground(theme, dc, BP_CHECKBOX, part_state, &r, nullptr);
            CloseThemeData(theme);
        }
        else
        {
            DrawFrameControl(dc, &r, DFC_BUTTON,
                             DFCS_BUTTONCHECK |
                                 (state != check_state::unchecked ? DFCS_CHECKED : 0) |
                                 (state == check_state::indeterminate ? DFCS_INACTIVE : 0));
            for (int i = 0; i < n * n; ++i) px32[i] |= 0xFF000000u;  // GDI leaves alpha 0
        }
        GdiFlush();
        float dpi_x = 96, dpi_y = 96;
        rt->GetDpi(&dpi_x, &dpi_y);
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_x,
            dpi_y);
        rt->CreateBitmap(D2D1::SizeU(UINT32(n), UINT32(n)), bits, UINT32(n * 4), &props, out.put());
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
        return out.get();
    }
    // A press on a row's box toggles it (the selection stays); Space toggles
    // the focused row's.
    static LRESULT CALLBACK check_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                                       DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) && self->model_)
        {
            LVHITTESTINFO hit{};
            hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (ListView_SubItemHitTest(h, &hit) >= 0 && hit.iSubItem == 0)
            {
                RECT cell{};
                ListView_GetSubItemRect(h, hit.iItem, 0, LVIR_BOUNDS, &cell);
                // Column 0's LVIR_BOUNDS is the whole row: its own width is the column's.
                cell.right = cell.left + ListView_GetColumnWidth(h, 0);
                const int left = cell.left + self->first_inset() + self->px(4);
                if (hit.pt.x >= left && hit.pt.x < left + self->check_size() &&
                    hit.pt.x < cell.right)
                {
                    self->model_->toggle_checked(size_t(hit.iItem));
                    return 0;
                }
            }
        }
        if (msg == WM_KEYDOWN && wp == VK_SPACE && self->model_ && GetKeyState(VK_CONTROL) >= 0 &&
            GetKeyState(VK_MENU) >= 0)
        {
            const int focused = ListView_GetNextItem(h, -1, LVNI_FOCUSED);
            if (focused >= 0) self->model_->toggle_checked(size_t(focused));
            return 0;
        }
        if (msg == WM_CHAR && wp == L' ') return 0;  // no type-ahead or beep for it
        return DefSubclassProc(h, msg, wp, lp);
    }

    ID2D1Bitmap* file_icon(ID2D1RenderTarget* rt, const std::string& name)
    {
        const bool folder = !name.empty() && name.back() == '/';
        const size_t slash = name.find_last_of("/\\", folder ? name.size() - 2 : std::string::npos);
        const std::string base = name.substr(slash == std::string::npos ? 0 : slash + 1);
        const size_t dot = base.rfind('.');
        const std::string key = folder ? "/" : dot == std::string::npos ? base : base.substr(dot);
        auto it = file_icons_.find(key);
        if (it != file_icons_.end()) return it->second.get();
        com<ID2D1Bitmap>& out = file_icons_[key];
        SHFILEINFOW info{};
        const std::wstring probe = folder ? L"folder" : wide(dot == std::string::npos ? base : "file" + base.substr(dot));
        if (!SHGetFileInfoW(probe.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &info, sizeof info,
                            SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES) ||
            !info.hIcon)
            return nullptr;
        com<IWICBitmap> wic;
        com<IWICFormatConverter> pbgra;
        if (wic_factory() && SUCCEEDED(wic_factory()->CreateBitmapFromHICON(info.hIcon, wic.put())) &&
            SUCCEEDED(wic_factory()->CreateFormatConverter(pbgra.put())) &&
            SUCCEEDED(pbgra->Initialize(wic.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                        WICBitmapPaletteTypeCustom)))
        {
            // At the target's DPI the icon's pixels map 1:1 to device pixels.
            float dpi_x = 96, dpi_y = 96;
            rt->GetDpi(&dpi_x, &dpi_y);
            const D2D1_BITMAP_PROPERTIES props =
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_x, dpi_y);
            rt->CreateBitmapFromWicBitmap(pbgra.get(), &props, out.put());
        }
        DestroyIcon(info.hIcon);
        return out.get();
    }

    void paint_rows(HDC dc)
    {
        RECT clip{};
        if (GetClipBox(dc, &clip) == NULLREGION || IsRectEmpty(&clip)) return;
        // The column header is a child inside the client area, at the top:
        // painting over it (the background clear) blanked it until hovered.
        if (HWND header = ListView_GetHeader(h_); header && IsWindowVisible(header))
        {
            RECT hr{};
            GetWindowRect(header, &hr);
            MapWindowPoints(HWND_DESKTOP, h_, reinterpret_cast<POINT*>(&hr), 2);
            clip.top = std::max(clip.top, hr.bottom);
            if (clip.top >= clip.bottom) return;
        }
        // Below the last row is only background: GDI fills it, and Direct2D
        // (whose software target costs by the area it's bound to) draws the
        // rows alone.
        {
            const int count = int(model_->row_count());
            RECT last{};
            const long rows_bottom =
                count && ListView_GetItemRect(h_, count - 1, &last, LVIR_BOUNDS) ? last.bottom : clip.top;
            if (rows_bottom < clip.bottom)
            {
                const color base = theme_color_of(theme_color::base);
                HBRUSH brush = CreateSolidBrush(RGB(base.r, base.g, base.b));
                RECT rest{clip.left, std::max(clip.top, rows_bottom), clip.right, clip.bottom};
                FillRect(dc, &rest, brush);
                DeleteObject(brush);
                clip.bottom = std::max(clip.top, rows_bottom);
                if (clip.top >= clip.bottom) return;
            }
        }
        if (!row_target_)
        {
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96, 96);
            if (FAILED(d2d_factory()->CreateDCRenderTarget(&props, row_target_.put()))) return;
            // Crisp GDI-style glyphs, like the list_view's own text.
            com<IDWriteRenderingParams> def, params;
            dwrite_factory()->CreateRenderingParams(def.put());
            if (def &&
                SUCCEEDED(dwrite_factory()->CreateCustomRenderingParams(
                    def->GetGamma(), def->GetEnhancedContrast(), def->GetClearTypeLevel(), def->GetPixelGeometry(),
                    DWRITE_RENDERING_MODE_GDI_CLASSIC, params.put())))
                row_target_->SetTextRenderingParams(params.get());
            row_target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        }
        ID2D1DCRenderTarget* rt = row_target_.get();
        if (FAILED(rt->BindDC(dc, &clip))) return;
        const float s = win_.scale();
        auto dip = [s](long v) { return v / s; };

        // Column spans in client px: the header's item rects (in display
        // order, hidden ones zero-wide) shifted by the horizontal scroll.
        struct span
        {
            size_t col;
            long left, right;
        };
        std::vector<span> spans;
        HWND header = ListView_GetHeader(h_);
        const size_t count = model_->row_count();
        RECT first{};
        long scroll_x = 0;
        if (count && ListView_GetItemRect(h_, ListView_GetTopIndex(h_), &first, LVIR_BOUNDS)) scroll_x = first.left;
        for (size_t c = 0; c < columns_.size(); ++c)
        {
            RECT hr{};
            if (!visible_[c] || !Header_GetItemRect(header, int(c), &hr) || hr.right <= hr.left) continue;
            spans.push_back({c, hr.left + scroll_x, hr.right + scroll_x});
        }

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        const color base = theme_color_of(theme_color::base);
        rt->Clear(D2D1::ColorF(base.r / 255.f, base.g / 255.f, base.b / 255.f));
        {
            d2d_painter painter(rt, s);  // sets the target's DPI: coordinates below are DIPs
            const D2D1_MATRIX_3X2_F origin = D2D1::Matrix3x2F::Translation(-dip(clip.left), -dip(clip.top));
            com<ID2D1SolidColorBrush> brush;
            rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush.put());
            auto fill = [&](RECT r, color c)
            {
                brush->SetColor(D2D1::ColorF(c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f));
                rt->SetTransform(origin);
                rt->FillRectangle(D2D1::RectF(dip(r.left), dip(r.top), dip(r.right), dip(r.bottom)), brush.get());
            };

            const long inset = first_inset();
            for (int row = count ? ListView_GetTopIndex(h_) : 0; size_t(row) < count; ++row)
            {
                RECT rr{};
                if (!ListView_GetItemRect(h_, row, &rr, LVIR_BOUNDS) || rr.top >= clip.bottom) break;
                if (rr.bottom <= clip.top) continue;
                const bool selected = ListView_GetItemState(h_, row, LVIS_SELECTED) & LVIS_SELECTED;
                const row_colors colors = colors_for_row(selected, row == hot_);
                // Across the columns only, as the list_view repaints a row
                // whose state changes: filled past them (a window wider than
                // its columns), a strip stayed behind when it was deselected.
                fill(rr, colors.background);

                for (const span& sp : spans)
                {
                    if (sp.right <= clip.left || sp.left >= clip.right) continue;
                    // Column 0's content starts past the file_view inset; its background doesn't.
                    const RECT whole{sp.left, rr.top, sp.right, rr.bottom};
                    const RECT cell{sp.col == 0 ? std::min(sp.left + inset, sp.right) : sp.left, rr.top, sp.right,
                                    rr.bottom};
                    const text_style st = model_->style(size_t(row), sp.col);
                    if (st.bg && !selected) fill(whole, painter.resolve(*st.bg));
                    if (columns_[sp.col].painted)
                    {
                        d2d_painter cell_painter(rt, s);
                        cell_painter.set_gdi_classic();  // as the rows' own text
                        if (soft_)
                        {
                            cell_painter.override_color(theme_color::highlight, colors.background);
                            cell_painter.override_color(theme_color::highlight_text, theme_color_of(theme_color::text));
                        }
                        if (colors.text) cell_painter.override_color(theme_color::text, *colors.text);
                        rt->SetTransform(D2D1::Matrix3x2F::Translation(dip(cell.left), dip(cell.top)) * origin);
                        const size_f size{dip(cell.right - cell.left), dip(cell.bottom - cell.top)};
                        cell_painter.push_clip({0, 0, size.w, size.h});
                        model_->paint(size_t(row), sp.col, cell_painter, cell_info{size, selected, false});
                        cell_painter.pop_clip();
                        continue;
                    }
                    float icon_space = 0;
                    if (checks_ && sp.col == 0)
                        if (ID2D1Bitmap* bmp = check_bitmap(rt, model_->checked(size_t(row))))
                        {
                            const D2D1_SIZE_F sz = bmp->GetSize();
                            const float x = dip(cell.left + px(4)),
                                        y = dip(cell.top) +
                                            (dip(cell.bottom - cell.top) - sz.height) / 2;
                            rt->SetTransform(origin);
                            rt->DrawBitmap(bmp, D2D1::RectF(x, y, x + sz.width, y + sz.height));
                            icon_space = dip(check_lead()) - 4;
                        }
                    // The shell's icon for the file's type, or the model's
                    // drawing, before the text; its overlay over it.
                    if (const icon icon = model_->icon(size_t(row), sp.col))
                    {
                        ID2D1Bitmap* bmp = !icon.file.empty() ? file_icon(rt, icon.file) : nullptr;
                        if (bmp || icon.draw)
                        {
                            const D2D1_SIZE_F sz = bmp ? bmp->GetSize()  // DIPs at the target's DPI
                                                       : D2D1::SizeF(16, 16);
                            const float x = dip(cell.left) + 4 + icon_space,
                                        y = dip(cell.top) +
                                            (dip(cell.bottom - cell.top) - sz.height) / 2;
                            const auto draw = [&](const std::function<void(rap::painter&, rap::size_f)>& f)
                            {
                                d2d_painter p(rt, s);
                                p.set_gdi_classic();
                                rt->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * origin);
                                f(p, {sz.width, sz.height});
                            };
                            rt->SetTransform(origin);
                            if (bmp)
                                rt->DrawBitmap(bmp, D2D1::RectF(x, y, x + sz.width, y + sz.height));
                            else
                                draw(icon.draw);
                            if (icon.overlay) draw(icon.overlay);
                            icon_space += sz.width + 2;
                        }
                    }
                    const std::wstring text = wide(model_->text(size_t(row), sp.col));
                    if (text.empty()) continue;
                    IDWriteTextFormat* format = text_format(st, columns_[sp.col].align);
                    if (!format) continue;
                    const color fg = colors.text && !st.fg ? *colors.text
                                                           : painter.resolve(st.fg ? *st.fg : paint{theme_color::text});
                    brush->SetColor(D2D1::ColorF(fg.r / 255.f, fg.g / 255.f, fg.b / 255.f, fg.a / 255.f));
                    rt->SetTransform(origin);
                    const float pad = 6;  // the list_view's own cell padding, in DIPs
                    rt->DrawText(text.c_str(), UINT32(text.size()), format,
                                 D2D1::RectF(dip(cell.left) + pad + icon_space, dip(cell.top), dip(cell.right) - pad, dip(cell.bottom)),
                                 brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                                 // GDI-classic glyphs need GDI-classic advances: natural
                                 // ones put each snapped glyph off by a fraction ('throu gh').
                                 DWRITE_MEASURING_MODE_GDI_CLASSIC);
                }
            }

            // The drop line while rows are dragged (the list_view would draw
            // it, but its item painting is skipped).
            RECT mark{};
            if ((dragging_ || dnd_) && SendMessageW(h_, LVM_GETINSERTMARKRECT, 0, reinterpret_cast<LPARAM>(&mark)) &&
                !IsRectEmpty(&mark))
                fill(mark, theme_color_of(theme_color::text));
            // A cell drag: the drop row framed, the label at the pointer.
            if (cell_.dragging)
            {
                const color hl = theme_color_of(theme_color::highlight);
                brush->SetColor(D2D1::ColorF(hl.r / 255.f, hl.g / 255.f, hl.b / 255.f));
                rt->SetTransform(origin);
                RECT tr{};
                if (cell_.target && ListView_GetItemRect(h_, int(*cell_.target), &tr, LVIR_BOUNDS))
                    rt->DrawRectangle(D2D1::RectF(dip(tr.left) + 1, dip(tr.top) + 1, dip(tr.right) - 1, dip(tr.bottom) - 1),
                                      brush.get(), 2);
                if (!cell_.start.label.empty())
                {
                    rt->SetTransform(D2D1::Matrix3x2F::Translation(dip(cell_.pt.x) + 12, dip(cell_.pt.y) + 4) * origin);
                    text_style ts;
                    ts.bold = true;
                    const size_f t = painter.measure(cell_.start.label, ts);
                    painter.rect({0, 0, t.w + 12, t.h + 4}, 4, paint{theme_color::highlight}, paint{theme_color::mid});
                    ts.fg = theme_color::highlight_text;
                    painter.text({6, 2}, cell_.start.label, ts);
                }
            }
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        if (rt->EndDraw() == D2DERR_RECREATE_TARGET)
        {
            row_target_.reset();
            file_icons_.clear();  // bitmaps belong to the target
            for (auto& b : check_bitmaps_) b.reset();
        }
    }

    // A click changes the selection in steps (everything off, then the new
    // row on), and each step notified the app, which redrew its details
    // twice. Changes are reported once, after the list_view is done: posted
    // messages run before the next paint, so the details still land in the
    // same frame as the highlight.
    void selection_changed()
    {
        if (!sel_changed_ || sel_pending_) return;
        sel_pending_ = true;
        PostMessageW(h_, wm_rapport_selection, 0, 0);
    }
    static LRESULT CALLBACK selection_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        if (msg != wm_rapport_selection) return DefSubclassProc(h, msg, wp, lp);
        auto* self = reinterpret_cast<list_impl*>(data);
        self->sel_pending_ = false;
        if (self->sel_changed_) self->sel_changed_();
        return 0;
    }

    // The row under the pointer gets a light tint (soft-selection lists,
    // like Explorer's and TortoiseGit's).
    void set_hot(int row)
    {
        if (row == hot_) return;
        for (int r : {hot_, row})
        {
            RECT rr{};
            if (r >= 0 && ListView_GetItemRect(h_, r, &rr, LVIR_BOUNDS)) InvalidateRect(h_, &rr, FALSE);
        }
        hot_ = row;
    }
    static LRESULT CALLBACK hot_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        if (msg == WM_MOUSEMOVE)
        {
            LVHITTESTINFO hit{};
            hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int row = ListView_HitTest(h, &hit);
            self->set_hot(row >= 0 && (hit.flags & LVHT_ONITEM) ? row : -1);
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
        }
        else if (msg == WM_MOUSELEAVE)
            self->set_hot(-1);
        return DefSubclassProc(h, msg, wp, lp);
    }

    // Where dragged rows would land for a point in client coordinates: the
    // row they go in front of (row_count() = the end). Above the rows (over the
    // header) means the first visible row; only space below the last row
    // means the end.
    size_t drop_target(POINT pt) const
    {
        const size_t count = model_ ? model_->row_count() : 0;
        if (!count) return 0;
        const int top = ListView_GetTopIndex(h_);
        RECT first{};
        ListView_GetItemRect(h_, top, &first, LVIR_BOUNDS);
        if (pt.y < first.top) return size_t(top);
        LVINSERTMARK mark{sizeof mark, 0, -1, 0};
        SendMessageW(h_, LVM_INSERTMARKHITTEST, reinterpret_cast<WPARAM>(&pt), reinterpret_cast<LPARAM>(&mark));
        if (mark.iItem < 0) return count;
        return size_t(mark.iItem) + ((mark.dwFlags & LVIM_AFTER) ? 1 : 0);
    }
    // A drop with these keys held: a copy with copy_modifiers_.
    drop_action action_of(unsigned held) const
    {
        return copy_modifiers_ && (held & copy_modifiers_) == copy_modifiers_ ? drop_action::copy
                                                                              : drop_action::move;
    }
    // ...with the keys held now, as of the message being handled.
    drop_action action_now() const { return action_of(key_modifiers()); }
    // ...with an OLE drag's grfKeyState.
    drop_action action_of_ole(DWORD keys) const
    {
        unsigned held = 0;
        if (keys & MK_CONTROL) held |= mod_ctrl;
        if (keys & MK_SHIFT) held |= mod_shift;
        if (keys & MK_ALT) held |= mod_alt;
        return action_of(held);
    }
    // A contiguous selection dropped onto its own place moves nothing.
    bool noop(size_t target) const
    {
        const std::vector<size_t> rows = selection();
        if (rows.empty()) return true;
        const bool contiguous = rows.back() - rows.front() + 1 == rows.size();
        return contiguous && target >= rows.front() && target <= rows.back() + 1;
    }
    // The drop line in front of `target`, or none.
    void set_mark(std::optional<size_t> target)
    {
        const size_t count = model_ ? model_->row_count() : 0;
        LVINSERTMARK mark{sizeof mark, 0, -1, 0};
        if (target && count)
        {
            mark.iItem = int(std::min(*target, count - 1));
            mark.dwFlags = *target >= count ? LVIM_AFTER : 0;
        }
        SendMessageW(h_, LVM_SETINSERTMARK, 0, reinterpret_cast<LPARAM>(&mark));
    }
    // The arrow with a "+" that OLE's drag loop shows for a copy. There's
    // no stock one (IDC_*): it's ole32's own cursor group 3 (1 no-drop, 2
    // move, 3 copy, 4 link; 5 to 7 the old monochrome ones), as Windows 10
    // and 11's ole32.dll have them; the arrow if it's ever missing.
    static HCURSOR copy_cursor()
    {
        static const HCURSOR c = []
        {
            HMODULE ole = GetModuleHandleW(L"ole32.dll");
            HCURSOR found = ole ? LoadCursorW(ole, MAKEINTRESOURCEW(3)) : nullptr;
            return found ? found : LoadCursorW(nullptr, IDC_ARROW);
        }();
        return c;
    }
    void show_mark(size_t target)
    {
        const bool none = noop(target);
        SetCursor(none                                ? LoadCursorW(nullptr, IDC_NO)
                  : action_now() == drop_action::copy ? copy_cursor()
                                                      : LoadCursorW(nullptr, IDC_ARROW));
        set_mark(none ? std::nullopt : std::optional<size_t>(target));
    }
    // A step of scrolling with the pointer (client coordinates) near the
    // top or bottom edge, as anything dragged there does.
    void edge_scroll(POINT pt)
    {
        RECT c;
        GetClientRect(h_, &c);
        const int edge = px(20);
        if (pt.y < edge) ListView_Scroll(h_, 0, -px(8));
        else if (pt.y > c.bottom - edge) ListView_Scroll(h_, 0, px(8));
    }
public:
    void cancel_drag()
    {
        stop_auto_scroll();
        end_cell_drag(false);
        end_drag(false, POINT{});
    }

private:
    void end_drag(bool drop, POINT pt)
    {
        if (!dragging_) return;
        dragging_ = false;
        if (g_dragging == this) g_dragging = nullptr;
        set_mark(std::nullopt);
        RemoveWindowSubclass(h_, drag_proc, 2);
        if (GetCapture() == h_) ReleaseCapture();
        if (drop && reorder_)
        {
            const std::vector<size_t> rows = selection();
            const size_t target = drop_target(pt);
            if (!rows.empty() && !noop(target)) reorder_(rows, target, action_now());
        }
    }
    // cell_drag: the press is noted before the list_view sees it (it selects
    // as usual, then reports LVN_BEGINDRAG past the threshold).
    struct cell_drag_state
    {
        bool pressed = false, dragging = false;
        size_t from = 0;
        cell_drag_start start;
        std::optional<size_t> target;
        POINT pt{};  // pointer, client coordinates
    };
    // A client point in logical px from the top left of the cell at `row`,
    // `col`, as the model's cell callbacks take it: column 0's from where its
    // content starts, past the file_view inset, as it's painted.
    point_f in_cell(int row, int col, POINT p) const
    {
        RECT cell{};
        ListView_GetSubItemRect(h_, row, col, LVIR_BOUNDS, &cell);
        if (col == 0) cell.left += first_inset();
        const float s = win_.scale();
        return {(p.x - cell.left) / s, (p.y - cell.top) / s};
    }

    // The model's tooltip for the point under the pointer, asked as it
    // moves: a tool over the whole list whose text follows it (none: no tool).
    static LRESULT CALLBACK tooltip_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        if (msg == WM_MOUSEMOVE && self->model_)
        {
            std::string text;
            LVHITTESTINFO hit{};
            hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (ListView_SubItemHitTest(h, &hit) >= 0 && (hit.flags & LVHT_ONITEM) &&
                size_t(hit.iItem) < self->model_->row_count())
                text = self->model_->tooltip(size_t(hit.iItem), size_t(hit.iSubItem),
                                             self->in_cell(hit.iItem, hit.iSubItem, hit.pt));
            if (text != self->tip_text_) self->set_tip(std::move(text));
        }
        return DefSubclassProc(h, msg, wp, lp);
    }
    void set_tip(std::string text)
    {
        if (!tip_)
        {
            tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, h_, nullptr,
                                   instance(), nullptr);
            SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, 480);
            if (dark_mode()) theme_control(tip_);
            frame_popup(tip_);
        }
        std::wstring w = wide(text);
        TOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = GetParent(h_);
        ti.uId = reinterpret_cast<UINT_PTR>(h_);
        ti.lpszText = w.data();
        if (text.empty())
            SendMessageW(tip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        else if (tip_text_.empty())
            SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        else
            SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
        tip_text_ = std::move(text);
    }

    static LRESULT CALLBACK cell_press_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        if (msg == WM_LBUTTONDOWN && !(wp & (MK_SHIFT | MK_CONTROL)) && self->cell_drag_.begin && self->model_)
        {
            self->cell_ = {};
            LVHITTESTINFO hit{};
            hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (ListView_SubItemHitTest(h, &hit) >= 0 && (hit.flags & LVHT_ONITEM) && size_t(hit.iItem) < self->model_->row_count())
            {
                if (auto start = self->cell_drag_.begin(size_t(hit.iItem), size_t(hit.iSubItem),
                                                        self->in_cell(hit.iItem, hit.iSubItem, hit.pt)))
                    self->cell_ = {true, false, size_t(hit.iItem), std::move(*start), std::nullopt, hit.pt};
            }
        }
        else if (msg == WM_LBUTTONUP && !self->cell_.dragging)
            self->cell_ = {};
        return DefSubclassProc(h, msg, wp, lp);
    }
    static LRESULT CALLBACK cell_drag_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        switch (msg)
        {
        case WM_MOUSEMOVE:
        {
            cell_drag_state& c = self->cell_;
            c.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            LVHITTESTINFO hit{};
            hit.pt = c.pt;
            const int row = ListView_HitTest(h, &hit);
            c.target.reset();
            if (row >= 0 && (hit.flags & LVHT_ONITEM) && self->cell_drag_.can_drop &&
                self->cell_drag_.can_drop(c.from, size_t(row), c.start.data))
                c.target = size_t(row);
            SetCursor(LoadCursorW(nullptr, c.target ? IDC_HAND : IDC_NO));
            self->edge_scroll(c.pt);
            InvalidateRect(h, nullptr, FALSE);  // one paint pass: a few ms
            return 0;
        }
        case WM_LBUTTONUP: self->end_cell_drag(true); return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE: self->end_cell_drag(false); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE)
            {
                self->end_cell_drag(false);
                return 0;
            }
            break;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }
    void end_cell_drag(bool drop)
    {
        if (!cell_.dragging) return;
        cell_drag_state c = std::move(cell_);
        cell_ = {};
        if (g_dragging == this) g_dragging = nullptr;
        RemoveWindowSubclass(h_, cell_drag_proc, 6);
        if (GetCapture() == h_) ReleaseCapture();
        InvalidateRect(h_, nullptr, FALSE);
        if (drop && c.target && cell_drag_.drop) cell_drag_.drop(c.from, *c.target, c.start.data);
    }

    // Autoscroll (spec.auto_scroll): with the middle button held, a timer
    // scrolls by the pointer's offset from the anchor. The list_view scrolls
    // whole rows, so pixels gather until a row's worth.
    struct auto_scroll_state
    {
        bool on = false;
        POINT anchor{};
        int y = 0;
        double last = 0, pending = 0;  // ms; pixels not yet scrolled
    };
    static double now_ms()
    {
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
    }
    static float auto_scroll_speed(float offset)  // px/s; as gtk_common's
    {
        const float dead = 8, d = std::abs(offset) - dead;
        if (d <= 0) return 0;
        return (offset < 0 ? -1.f : 1.f) * (d * 6 + d * d * 0.05f);
    }
    void stop_auto_scroll()
    {
        if (!auto_.on) return;
        auto_ = {};
        KillTimer(h_, 0xa5c0);
        if (GetCapture() == h_) ReleaseCapture();
        if (g_dragging == this) g_dragging = nullptr;
        if (anchor_) ShowWindow(anchor_, SW_HIDE);
    }

    // The anchor floats above the list in a window of its own (layered,
    // see-through to the mouse, never active): painted into the rows, each
    // scroll's blit carried it off with them until the next repaint.
    void show_anchor(POINT at)
    {
        const float s = win_.scale();
        const int side = int(std::ceil(26 * s)) + 2;  // the ring is 11 px round, plus its stroke
        if (!anchor_)
        {
            static const ATOM cls = []
            {
                WNDCLASSEXW wc{sizeof wc};
                wc.lpfnWndProc = DefWindowProcW;
                wc.hInstance = instance();
                wc.lpszClassName = L"RapportAutoScrollAnchor";
                return RegisterClassExW(&wc);
            }();
            anchor_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                      MAKEINTATOM(cls), nullptr, WS_POPUP, 0, 0, side, side, GetAncestor(h_, GA_ROOT),
                                      nullptr, instance(), nullptr);
            if (!anchor_) return;
        }
        // Drawn premultiplied into a DIB, handed over with its alpha.
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), side, -side, 1, 32, BI_RGB};
        void* bits = nullptr;
        HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96, 96);
        com<ID2D1DCRenderTarget> rt;
        RECT all{0, 0, side, side};
        if (bits && SUCCEEDED(d2d_factory()->CreateDCRenderTarget(&props, rt.put())) &&
            SUCCEEDED(rt->BindDC(mem, &all)))
        {
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            {
                d2d_painter p(rt.get(), s);
                const float c = side / s / 2;
                p.circle({c, c}, 11, paint{theme_color::base}, paint{theme_color::mid});
                p.polygon({{c - 4, c - 3}, {c + 4, c - 3}, {c, c - 8}}, paint{theme_color::text}, std::nullopt, 1);
                p.polygon({{c - 4, c + 3}, {c + 4, c + 3}, {c, c + 8}}, paint{theme_color::text}, std::nullopt, 1);
                p.circle({c, c}, 1.5f, paint{theme_color::text}, std::nullopt);
            }
            rt->EndDraw();
            POINT pos = at;
            ClientToScreen(h_, &pos);
            pos.x -= side / 2;
            pos.y -= side / 2;
            SIZE size{side, side};
            POINT zero{};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(anchor_, screen, &pos, &size, mem, &zero, 0, &blend, ULW_ALPHA);
            ShowWindow(anchor_, SW_SHOWNOACTIVATE);
        }
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }
    static LRESULT CALLBACK size_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        // The last column fitted to the new width before the list_view sees
        // it: after, the list_view had shown a horizontal scroll bar for the
        // old widths, a flash before the column shrank. Again after, for a
        // vertical scroll bar the new size brought or took away. A list
        // narrowing (a splitter dragged across it) is fitted before the move
        // is made at all: the list_view settles its scroll bars as the move
        // lands, before WM_SIZE, and showed the horizontal one at every step.
        if (msg == WM_WINDOWPOSCHANGING)
        {
            const auto* pos = reinterpret_cast<const WINDOWPOS*>(lp);
            RECT wr, cr;
            if (!(pos->flags & SWP_NOSIZE) && GetWindowRect(h, &wr) && GetClientRect(h, &cr))
            {
                const long w = pos->cx - ((wr.right - wr.left) - cr.right);
                const long hgt = pos->cy - ((wr.bottom - wr.top) - cr.bottom);
                if (w < cr.right)
                {
                    self->fit_stretch(self->width_after(w, hgt));
                    self->fill_last(self->width_after(w, hgt));
                }
            }
        }
        if (msg == WM_SIZE)
        {
            self->fit_stretch(self->width_after(LOWORD(lp), HIWORD(lp)));
            self->fill_last(self->width_after(LOWORD(lp), HIWORD(lp)));
        }
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (msg == WM_SIZE || msg == wm_rapport_fill_last) self->fill_last();
        return r;
    }
    static LRESULT CALLBACK auto_scroll_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        auto_scroll_state& a = self->auto_;
        switch (msg)
        {
        case WM_MBUTTONDOWN:
            a = {true, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, GET_Y_LPARAM(lp), now_ms(), 0};
            SetCapture(h);
            g_dragging = self;  // Esc stops it, as it cancels drags
            SetTimer(h, 0xa5c0, 8, nullptr);
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            self->show_anchor(a.anchor);
            return 0;
        case WM_MOUSEMOVE:
            if (!a.on) break;
            a.y = GET_Y_LPARAM(lp);
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return 0;
        case WM_TIMER:
        {
            if (wp != 0xa5c0 || !a.on) break;
            const double now = now_ms();
            const float s = self->win_.scale();
            a.pending += auto_scroll_speed((a.y - a.anchor.y) / s) * s * (now - a.last) / 1000.0;
            a.last = now;
            RECT row{};
            if (ListView_GetItemRect(h, ListView_GetTopIndex(h), &row, LVIR_BOUNDS) && row.bottom > row.top)
            {
                const int row_px = row.bottom - row.top, rows = int(a.pending / row_px);
                if (rows)
                {
                    ListView_Scroll(h, 0, rows * row_px);
                    a.pending -= rows * row_px;
                }
            }
            return 0;
        }
        case WM_MBUTTONUP:
        case WM_CAPTURECHANGED:
            if (a.on) self->stop_auto_scroll();
            if (msg == WM_MBUTTONUP) return 0;
            break;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    static LRESULT CALLBACK drag_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        switch (msg)
        {
        case WM_MOUSEMOVE:
        {
            // Out of the list, the rows go on as a system drag (drag_out),
            // asked once a drag: a reorder `begin` declines stays one.
            RECT c;
            GetClientRect(h, &c);
            if (self->drag_out_.begin && !self->asked_out_ && !PtInRect(&c, pt))
            {
                self->asked_out_ = true;
                if (self->start_drag_out()) return 0;  // the list may be gone
            }
            self->show_mark(self->drop_target(pt));
            self->edge_scroll(pt);
            return 0;
        }
        case WM_LBUTTONUP: self->end_drag(true, pt); return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE: self->end_drag(false, pt); return 0;
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:  // with Alt down
        case WM_SYSKEYUP:
        {
            if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
            {
                self->end_drag(false, pt);
                return 0;
            }
            // A modifier down or up: the cursor says again whether the drop
            // would move or copy (lp is no point here).
            if (wp == VK_CONTROL || wp == VK_SHIFT || wp == VK_MENU)
            {
                POINT at;
                GetCursorPos(&at);
                ScreenToClient(h, &at);
                self->show_mark(self->drop_target(at));
                return 0;
            }
            // Alt's keys stay here mid-drag, not the window's menu's.
            if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) return 0;
            break;
        }
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    // The selected rows as drag_out offers them, through the system's drag
    // and drop, which runs until the drop. Returns whether one ran: the
    // list may be gone by then (a drop handler closing its window).
    bool start_drag_out()
    {
        std::optional<data_offer> offer = drag_out_.begin(selection());
        if (!offer || offer->formats.empty()) return false;
        end_drag(false, POINT{});  // a reorder hands over to it
        set_hot(-1);
        com<IDataObject> data;
        *data.put() = new offer_data(*offer);
        com<IDropSource> source;
        *source.put() = new drop_source;
        g_drag_out = this;
        DWORD effect = 0;
        DoDragDrop(data.get(), source.get(), DROPEFFECT_COPY, &effect);
        g_rows_would_move = false;
        if (g_drag_out == this) g_drag_out = nullptr;  // cleared already if the list went
        return true;
    }

    // System drags land through OLE (list_drop_target): drop_in's data, and
    // this list's own rows brought back after leaving it (a reorder).
    void accept_drops()
    {
        if (drop_target_ || !(drop_in_.drop || (reorder_ && drag_out_.begin))) return;
        *drop_target_.put() = new list_drop_target(this);
        RegisterDragDrop(h_, drop_target_.get());
    }
    // What a system drag over the list would drop: data of one of drop_in's
    // types, this list's own rows, or files, which are the window's (the
    // list's drop target stands in front of the window's own).
    struct dnd_state
    {
        enum class what
        {
            data,
            own_rows,
            files
        } kind;
        std::string type;
        UINT format = 0;
    };
    std::optional<dnd_state> dnd_offered(IDataObject* data) const
    {
        if (g_drag_out == this && reorderable_) return dnd_state{dnd_state::what::own_rows};
        if (drop_in_.drop)
        {
            // The first of the drag's formats drop_in takes.
            auto taken = [&](UINT format) -> std::optional<dnd_state>
            {
                for (const std::string& type : drop_in_.types)
                    if (clipboard_format(type) == format) return dnd_state{dnd_state::what::data, type, format};
                return std::nullopt;
            };
            com<IEnumFORMATETC> formats;
            if (SUCCEEDED(data->EnumFormatEtc(DATADIR_GET, formats.put())) && formats)
            {
                FORMATETC f;
                while (formats->Next(1, &f, nullptr) == S_OK)
                {
                    if (f.ptd) CoTaskMemFree(f.ptd);
                    if (f.tymed & TYMED_HGLOBAL)
                        if (auto d = taken(f.cfFormat)) return d;
                }
            }
            else
                for (const std::string& type : drop_in_.types)
                {
                    FORMATETC f{CLIPFORMAT(clipboard_format(type)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                    if (data->QueryGetData(&f) == S_OK) return dnd_state{dnd_state::what::data, type, f.cfFormat};
                }
        }
        FORMATETC files{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        if ((GetWindowLongW(GetAncestor(h_, GA_ROOT), GWL_EXSTYLE) & WS_EX_ACCEPTFILES) &&
            data->QueryGetData(&files) == S_OK)
            return dnd_state{dnd_state::what::files};
        return std::nullopt;
    }
    bool dnd_can_drop(size_t at) const
    {
        switch (dnd_->kind)
        {
        case dnd_state::what::own_rows: return !noop(at);
        case dnd_state::what::data: return !drop_in_.can_drop || drop_in_.can_drop(at, dnd_->type);
        default: return true;
        }
    }

public:
    // list_drop_target's calls (screen coordinates), returning the effect.
    // `keys`: the drag's grfKeyState. This list's own rows answer a copy
    // (all the drag offers) and say through g_rows_would_move whether
    // they'd move, so the cursor shows the "+" only for a copy.
    DWORD dnd_enter(IDataObject* data, POINT pt, DWORD keys)
    {
        dnd_ = dnd_offered(data);
        return dnd_over(pt, keys);
    }
    DWORD dnd_over(POINT pt, DWORD keys)
    {
        g_rows_would_move = false;
        if (!dnd_) return DROPEFFECT_NONE;
        if (dnd_->kind == dnd_state::what::files) return DROPEFFECT_COPY;
        ScreenToClient(h_, &pt);
        edge_scroll(pt);  // called again and again while the pointer rests
        const size_t at = drop_target(pt);
        const bool ok = dnd_can_drop(at);
        set_mark(ok ? std::optional<size_t>(at) : std::nullopt);
        if (!ok) return DROPEFFECT_NONE;
        g_rows_would_move =
            dnd_->kind == dnd_state::what::own_rows && action_of_ole(keys) == drop_action::move;
        return DROPEFFECT_COPY;
    }
    void dnd_leave()
    {
        g_rows_would_move = false;
        if (dnd_) set_mark(std::nullopt);
        dnd_.reset();
    }
    DWORD dnd_drop(IDataObject* data, POINT pt, DWORD keys)
    {
        if (!dnd_) return DROPEFFECT_NONE;
        ScreenToClient(h_, &pt);
        const size_t at = drop_target(pt);
        const bool ok = dnd_can_drop(at);
        const dnd_state d = *dnd_;
        dnd_leave();
        if (!ok) return DROPEFFECT_NONE;
        switch (d.kind)
        {
        case dnd_state::what::own_rows:
            reorder_(selection(), at, action_of_ole(keys));  // the keys, not the effect
            return DROPEFFECT_COPY;
        case dnd_state::what::files:
            // To the window, as a drop there would have gone.
            if (std::optional<std::string> bytes = data_of(data, CF_HDROP))
                if (HGLOBAL copy = global_of(*bytes))
                    if (!PostMessageW(GetAncestor(h_, GA_ROOT), WM_DROPFILES, reinterpret_cast<WPARAM>(copy), 0))
                        GlobalFree(copy);
            return DROPEFFECT_COPY;
        case dnd_state::what::data:
        {
            const std::optional<std::string> bytes = data_of(data, d.format);
            return bytes && drop_in_.drop(at, d.type, *bytes) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        }
        return DROPEFFECT_NONE;
    }

private:
    reorder_copy_fn reorder_;
    unsigned copy_modifiers_ = 0;
    bool reorderable_ = false, dragging_ = false;
    drag_out drag_out_;
    drop_in drop_in_;
    bool asked_out_ = false;  // drag_out's begin has had its say this drag
    com<list_drop_target> drop_target_;
    std::optional<dnd_state> dnd_;  // a system drag over the list
    cell_drag cell_drag_;
    cell_drag_state cell_;
    auto_scroll_state auto_;
    HWND anchor_ = nullptr;  // the autoscroll anchor's window, made on first use
    std::vector<column> columns_;
    std::vector<bool> visible_;
    std::vector<int> widths_;
    list_model* model_ = nullptr;
    HWND tip_ = nullptr;    // the cells' tooltips (tooltip_proc)
    std::string tip_text_;  // its text now; empty: no tool
    std::wstring text_;  // LVN_GETDISPINFO's buffer
    bool quiet_ = false;
    bool user_sized_ = false;  // the user has sized a column: stretch columns fit no more
    std::vector<bool> restored_;  // per column: given its width by set_column_widths
    bool sel_pending_ = false;  // a selection change is posted, not yet reported
    com<ID2D1DCRenderTarget> row_target_;
    std::map<std::string, com<ID2D1Bitmap>> file_icons_;  // per extension, for row_target_
    bool checks_ = false;                                 // a checkbox at the start of column 0
    bool file_view_ = false;                              // Windows 11 file_view: column 0's content inset
    HIMAGELIST header_images_ = nullptr;                  // its one blank, for the header's first title
    int header_images_w_ = 0;
    std::array<com<ID2D1Bitmap>, 3> check_bitmaps_;       // per check_state, for row_target_
    std::map<int, com<IDWriteTextFormat>> formats_;  // by style and alignment
    bool soft_ = false;
    int hot_ = -1;  // the row under the pointer
    int sort_column_ = -1;
    sort_order sort_order_ = sort_order::ascending;
    std::function<void()> sel_changed_;
    std::function<void(size_t)> activate_;
    std::function<void(size_t, sort_order)> sort_;
    std::function<menu(std::optional<size_t>)> context_;
    std::function<menu(size_t)> header_menu_;
    std::function<menu(size_t, size_t, point_f)> cell_menu_;
};

// The list answers a copy or nothing; `*effect` comes in as what the
// drag's source allows.
HRESULT list_drop_target::DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    *effect = list ? list->dnd_enter(data, {pt.x, pt.y}, keys) & *effect : DROPEFFECT_NONE;
    return S_OK;
}
HRESULT list_drop_target::DragOver(DWORD keys, POINTL pt, DWORD* effect)
{
    *effect = list ? list->dnd_over({pt.x, pt.y}, keys) & *effect : DROPEFFECT_NONE;
    return S_OK;
}
HRESULT list_drop_target::DragLeave()
{
    if (list) list->dnd_leave();
    return S_OK;
}
HRESULT list_drop_target::Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    if (!list || !(*effect & DROPEFFECT_COPY))
    {
        if (list) list->dnd_leave();
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    *effect = list->dnd_drop(data, {pt.x, pt.y}, keys) & *effect;
    return S_OK;
}

// -- tabs ---------------------------------------------------------------------

// The tab control has no dark style: in dark mode it's painted here, the
// tabs on the surface's colour with the current one raised and underlined
// in the accent, as Windows 11's own tabs are.
void draw_dark_tabs(HWND h, HDC dc)
{
    RECT c;
    GetClientRect(h, &c);
    const theme_color surface = surface_of(h);
    FillRect(dc, &c, theme_brush(surface));
    const UINT dpi = dpi_of(h);
    auto px = [dpi](int v) { return MulDiv(v, int(dpi), 96); };
    RECT page = c;
    traced_tab_adjust(h, FALSE, &page);
    InflateRect(&page, px(2), px(2));
    FrameRect(dc, &page, theme_brush(theme_color::border));
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    const LRESULT ui = SendMessageW(h, WM_QUERYUISTATE, 0, 0);
    const int current = TabCtrl_GetCurSel(h), n = TabCtrl_GetItemCount(h);
    POINT pointer;
    GetCursorPos(&pointer);
    ScreenToClient(h, &pointer);
    for (int i = 0; i < n; ++i)
    {
        RECT r;
        if (!TabCtrl_GetItemRect(h, i, &r)) continue;
        const bool selected = i == current, hot = PtInRect(&r, pointer);
        if (selected) InflateRect(&r, px(2), px(2));
        FillRect(dc, &r,
                 theme_brush(selected ? theme_color::card
                             : hot    ? theme_color::hover
                                      : surface));
        FrameRect(dc, &r, theme_brush(theme_color::border));
        if (selected)
        {
            RECT bar{r.left + px(1), r.bottom - px(3), r.right - px(1), r.bottom};
            FillRect(dc, &bar, theme_brush(theme_color::accent));
        }
        wchar_t text[128] = {};
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = text;
        item.cchTextMax = int(std::size(text));
        TabCtrl_GetItem(h, i, &item);
        SetTextColor(dc,
                     color_ref(theme_color_of(IsWindowEnabled(h) ? surface_text_of(h)
                                                                 : theme_color::disabled_text)));
        DWORD flags = DT_SINGLELINE | DT_VCENTER | DT_CENTER;
        if (ui & UISF_HIDEACCEL) flags |= DT_HIDEPREFIX;
        DrawTextW(dc, text, -1, &r, flags);
        if (selected && GetFocus() == h && !(ui & UISF_HIDEFOCUS))
        {
            RECT focus = r;
            InflateRect(&focus, -px(3), -px(3));
            DrawFocusRect(dc, &focus);
        }
    }
    SelectObject(dc, old);
}

// In light mode on a base surface the style's drawing is kept, on the
// surface's colour, with its page's body (the style's own lighter shade)
// filled in the surface's colour too. The body's colour is read back from
// `dc`, so it's whole only on a DC that isn't clipped (render_tabs' bitmap).
void draw_base_tabs(HWND h, HDC dc)
{
    RECT c;
    GetClientRect(h, &c);
    FillRect(dc, &c, theme_brush(theme_color::base));
    // Its own erase left out: around the tabs is the surface, filled here.
    DefSubclassProc(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    RECT body = c;
    traced_tab_adjust(h, FALSE, &body);
    if (body.right <= body.left || body.bottom <= body.top) return;
    const POINT mid{(body.left + body.right) / 2, (body.top + body.bottom) / 2};
    const COLORREF face = GetPixel(dc, mid.x, mid.y);
    if (face == CLR_INVALID || face == color_ref(theme_color_of(theme_color::base))) return;
    // The body's pixels, out to the page's edge, whatever its inset.
    HGDIOBJ old = SelectObject(dc, theme_brush(theme_color::base));
    ExtFloodFill(dc, mid.x, mid.y, face, FLOODFILLSURFACE);
    SelectObject(dc, old);
}

// Tabs rapport draws itself: dark mode's, and a base surface's.
bool own_tabs(HWND h)
{
    return dark_mode() || surface_of(h) == theme_color::base;
}
void draw_own_tabs(HWND h, HDC dc)
{
    if (dark_mode())
        draw_dark_tabs(h, dc);
    else
        draw_base_tabs(h, dc);
}

// The tabs drawn whole into a bitmap of their own, then copied to `dc`
// through whatever clip it has (a paint's update region, a partial print),
// which is left as it was. False if the bitmap couldn't be made or copied.
bool render_tabs(HWND h, HDC dc)
{
    RECT c;
    GetClientRect(h, &c);
    if (c.right <= 0 || c.bottom <= 0) return true;  // nothing to draw
    HDC mem = CreateCompatibleDC(dc);
    if (!mem) return false;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), c.right, -c.bottom, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = bitmap ? SelectObject(mem, bitmap) : nullptr;
    bool drawn = false;
    if (old && old != HGDI_ERROR)
    {
        draw_own_tabs(h, mem);
        GdiFlush();
        drawn = BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY) != FALSE;
        SelectObject(mem, old);
    }
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(mem);
    return drawn;
}

// Screen and print (a pane's off-screen copy, tab_body_brush's measure)
// alike: through render_tabs, else drawn straight onto the DC, so a failure
// still paints (a light body may keep the style's shade then), and is never
// retried.
LRESULT CALLBACK dark_tabs_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if ((msg != WM_PAINT && msg != WM_ERASEBKGND && msg != WM_PRINTCLIENT) || !own_tabs(h))
        return DefSubclassProc(h, msg, wp, lp);
    switch (msg)
    {
    case WM_ERASEBKGND: return 1;
    case WM_PRINTCLIENT:
    {
        if (!(lp & PRF_CLIENT)) return 0;
        HDC dc = reinterpret_cast<HDC>(wp);
        if (!render_tabs(h, dc)) draw_own_tabs(h, dc);
        return 0;
    }
    default:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (!render_tabs(h, dc)) draw_own_tabs(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    }
}

class tabs_impl : public leaf<rap::tabs>
{
public:
    tabs_impl(win_context& win, HWND parent, const spec::tabs& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP));
        SetWindowSubclass(h_, dark_tabs_proc, 1, 0);
        for (size_t i = 0; i < s.titles.size(); ++i)
        {
            std::wstring t = mnemonic(s.titles[i]);
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = t.data();
            TabCtrl_InsertItem(h_, int(i), &item);
        }
    }
    std::vector<item*> pages;
    // Lazy pages: their builders until built.
    std::vector<std::function<node()>> lazy;
    std::function<item*(const node&)> buildPage;

    int current() const override { return TabCtrl_GetCurSel(h_); }
    void set_current(int i) override
    {
        TabCtrl_SetCurSel(h_, i);
        ensure_built(i);
        win_.relayout_at_once();
    }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }
    LRESULT notify(NMHDR* n, bool& handled) override
    {
        handled = n->code == TCN_SELCHANGE;
        if (handled)
        {
            ensure_built(current());
            win_.relayout_at_once();
            if (changed_) changed_(current());
        }
        return 0;
    }
    void ensure_built(int i)
    {
        if (i < 0 || size_t(i) >= lazy.size() || !lazy[size_t(i)]) return;
        const auto build = std::move(lazy[size_t(i)]);
        lazy[size_t(i)] = nullptr;
        pages[size_t(i)] = buildPage(build());
        // New controls go above the tab control, as the first pages did.
        traced_set_window_pos(h_, HWND_BOTTOM, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        win_.relayout();
    }
    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        SIZE in{0, 0};
        for (size_t i = 0; i < pages.size(); ++i)
        {
            // An unbuilt page takes the area it's given; once built it has
            // a size like any other.
            if (i < lazy.size() && lazy[i]) continue;
            const SIZE s = pages[i]->natural();
            in.cx = std::max(in.cx, s.cx);
            in.cy = std::max(in.cy, s.cy);
        }
        RECT r{0, 0, in.cx + 2 * pad(), in.cy + 2 * pad()};
        traced_tab_adjust(h_, TRUE, &r);
        return {r.right - r.left, r.bottom - r.top};
    }
    void place(RECT r) override
    {
        leaf::place(r);
        RECT in{0, 0, r.right - r.left, r.bottom - r.top};
        traced_tab_adjust(h_, FALSE, &in);
        OffsetRect(&in, r.left, r.top);
        InflateRect(&in, -pad(), -pad());
        for (item* p : pages) p->place(in);
    }
    void show_all(bool shown) override
    {
        leaf::show_all(shown);
        if (shown && visible) ensure_built(current());  // the first time a lazy page is the one showing
        for (size_t i = 0; i < pages.size(); ++i) pages[i]->show_all(shown && visible && int(i) == current());
    }

private:
    int pad() const { return px(6); }
    std::function<void(int)> changed_;
};

// Tabs as a list of page names down the left, the page beside it.
class side_tabs_impl : public leaf<rap::tabs>
{
public:
    side_tabs_impl(win_context& win, HWND parent, const spec::tabs& s)
        : leaf(win)
    {
        // Owner-drawn only to centre the names in rows taller than the text
        // (a plain list box draws them at the top of each row).
        adopt(create_control(win, parent, L"LISTBOX", L"",
                            LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_TABSTOP,
                            WS_EX_CLIENTEDGE));
        for (const std::string& t : s.titles)
        {
            // Mnemonics mean nothing in a list: "_General" shows as "General".
            std::wstring title = mnemonic(t);
            title.erase(std::remove(title.begin(), title.end(), L'&'), title.end());
            titles_.push_back(title);
            SendMessageW(h_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(title.c_str()));
        }
        SendMessageW(h_, LB_SETITEMHEIGHT, 0, font_metrics(win.dpi()).cy + px(12));
        SendMessageW(h_, LB_SETCURSEL, 0, 0);
    }
    std::vector<item*> pages;
    std::vector<std::function<node()>> lazy;
    std::function<item*(const node&)> buildPage;

    int current() const override { return std::max(0, int(SendMessageW(h_, LB_GETCURSEL, 0, 0))); }
    void set_current(int i) override
    {
        SendMessageW(h_, LB_SETCURSEL, WPARAM(i), 0);
        if (i == shown_) return;
        shown_ = i;
        ensure_built(i);
        win_.relayout_at_once();
    }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        // A click on the empty space below the names reports a change too.
        if (code != LBN_SELCHANGE || current() == shown_) return;
        shown_ = current();
        ensure_built(current());
        win_.relayout_at_once();
        if (changed_) changed_(current());
    }
    void ensure_built(int i)
    {
        if (i < 0 || size_t(i) >= lazy.size() || !lazy[size_t(i)]) return;
        const auto build = std::move(lazy[size_t(i)]);
        lazy[size_t(i)] = nullptr;
        pages[size_t(i)] = buildPage(build());
        win_.relayout();
    }
    bool draw_item(DRAWITEMSTRUCT* d) override
    {
        if (d->itemID == UINT(-1) || d->itemID >= titles_.size()) return false;
        const bool selected = d->itemState & ODS_SELECTED;
        FillRect(d->hDC, &d->rcItem,
                 theme_brush(selected ? theme_color::highlight : theme_color::base));
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, color_ref(theme_color_of(selected ? theme_color::highlight_text
                                                               : theme_color::text)));
        HGDIOBJ old = SelectObject(d->hDC, ui_font(win_.dpi()));
        RECT text = d->rcItem;
        text.left += px(8);
        DrawTextW(d->hDC, titles_[d->itemID].c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(d->hDC, old);
        if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) DrawFocusRect(d->hDC, &d->rcItem);
        return true;
    }
    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        SIZE in{0, 0};
        for (size_t i = 0; i < pages.size(); ++i)
        {
            // An unbuilt page takes the area it's given; once built it has
            // a size like any other.
            if (i < lazy.size() && lazy[i]) continue;
            const SIZE s = pages[i]->natural();
            in.cx = std::max(in.cx, s.cx);
            in.cy = std::max(in.cy, s.cy);
        }
        return {list_width() + gap() + in.cx, std::max<long>(in.cy, long(titles_.size()) * (font_metrics(win_.dpi()).cy + px(12)))};
    }
    void place(RECT r) override
    {
        const int lw = list_width();
        leaf::place({r.left, r.top, r.left + lw, r.bottom});
        for (item* p : pages) p->place({r.left + lw + gap(), r.top, r.right, r.bottom});
    }
    void show_all(bool shown) override
    {
        leaf::show_all(shown);
        if (shown && visible) ensure_built(current());
        for (size_t i = 0; i < pages.size(); ++i) pages[i]->show_all(shown && visible && int(i) == current());
    }

private:
    int gap() const { return px(12); }
    int list_width()
    {
        long w = px(120);
        for (const std::wstring& t : titles_) w = std::max(w, text_extent(h_, t).cx + px(36));
        return int(w);
    }

    std::vector<std::wstring> titles_;
    int shown_ = 0;  // the page showing
    std::function<void(int)> changed_;
};

// -- splitter -----------------------------------------------------------------

// Panes side by side with draggable bars between them. Sizes are device px
// per pane; the window keeps the panes' proportions until a drag.
// A thin splitter's bars (spec::splitter::thin) are as wide to drag, but
// drawn as Explorer draws its panes' divider: the base colour, with a line
// of the divider colour two pixels in from the pane before it. Next to
// lists and trees that's one pixel of line with their white either side,
// not a grey bar that merges with a scroll bar beside it.
class splitter_impl : public rap::splitter, public item
{
public:
    splitter_impl(win_context& win, HWND parent, orientation dir, bool thin)
        : win_(win)
        , parent_(parent)
        , dir_(dir)
        , thin_(thin)
    {
    }
    ~splitter_impl() override
    {
        for (HWND b : bars_) DestroyWindow(b);
    }
    std::vector<item*> panes;

    HWND trace_bar(unsigned index) const
    {
        if (index >= bars_.size()) return nullptr;
        HWND bar = bars_[index];
        return GetWindowThreadProcessId(bar, nullptr) == GetCurrentThreadId() ? bar : nullptr;
    }

    void add_bar()
    {
        register_classes();
        HWND bar = make_host(parent_);
        auto* fn = new std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>(
            [this, index = bars_.size()](HWND h, UINT msg, WPARAM, LPARAM lp, bool& handled) -> LRESULT
            { return bar_message(index, h, msg, lp, handled); });
        SetWindowLongPtrW(bar, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fn));
        // Its erase, which the host's (the window colour) would otherwise
        // take, on screen and printed with a pane's copy alike.
        if (thin_)
            SetWindowSubclass(
                bar,
                [](HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR self) -> LRESULT
                {
                    if (msg != WM_ERASEBKGND) return DefSubclassProc(h, msg, wp, lp);
                    reinterpret_cast<splitter_impl*>(self)->paint_thin_bar(
                        h, reinterpret_cast<HDC>(wp));
                    return 1;
                },
                0, reinterpret_cast<DWORD_PTR>(this));
        bars_.push_back(bar);
        fns_.emplace_back(fn);
    }

    void set_enabled(bool) override {}
    void set_visible(bool v) override
    {
        if (visible == v) return;
        visible = v;
        win_.relayout();
    }
    void set_tooltip(const std::string&) override {}
    void focus() override {}
    bool has_focus() const override { return false; }
    void on_moved(std::function<void()> f) override { moved_ = std::move(f); }
    float divider() const override { return float(bar()) / win_.scale(); }
    // The sizes it was given or dragged to, else those it was laid out at.
    std::vector<float> sizes() const override
    {
        std::vector<float> out;
        for (long s : sizes_.size() == panes.size() ? sizes_ : placed_)
            out.push_back(std::max(0L, s) / win_.scale());
        return out;
    }
    // A size of 0 leaves that pane's as it was.
    void set_sizes(const std::vector<float>& s) override
    {
        trace::scope step(trace::phase::drag_step, uintptr_t(parent_));
        std::vector<long> was = sizes_.size() == panes.size() ? sizes_ : placed_;
        was.resize(panes.size(), 0);
        sizes_.assign(panes.size(), 0);
        for (size_t i = 0; i < panes.size(); ++i)
            sizes_[i] = i < s.size() && s[i] > 0 ? long(s[i] * win_.scale()) : was[i];
        win_.relayout();
    }

    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        long main = 0, cross = 0;
        int shown = 0;
        for (item* p : panes)
        {
            if (!p->visible) continue;
            const SIZE s = size_of(p);
            main += dir_ == orientation::horizontal ? s.cx : s.cy;
            cross = std::max(cross, dir_ == orientation::horizontal ? s.cy : s.cx);
            ++shown;
        }
        main += bar() * long(std::max(shown, 1) - 1);
        return dir_ == orientation::horizontal ? SIZE{main, cross} : SIZE{cross, main};
    }
    void place(RECT r) override
    {
        rect_ = r;
        std::vector<size_t> shown;
        for (size_t i = 0; i < panes.size(); ++i)
            if (panes[i]->visible) shown.push_back(i);
        const long total = dir_ == orientation::horizontal ? r.right - r.left : r.bottom - r.top;
        // Sizes: remembered ones where we have them; the first pane takes the rest.
        std::vector<long> sz(panes.size(), 0);
        long rest = total - bar() * long(std::max<size_t>(shown.size(), 1) - 1);
        if (sizes_.size() != panes.size())
        {
            int stretch_sum = 0;
            for (size_t i : shown) stretch_sum += std::max(panes[i]->stretch, 0);
            long fixed = 0;
            for (size_t i : shown)
                if (!panes[i]->stretch)
                {
                    sz[i] = natural_along(i);
                    fixed += sz[i];
                }
            if (stretch_sum)
                for (size_t i : shown)
                    if (panes[i]->stretch) sz[i] = (rest - fixed) * panes[i]->stretch / stretch_sum;
            // None stretches: the room left over is shared in proportion to
            // their natural sizes (as Qt's splitter starts), not left empty.
            if (!stretch_sum && fixed > 0 && rest > fixed)
            {
                long given = 0;
                for (size_t k = 0; k < shown.size(); ++k)
                {
                    const size_t i = shown[k];
                    const long add = k + 1 < shown.size() ? (rest - fixed) * sz[i] / fixed : rest - fixed - given;
                    sz[i] += add;
                    given += add;
                }
            }
        }
        else if (!shown.empty())
        {
            // The pane that stretches (else the first) takes what the window
            // gains or loses; the others keep the sizes they were dragged to,
            // down to their natural sizes if the window gets too small for that.
            // One never sized (hidden when the others were) starts at its
            // natural size.
            size_t flex = shown[0];
            for (size_t i : shown)
                if (panes[i]->stretch > 0)
                {
                    flex = i;
                    break;
                }
            long given = 0;
            for (size_t i : shown)
                if (i != flex)
                {
                    sz[i] = sizes_[i] > 0 ? sizes_[i] : natural_along(i);
                    given += sz[i];
                }
            sz[flex] = rest - given;
            long short_by = natural_along(flex) - sz[flex];
            for (size_t k = shown.size(); short_by > 0 && k-- > 0;)
            {
                const size_t i = shown[k];
                if (i == flex) continue;
                const long take = std::clamp(sz[i] - natural_along(i), 0L, short_by);
                sz[i] -= take;
                sz[flex] += take;
                short_by -= take;
            }
            sz[flex] = std::max(0L, sz[flex]);
        }
        long pos = dir_ == orientation::horizontal ? r.left : r.top;
        size_t b = 0;
        for (size_t k = 0; k < shown.size(); ++k)
        {
            const size_t i = shown[k];
            const long s = std::max(0L, sz[i]);
            panes[i]->place(dir_ == orientation::horizontal ? RECT{pos, r.top, pos + s, r.bottom}
                                                            : RECT{r.left, pos, r.right, pos + s});
            pos += s;
            if (k + 1 < shown.size() && b < bars_.size())
            {
                HWND bh = bars_[b++];
                if (dir_ == orientation::horizontal)
                    move_control(bh, pos, r.top, bar(), r.bottom - r.top);
                else
                    move_control(bh, r.left, pos, r.right - r.left, bar());
                show_control(bh, true);
                pos += bar();
            }
        }
        for (; b < bars_.size(); ++b) show_control(bars_[b], false);
        placed_ = sz;
    }
    void show_all(bool shown) override
    {
        for (item* p : panes) p->show_all(shown && visible);
        if (!(shown && visible))
            for (HWND b : bars_) show_control(b, false);
    }

private:
    // Five pixels to drag, thin or not.
    int bar() const { return win_.px(5); }
    // The base colour, and the divider's line across it at px(2).
    void paint_thin_bar(HWND h, HDC dc) const
    {
        RECT c;
        GetClientRect(h, &c);
        FillRect(dc, &c, theme_brush(theme_color::base));
        RECT line = c;
        const int at = win_.px(2), width = std::max(1, win_.px(1));
        if (dir_ == orientation::horizontal)
        {
            line.left = c.left + at;
            line.right = line.left + width;
        }
        else
        {
            line.top = c.top + at;
            line.bottom = line.top + width;
        }
        FillRect(dc, &line, theme_brush(theme_color::divider));
    }
    // A pane's size: natural, or the width or height it was given (as a
    // box takes it). Ignoring that, a pane of wrapping text 60 characters
    // wide asked for its text's whole single-line width.
    static SIZE size_of(item* p)
    {
        SIZE s = p->natural();
        if (p->fixed_w > 0) s.cx = p->fixed_w;
        if (p->fixed_h > 0) s.cy = p->fixed_h;
        return s;
    }
    long natural_along(size_t i) const
    {
        const SIZE n = size_of(panes[i]);
        return dir_ == orientation::horizontal ? n.cx : n.cy;
    }
    LRESULT bar_message(size_t index, HWND h, UINT msg, LPARAM lp, bool& handled)
    {
        handled = true;
        switch (msg)
        {
        case WM_SETCURSOR:
            if (!busy_cursor_here(lp))
                SetCursor(LoadCursorW(nullptr,
                                      dir_ == orientation::horizontal ? IDC_SIZEWE : IDC_SIZENS));
            return TRUE;
        case WM_LBUTTONDOWN:
            SetCapture(h);
            dragging_ = true;
            pressed_ = sizes_;
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (!dragging_) break;
            dragging_ = false;
            if (msg == WM_LBUTTONUP) ReleaseCapture();
            if (sizes_ != pressed_ && moved_) moved_();
            return 0;
        case WM_MOUSEMOVE:
        {
            if (!dragging_) break;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            MapWindowPoints(h, parent_, &pt, 1);
            trace::scope input(trace::phase::drag_step, uintptr_t(h));
            if (trace::enabled.load(std::memory_order_relaxed))
            {
                const DWORD sent = DWORD(GetMessageTime());
                trace::mark(trace::phase::input_time, sent);
                trace::mark(trace::phase::input_age, DWORD(GetTickCount() - sent));
            }
            drag_bar(index, dir_ == orientation::horizontal ? pt.x : pt.y);
            return 0;
        }
        }
        handled = false;
        return 0;
    }

    // Bar `index` follows the pointer at `at` (parent coordinates): the pane
    // above it takes the difference, the panes between keep their sizes, and
    // the last pane gives or takes the rest; once the last pane is empty the
    // ones above it shrink in turn, nearest first. Matches GTK's nested
    // panes, so a middle pane stays put while the bar above it moves.
    void drag_bar(size_t index, long at)
    {
        std::vector<size_t> shown;
        for (size_t i = 0; i < panes.size(); ++i)
            if (panes[i]->visible) shown.push_back(i);
        if (index + 1 >= shown.size() || placed_.size() != panes.size()) return;
        std::vector<long> sz(shown.size());
        for (size_t k = 0; k < shown.size(); ++k) sz[k] = std::max(0L, placed_[shown[k]]);

        long bar_start = dir_ == orientation::horizontal ? rect_.left : rect_.top;
        for (size_t k = 0; k <= index; ++k) bar_start += sz[k] + (k < index ? bar() : 0);
        long delta = at - bar() / 2 - bar_start;
        // No pane goes below its natural size: its content doesn't shrink,
        // so less would overlap the next pane or run past the window.
        std::vector<long> least(sz.size());
        for (size_t k = 0; k < sz.size(); ++k) least[k] = std::min(sz[k], natural_along(shown[k]));
        long below = 0;  // what the panes after the bar can give up
        for (size_t k = index + 1; k < sz.size(); ++k) below += sz[k] - least[k];
        delta = std::clamp(delta, least[index] - sz[index], std::max(0L, below));
        if (delta == 0) return;

        sz[index] += delta;
        if (delta < 0) sz.back() -= delta;  // growing: the last pane takes it all
        else
            for (size_t k = sz.size() - 1; delta > 0 && k > index; --k)
            {
                const long take = std::min(delta, sz[k] - least[k]);
                sz[k] -= take;
                delta -= take;
            }
        sizes_.assign(panes.size(), 0);
        for (size_t k = 0; k < shown.size(); ++k) sizes_[shown[k]] = sz[k];
        relayout_dragged();
    }

    // Lays out the panes for the bar's new place and paints them, now and
    // in one go: a posted relayout, and WM_PAINT after it, waited behind
    // the stream of mouse moves, leaving stale pixels where the bar had
    // passed. Only the splitter is laid out again (nothing else changes
    // size, and its measurements hold), its moves in one batch, which
    // paints what they made dirty (and only that: what moved without
    // changing size carried its pixels, and the rest of the window is as
    // it was) before the next mouse move is read. Drawing isn't turned off
    // around it (WM_SETREDRAW): that hides the window from DWM's point of
    // view, and while it was off DWM composed the window without its
    // canvases' GPU layers, showing the GDI surface under them (never
    // painted there, so whatever was once drawn where they lie) for frames
    // at a time.
    void relayout_dragged()
    {
        trace::scope timing(trace::phase::layout, uintptr_t(parent_));
        moves_in_one_pass batch;
        place(rect_);
    }

    win_context& win_;
    HWND parent_;
    orientation dir_;
    bool thin_;
    std::vector<HWND> bars_;
    std::vector<std::unique_ptr<std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>>> fns_;
    std::vector<long> sizes_, placed_;
    RECT rect_{};
    bool dragging_ = false;
    std::vector<long> pressed_;  // the sizes when the drag began
    std::function<void()> moved_;
};

// -- scroll -------------------------------------------------------------------

// A child window that clips its content and scrolls it vertically (and
// horizontally if asked).
class scroll_item : public container_item
{
public:
    scroll_item(win_context& win, HWND parent, bool horizontal)
        : win_(win)
        , horizontal_(horizontal)
    {
        host_ = make_host(parent, WS_VSCROLL | (horizontal ? WS_HSCROLL : 0));
        fn_ = std::make_unique<std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>>(
            [this](HWND, UINT msg, WPARAM wp, LPARAM, bool& handled) -> LRESULT { return message(msg, wp, handled); });
        SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fn_.get()));
        // The content sits on one panel the size of all of it, and scrolling
        // moves just that: moving each control would cost an accessibility
        // event apiece, which is slow whenever an automation client listens.
        // The panel erases under its children: group boxes are transparent
        // and never erase their insides.
        panel_ = make_host(host_);
        // A canvas in it paints with its controls (see canvas_impl::with_pane).
        SetPropW(panel_, L"rapport-pane", &paint_cause_);
        SetWindowLongPtrW(panel_, GWL_STYLE, GetWindowLongPtrW(panel_, GWL_STYLE) & ~WS_CLIPCHILDREN);
        show_control(panel_, true);
    }
    ~scroll_item() override { DestroyWindow(host_); }
    HWND host() const { return panel_; }  // where the content's controls go
    item* content = nullptr;

    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        SIZE c = content ? content->natural() : SIZE{0, 0};
        c.cx += GetSystemMetrics(SM_CXVSCROLL);
        c.cy = std::min<long>(c.cy, font_metrics(win_.dpi()).cy * 10);  // asks for a modest height
        return c;
    }
    void place(RECT r) override
    {
        placed(r);
        // Resizing the host lays the content out (WM_SIZE, below): not again.
        laid_out_ = false;
        move_pane_window(host_, r.left, r.top, r.right - r.left, r.bottom - r.top);
        if (!laid_out_) layout_content();
    }
    // In a layout pass the host and its panel move with their pixels (not
    // erased: the pane showed blank until it was drawn again), and the panel
    // is drawn off screen when it next paints (paint_pane). Elsewhere, as
    // move_now.
    void move_pane_window(HWND h, int x, int y, int w, int hgt)
    {
        if (!g_moves || g_moves->ahead)
        {
            move_now(h, x, y, w, hgt);
            return;
        }
        g_moves->panes.insert(panel_);
        // Without redrawing either: what it uncovers keeps what was there a
        // moment, until the panel's drawn.
        if (!placed_at(h, x, y, w, hgt))
            traced_set_window_pos(h, nullptr, x, y, w, hgt,
                                  SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    }
    void show_all(bool shown) override
    {
        showing(shown);
        show_control(host_, shown && visible);
        if (content) content->show_all(shown && visible);
    }

private:
    // Setting the scroll range shows or hides a scroll bar, which narrows or
    // widens the client area (and sends WM_SIZE, back in here): laid out at
    // the width it had before, content that had just grown taller than the
    // view ran a scroll bar's width past its right edge until the next
    // layout. So it's laid out again until the client area holds still.
    void layout_content()
    {
        if (!content) return;
        if (laying_out_)
        {
            again_ = true;
            return;
        }
        laying_out_ = true;
        laid_out_ = true;
        // (A bar that shows only while the other is hidden could flip for
        // ever: three goes, and the last one stands.)
        for (int tries = 0; tries < 3; ++tries)
        {
            again_ = false;
            if (lay_out_once(tries == 2) && !again_) break;
        }
        laying_out_ = false;
    }
    // False if setting the scroll bars changed the client area, unless it's
    // the last go.
    bool lay_out_once(bool last)
    {
        RECT client;
        GetClientRect(host_, &client);
        const SIZE n = content->natural();
        // As tall as the content is at the width it's given: text that wraps
        // takes more lines when narrower than its natural width.
        const long w = horizontal_ ? std::max<long>(n.cx, client.right) : client.right;
        const long h = std::max<long>(content->height_at(w), client.bottom);
        SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, int(h - 1), UINT(client.bottom), 0, 0};
        y_ = std::clamp(y_, 0L, std::max(0L, h - client.bottom));
        si.nPos = int(y_);
        SetScrollInfo(host_, SB_VERT, &si, TRUE);
        if (horizontal_)
        {
            SCROLLINFO sh{sizeof sh, SIF_RANGE | SIF_PAGE | SIF_POS, 0, int(w - 1), UINT(client.right), 0, 0};
            x_ = std::clamp(x_, 0L, std::max(0L, w - client.right));
            sh.nPos = int(x_);
            SetScrollInfo(host_, SB_HORZ, &sh, TRUE);
        }
        RECT now;
        GetClientRect(host_, &now);
        if (!last && (now.right != client.right || now.bottom != client.bottom)) return false;
        move_pane_window(panel_, -x_, -y_, w, h);
        content->place({0, 0, w, h});
        content_w_ = w, content_h_ = h;
        return true;
    }
    // Scrolling moves what's already drawn: Windows shifts the pixels and the
    // panel together, and only the strip that comes into view paints.
    void scroll_to(long x, long y)
    {
        RECT client;
        GetClientRect(host_, &client);
        x = std::clamp(x, 0L, std::max(0L, content_w_ - client.right));
        y = std::clamp(y, 0L, std::max(0L, content_h_ - client.bottom));
        const int dx = int(x_ - x), dy = int(y_ - y);
        if (!dx && !dy) return;
        x_ = x, y_ = y;
        SetScrollPos(host_, SB_VERT, int(y_), TRUE);
        if (horizontal_) SetScrollPos(host_, SB_HORZ, int(x_), TRUE);
        HRGN exposed = CreateRectRgn(0, 0, 0, 0);
        ScrollWindowEx(host_, dx, dy, nullptr, nullptr, exposed, nullptr, SW_SCROLLCHILDREN);
        redraw_now(host_, nullptr, exposed,
                   RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        DeleteObject(exposed);
    }

    LRESULT message(UINT msg, WPARAM wp, bool& handled)
    {
        handled = true;
        switch (msg)
        {
        case WM_SIZE:
        {
            trace::scope timing(trace::phase::window_size, uintptr_t(host_));
            layout_content();
            return 0;
        }
        case WM_VSCROLL:
        case WM_HSCROLL:
        {
            const bool v = msg == WM_VSCROLL;
            SCROLLINFO si{sizeof si, SIF_ALL};
            GetScrollInfo(host_, v ? SB_VERT : SB_HORZ, &si);
            long pos = v ? y_ : x_;
            const long line = font_metrics(win_.dpi()).cy;
            switch (LOWORD(wp))
            {
            case SB_LINEUP: pos -= line; break;
            case SB_LINEDOWN: pos += line; break;
            case SB_PAGEUP: pos -= long(si.nPage); break;
            case SB_PAGEDOWN: pos += long(si.nPage); break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            }
            if (v)
                scroll_to(x_, pos);
            else
                scroll_to(pos, y_);
            return 0;
        }
        case WM_MOUSEWHEEL:
            scroll_to(x_, y_ - GET_WHEEL_DELTA_WPARAM(wp) * font_metrics(win_.dpi()).cy * 3 / WHEEL_DELTA);
            return 0;
        }
        handled = false;
        return 0;
    }

    win_context& win_;
    bool horizontal_;
    uint64_t paint_cause_ = 0;  // newest move batch awaiting a panel repaint (UI thread)
    HWND host_, panel_;
    std::unique_ptr<std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>> fn_;
    long x_ = 0, y_ = 0;
    long content_w_ = 0, content_h_ = 0;
    bool laying_out_ = false, again_ = false;
    bool laid_out_ = false;  // by the host's WM_SIZE, in place()
};

// -- frames -------------------------------------------------------------------

// One display-paced clock for every frame subscriber: a thread waits for the
// compositor's next frame and posts a tick to the UI thread.
class frame_clock
{
public:
    explicit frame_clock(HWND target)
        : target_(target)
    {
    }
    ~frame_clock() { stop_thread(); }

    subscription subscribe(std::function<void(double)> fn)
    {
        const int id = ++next_;
        subscribers_[id] = std::move(fn);
        start_thread();
        return subscription(
            [this, id, alive = alive_]
            {
                if (!*alive) return;
                subscribers_.erase(id);
            });
    }
    void tick()
    {
        pending_ = false;
        if (subscribers_.empty())
        {
            stop_thread();
            return;
        }
        const double t = now_seconds();
        auto copy = subscribers_;  // callbacks may unsubscribe
        for (auto& [id, fn] : copy)
            if (subscribers_.count(id)) fn(t);
    }

private:
    void start_thread()
    {
        if (running_) return;
        running_ = true;
        thread_ = std::thread(
            [this]
            {
                const com<IDXGIOutput> display = compositor_clock() ? com<IDXGIOutput>() : main_display();
                while (running_)
                {
                    wait_for_frame(display.get());
                    if (!pending_.exchange(true)) PostMessageW(target_, wm_rapport_frame, 0, 0);
                }
            });
    }
    // DwmFlush doesn't pace it: with canvases on composition swap chains
    // (gpu_target) it returns at once. The compositor's clock (Windows 11),
    // or the main display's refresh.
    using compositor_clock_fn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);
    static compositor_clock_fn compositor_clock()
    {
        static const auto fn = reinterpret_cast<compositor_clock_fn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"dcomp.dll"), "DCompositionWaitForCompositorClock")));
        return fn;
    }
    static com<IDXGIOutput> main_display()
    {
        com<IDXGIFactory1> factory;
        com<IDXGIAdapter1> adapter;
        com<IDXGIOutput> output;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put()))) &&
            SUCCEEDED(factory->EnumAdapters1(0, adapter.put())))
            adapter->EnumOutputs(0, output.put());
        return output;
    }
    static void wait_for_frame(IDXGIOutput* display)
    {
        if (compositor_clock())
            compositor_clock()(0, nullptr, 100);  // 100 ms at most, whatever the compositor does
        else if (!display || FAILED(display->WaitForVBlank()))
            Sleep(16);
    }
    void stop_thread()
    {
        if (!running_) return;
        running_ = false;
        if (thread_.joinable()) thread_.join();
        *alive_ = true;
    }

    HWND target_;
    std::atomic<bool> running_{false}, pending_{false};
    std::thread thread_;
    std::map<int, std::function<void(double)>> subscribers_;
    int next_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

frame_clock* g_frames = nullptr;

// -- canvas -------------------------------------------------------------------

// -- GPU ----------------------------------------------------------------------

// One Direct3D device, and Direct2D's over it, for every canvas: an image is
// uploaded once for all of them. Made on a thread: it costs a couple of
// hundred milliseconds the first time, and loading the driver holds the
// process's loader lock all along, so it starts once a first frame is up
// (on the CPU), and canvases take it at their next paint after it's ready.
// When it's lost (a driver update, a GPU reset), it's dropped, the
// canvases go back to the CPU, and a new one is made. UI thread only.
struct gpu_shared
{
    com<ID3D11Device> d3d;
    com<IDXGIFactory2> dxgi;
    com<ID2D1Device> d2d;
    com<IDCompositionDevice> dcomp;  // canvases' swap chains in their windows (see gpu_target)
    uint64_t key = 0;   // its bitmaps' (see new_resource_key)
    uint64_t luid = 0;  // its adapter's, for frame_target::device
    // rap::gpu over the same device, for apps' passes, made when an app
    // first asks for it (canvas::gpu_device; UI thread). The factory is
    // multithreaded from the start: their workers and Direct2D take turns
    // through its lock.
    com<ID2D1Multithread> lock;
    std::shared_ptr<rap::gpu::device> api;
};

// Until the GPU has done what's queued on `dev` (canvases' frames drawn for
// a size their window's about to take), or `max_ms` has passed: a fence and
// its event where there are (Windows 10 1703 on), else an event query.
void wait_for_gpu(const std::shared_ptr<gpu_shared>& dev, DWORD max_ms)
{
    if (!dev) return;
    trace::scope timing(trace::phase::gpu_wait, max_ms);
    com<ID3D11DeviceContext> context;
    dev->d3d->GetImmediateContext(context.put());
    // Apps' passes run on their workers through Direct2D's lock: the context
    // is used under it.
    const auto locked = [&](auto&& f)
    {
        if (dev->lock) dev->lock->Enter();
        f();
        if (dev->lock) dev->lock->Leave();
    };
    com<ID3D11Device5> d3d5;
    com<ID3D11DeviceContext4> context4;
    com<ID3D11Fence> fence;
    if (SUCCEEDED(dev->d3d->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(d3d5.put()))) &&
        SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(context4.put()))) &&
        SUCCEEDED(d3d5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), reinterpret_cast<void**>(fence.put()))))
    {
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (done)
        {
            bool armed = false;
            locked([&] { armed = SUCCEEDED(context4->Signal(fence.get(), 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, done)); context->Flush(); });
            if (armed)
            {
                trace::scope wait(trace::phase::fence_wait, max_ms);
                WaitForSingleObject(done, max_ms);
            }
            CloseHandle(done);
            if (armed) return;
        }
    }
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    com<ID3D11Query> query;
    if (FAILED(dev->d3d->CreateQuery(&desc, query.put()))) return;
    locked([&] { context->End(query.get()); context->Flush(); });
    trace::scope wait(trace::phase::query_wait, max_ms);
    const ULONGLONG until = GetTickCount64() + max_ms;
    for (;;)
    {
        BOOL finished = FALSE;
        HRESULT hr = S_OK;
        locked([&] { hr = context->GetData(query.get(), &finished, sizeof finished, 0); });
        if (FAILED(hr) || (hr == S_OK && finished) || GetTickCount64() >= until) return;
        Sleep(0);
    }
}

class gpu
{
public:
    // The device, or null while there's none (yet); with `start`, one is
    // made if there isn't.
    static std::shared_ptr<gpu_shared> device(bool start)
    {
        state& s = get();
        if (!s.current && s.pending.valid() && s.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            s.current = s.pending.get();
            if (s.current)
                s.current->key = new_resource_key();
            else
                s.failed_at = GetTickCount64();
        }
        // After a failure (no usable GPU, or one mid-reset), a while before the next try.
        if (start && !s.current && !s.pending.valid() && (!s.failed_at || GetTickCount64() - s.failed_at > 10000))
            s.pending = std::async(std::launch::async, make);
        return s.current;
    }
    static void lost(const std::shared_ptr<gpu_shared>& d)
    {
        state& s = get();
        if (!d || s.current != d) return;
        retire_resource_key(d->key);
        s.current.reset();
    }

private:
    struct state
    {
        std::shared_ptr<gpu_shared> current;
        std::future<std::shared_ptr<gpu_shared>> pending;
        ULONGLONG failed_at = 0;
    };
    static state& get()
    {
        static state s;
        return s;
    }
    static std::shared_ptr<gpu_shared> make()
    {
        auto d = std::make_shared<gpu_shared>();
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, d->d3d.put(), nullptr, nullptr)))
            return nullptr;
        com<IDXGIDevice> dxgi_device;
        com<IDXGIAdapter> adapter;
        com<ID2D1Factory1> factory;
        // Its own factory: the shared one would hold the UI up meanwhile.
        if (FAILED(d->d3d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi_device.put()))) ||
            FAILED(dxgi_device->GetAdapter(adapter.put())) ||
            FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(d->dxgi.put()))) ||
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                     reinterpret_cast<void**>(factory.put()))) ||
            FAILED(factory->CreateDevice(dxgi_device.get(), d->d2d.put())) ||
            FAILED(factory->QueryInterface(__uuidof(ID2D1Multithread), reinterpret_cast<void**>(d->lock.put()))) ||
            FAILED(DCompositionCreateDevice(dxgi_device.get(), __uuidof(IDCompositionDevice),
                                            reinterpret_cast<void**>(d->dcomp.put()))))
            return nullptr;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc)))
            d->luid = uint64_t(desc.AdapterLuid.LowPart) | uint64_t(uint32_t(desc.AdapterLuid.HighPart)) << 32;
        return d;  // used on the UI thread from here on
    }
};

// A canvas's drawing on the GPU's device: a swap chain in its window,
// through DirectComposition. The window's own kind (CreateSwapChainForHwnd)
// is scaled to the window by the compositor, and nothing ties a resize to
// the frame for it: the last frame shows stretched until the next lands.
// This one is never scaled: as big as the canvas has been, and cut to the
// window. A frame is drawn into a bitmap of that size that's kept from one
// frame to the next, only where the canvas now reaches: past that is the
// last frame's, as it was. Drawn at a size before the window takes it
// (window::resize), it then shows whole as the window does, and in the
// moment before, the window still the old size, what the new frame doesn't
// cover is the old one rather than a cleared band.
class gpu_target
{
public:
    ~gpu_target()
    {
        // Out of the window at once: what's drawn there next (on the CPU,
        // after a lost device) shows only once the visual's gone.
        if (!target_) return;
        target_->SetRoot(nullptr);
        dev_->dcomp->Commit();
    }
    static std::unique_ptr<gpu_target> make(std::shared_ptr<gpu_shared> dev, HWND h, UINT w, UINT hgt)
    {
        auto t = std::unique_ptr<gpu_target>(new gpu_target(std::move(dev)));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = w;
        desc.Height = hgt;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (FAILED(t->dev_->dxgi->CreateSwapChainForComposition(t->dev_->d3d.get(), &desc, nullptr, t->swap_.put())) ||
            FAILED(t->dev_->dcomp->CreateTargetForHwnd(h, TRUE, t->target_.put())) ||
            FAILED(t->dev_->dcomp->CreateVisual(t->visual_.put())) ||
            FAILED(t->visual_->SetContent(t->swap_.get())) ||
            FAILED(t->target_->SetRoot(t->visual_.get())) ||
            FAILED(t->dev_->d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, t->dc_.put())) ||
            !t->bind() || !t->keep(w, hgt))
            return nullptr;
        t->w_ = w, t->h_ = hgt;
        return t;
    }
    const std::shared_ptr<gpu_shared>& device() const { return dev_; }
    // Drawing for a canvas of w x h: the buffers grow to hold it, and never
    // shrink (what's past the canvas is the last frame's, cut off by the
    // window). The drawing is cut to w x h.
    ID2D1DeviceContext* begin(UINT w, UINT h)
    {
        if (w > w_ || h > h_)
        {
            const UINT bw = std::max(w, w_), bh = std::max(h, h_);
            dc_->SetTarget(nullptr);
            back_.reset();
            if (FAILED(swap_->ResizeBuffers(0, bw, bh, DXGI_FORMAT_UNKNOWN, 0)) || !bind() || !keep(bw, bh))
                return nullptr;
            w_ = bw, h_ = bh;
        }
        dc_->SetTarget(kept_.get());
        dc_->BeginDraw();
        dc_->SetDpi(96, 96);
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        dc_->PushAxisAlignedClip(D2D1::RectF(0, 0, float(w), float(h)), D2D1_ANTIALIAS_MODE_ALIASED);
        return dc_.get();
    }
    // False when the device is lost.
    bool end()
    {
        dc_->PopAxisAlignedClip();
        if (dc_->EndDraw() == D2DERR_RECREATE_TARGET) return false;
        // The kept frame, whole, into the buffer that's shown.
        dc_->SetTarget(back_.get());
        dc_->BeginDraw();
        dc_->SetDpi(96, 96);
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        dc_->DrawBitmap(kept_.get(), D2D1::RectF(0, 0, float(w_), float(h_)), 1, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                        nullptr, nullptr);
        if (dc_->EndDraw() == D2DERR_RECREATE_TARGET) return false;
        dc_->SetTarget(nullptr);
        HRESULT hr;
        {
            trace::scope timing(trace::phase::present, uintptr_t(swap_.get()));
            hr = swap_->Present(0, 0);
        }
        // Into the window with its first frame, not blank before it.
        if (!committed_) committed_ = SUCCEEDED(dev_->dcomp->Commit());
        return hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET;
    }

private:
    explicit gpu_target(std::shared_ptr<gpu_shared> dev)
        : dev_(std::move(dev))
    {
    }
    bool bind()
    {
        com<IDXGISurface> surface;
        com<ID2D1Bitmap1> bitmap;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(swap_->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.put()))) ||
            FAILED(dc_->CreateBitmapFromDxgiSurface(surface.get(), &props, bitmap.put())))
            return false;
        back_ = std::move(bitmap);
        return true;
    }
    // The kept frame at w x h: what the last held, as it was, at its top left.
    bool keep(UINT w, UINT h)
    {
        com<ID2D1Bitmap1> next;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(dc_->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &props, next.put()))) return false;
        if (kept_)
        {
            const D2D1_POINT_2U at{0, 0};
            const D2D1_RECT_U from{0, 0, std::min(w, w_), std::min(h, h_)};
            next->CopyFromBitmap(&at, kept_.get(), &from);
        }
        kept_ = std::move(next);
        return true;
    }

    std::shared_ptr<gpu_shared> dev_;
    com<IDXGISwapChain1> swap_;
    com<IDCompositionTarget> target_;
    com<IDCompositionVisual> visual_;
    com<ID2D1DeviceContext> dc_;
    com<ID2D1Bitmap1> back_;  // the swap chain's buffer, drawn to at present
    com<ID2D1Bitmap1> kept_;  // the frame, kept between frames
    UINT w_ = 0, h_ = 0;
    bool committed_ = false;
};

class canvas_impl : public leaf<rap::canvas>
{
public:
    canvas_impl(win_context& win, HWND parent, const spec::canvas& s)
        : leaf(win)
        , focusable_(s.focusable)
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.style = CS_DBLCLKS;
            wc.hInstance = instance();
            wc.lpfnWndProc = proc;
            wc.lpszClassName = L"rapportCanvas";
            RegisterClassExW(&wc);
            registered = true;
        }
        adopt(CreateWindowExW(0, L"rapportCanvas", L"", WS_CHILD | (s.focusable ? WS_TABSTOP : 0), 0, 0, 0, 0, parent,
                              nullptr, instance(), this));
        track_container_control(h_);
    }
    ~canvas_impl() override
    {
        *alive_ = false;
        if (slot_)
        {
            std::lock_guard<std::mutex> lock(slot_->m);
            slot_->to = nullptr;  // a read back that lands now goes nowhere
        }
        if (dc_key_) retire_resource_key(dc_key_);
        if (cpu_key_) retire_resource_key(cpu_key_);
        retire_gpu_frame();
        finish_reads(true);
    }

    void invalidate() override { InvalidateRect(h_, nullptr, FALSE); }
    size_f size() const override
    {
        win_.flush_layout();
        const SIZE px = pixels();
        const float s = win_.scale();
        return {px.cx / s, px.cy / s};
    }
    // Laid out ahead of the window's resize (see move_batch), it draws for
    // the new size where it is; moved there, it has drawn for it already.
    void place(RECT r) override
    {
        if (g_moves && g_moves->ahead)
        {
            ahead_ = SIZE{r.right - r.left, r.bottom - r.top};
            sized();
            return;
        }
        ahead_.reset();
        leaf::place(r);
        if (placed_at(h_, r.left, r.top, r.right - r.left, r.bottom - r.top)) sized();  // not moved after all
    }
    float device_scale() const override { return win_.scale(); }
    void set_cursor(cursor c) override
    {
        cursor_ = c;
        POINT pt;
        GetCursorPos(&pt);
        if (WindowFromPoint(pt) == h_) apply_cursor();
    }
    void on_paint(std::function<void(rap::painter&, rap::size_f)> f) override { paint_ = std::move(f); }
    void on_resize(std::function<void(size_f)> f) override { resize_ = std::move(f); }
    void on_pointer(std::function<bool(const pointer_event&)> f) override { pointer_ = std::move(f); }
    void on_scroll(std::function<bool(const scroll_event&)> f) override { scroll_ = std::move(f); }
    void on_key(std::function<bool(const key_event&)> f) override { key_ = std::move(f); }
    subscription on_frame(std::function<void(double)> f) override
    {
        return g_frames ? g_frames->subscribe(std::move(f)) : subscription();
    }
    rap::frame_target target() const override
    {
        rap::frame_target t = frames::pixels_target(size(), win_.scale());
        if (gpu_)
        {
            // Frames drawn by Direct2D straight from the app's texture.
            t.kind = rap::frame_target::api::d3d11;
            t.device = gpu_->device()->luid;
            t.formats = {{DXGI_FORMAT_B8G8R8A8_UNORM, {}}, {DXGI_FORMAT_R8G8B8A8_UNORM, {}}};
        }
        return t;
    }
    void on_frame_target(std::function<void(const rap::frame_target&)> f) override { frame_target_ = std::move(f); }
    void present(std::shared_ptr<const rap::image> pixels, frame_color color) override
    {
        retire_gpu_frame();
        if (slot_)
        {
            slot_->supersede();
            shown_pixels_.clear();
            shown_bitmap_.reset();
        }
        frame_ = {std::move(pixels), color};
        invalidate();
    }
    std::shared_ptr<rap::gpu::device> gpu_device() const override
    {
        const std::shared_ptr<gpu_shared> d = gpu_ ? gpu_->device() : gpu::device(false);
        if (d && !d->api) d->api = rap::gpu::d3d11::wrap(d->d3d.get(), d->lock.get());
        return d ? d->api : nullptr;
    }
    void present(rap::gpu::texture_ptr texture, rap::gpu::ticket, frame_color) override
    {
        if (with_pane())
        {
            read_back(std::move(texture));
            return;
        }
        // Drawn on the device that made it, after it in the context's order:
        // `ready` needs no wait. That's the one gpu_device hands out, which
        // the canvas may not have taken yet (it does at its next paint).
        const std::shared_ptr<gpu_shared> dev = gpu_ ? gpu_->device() : gpu::device(false);
        ID3D11Texture2D* native = dev && dev->api && texture ? rap::gpu::d3d11::native(*dev->api, *texture) : nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        if (native) native->GetDesc(&desc);
        if (!native || desc.MipLevels != 1 ||
            (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT))
        {
            OutputDebugStringW(L"rapport: canvas::present: not a texture this canvas can show "
                               L"(its device's, rgba8 or rgba16f, one level)\n");
            return;
        }
        retire_gpu_frame();
        frame_ = {};
        gpu_frame_ = std::make_unique<shown_frame>();
        gpu_frame_->device = dev;
        native->AddRef();
        *gpu_frame_->texture.put() = native;
        gpu_frame_->format = desc.Format;
        gpu_frame_->release = [texture] {};  // the hold on it, dropped once the GPU's done drawing it
        invalidate();
    }
    void present(gpu_frame frame) override
    {
        auto* handle = std::get_if<gpu_frame::d3d11>(&frame.handle);
        com<ID3D11Device1> d3d;
        com<ID3D11Texture2D> texture;
        if (!handle || !gpu_ ||
            FAILED(gpu_->device()->d3d->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(d3d.put()))) ||
            FAILED(d3d->OpenSharedResource1(handle->shared_handle, __uuidof(ID3D11Texture2D),
                                            reinterpret_cast<void**>(texture.put()))))
        {
            if (frame.release) frame.release();  // not one this canvas can show (now)
            return;
        }
        com<ID3D11Fence> fence;
        if (handle->fence_handle)
        {
            com<ID3D11Device5> d3d5;
            if (FAILED(d3d->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(d3d5.put()))) ||
                FAILED(d3d5->OpenSharedFence(handle->fence_handle, __uuidof(ID3D11Fence),
                                             reinterpret_cast<void**>(fence.put()))))
            {
                if (frame.release) frame.release();
                return;
            }
        }
        retire_gpu_frame();
        frame_ = {};
        gpu_frame_ = std::make_unique<shown_frame>();
        gpu_frame_->device = gpu_->device();
        gpu_frame_->texture = std::move(texture);
        D3D11_TEXTURE2D_DESC desc{};
        gpu_frame_->texture->GetDesc(&desc);  // what it is, whatever the frame says
        gpu_frame_->format = desc.Format;
        gpu_frame_->fence = std::move(fence);
        gpu_frame_->fence_value = handle->fence_value;
        gpu_frame_->release = std::move(frame.release);
        invalidate();
    }
    void clear_frame() override
    {
        present(std::shared_ptr<const rap::image>{}, {});
    }

    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override { return {px(64), px(64)}; }

private:
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<canvas_impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self)
        {
            bool handled = true;
            const LRESULT r = self->message(msg, wp, lp, handled);
            if (handled) return r;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    pointer_event make_pointer_event(pointer_event_type t, LPARAM lp, int button) const
    {
        const float s = win_.scale();
        return pointer_event{t, {GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s}, button, 1, key_modifiers()};
    }
    bool press(LPARAM lp, int button)
    {
        if (focusable_ && GetFocus() != h_) SetFocus(h_);
        SetCapture(h_);
        pointer_event e = make_pointer_event(pointer_event_type::press, lp, button);
        const DWORD time = GetMessageTime();
        const bool again = button == last_button_ && time - last_time_ <= GetDoubleClickTime() &&
                           std::abs(GET_X_LPARAM(lp) - last_pos_.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                           std::abs(GET_Y_LPARAM(lp) - last_pos_.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        clicks_ = again ? clicks_ + 1 : 1;
        e.clicks = clicks_;
        last_button_ = button;
        last_time_ = time;
        last_pos_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        return pointer_ && pointer_(e);
    }
    bool release(LPARAM lp, int button)
    {
        if (!(GetKeyState(VK_LBUTTON) & 0x8000) && !(GetKeyState(VK_RBUTTON) & 0x8000) &&
            !(GetKeyState(VK_MBUTTON) & 0x8000))
            ReleaseCapture();
        return pointer_ && pointer_(make_pointer_event(pointer_event_type::release, lp, button));
    }
    void apply_cursor()
    {
        const wchar_t* id = IDC_ARROW;
        switch (cursor_)
        {
        case cursor::default_: id = IDC_ARROW; break;
        case cursor::pointer: id = IDC_HAND; break;
        case cursor::text: id = IDC_IBEAM; break;
        case cursor::crosshair: id = IDC_CROSS; break;
        case cursor::grab:
        case cursor::grabbing:
        case cursor::move: id = IDC_SIZEALL; break;
        // Windows has no zoom cursor: the arrow, as for anything clickable
        // without one of its own (a crosshair says "pick a point").
        case cursor::zoom_in:
        case cursor::zoom_out: id = IDC_ARROW; break;
        case cursor::wait: id = IDC_WAIT; break;
        case cursor::hidden: SetCursor(nullptr); return;
        case cursor::resize_ew: id = IDC_SIZEWE; break;
        case cursor::resize_ns: id = IDC_SIZENS; break;
        case cursor::resize_nwse: id = IDC_SIZENWSE; break;
        case cursor::resize_nesw: id = IDC_SIZENESW; break;
        }
        SetCursor(LoadCursorW(nullptr, id));
    }

    LRESULT message(UINT msg, WPARAM wp, LPARAM lp, bool& handled)
    {
        using t = pointer_event_type;
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h_, &ps);
            if (g_canvases_quiet)
                ;
            else if (with_pane())
            {
                // Nothing of it due (a paint for its frame alone): it's drawn
                // with its pane's print instead.
                if (!IsRectEmpty(&ps.rcPaint)) render_into(dc);
            }
            else
                render();
            EndPaint(h_, &ps);
            // On the GPU or off it: frames come as textures or as pixels.
            if (bool(gpu_) != gpu_reported_)
            {
                gpu_reported_ = bool(gpu_);
                if (frame_target_) frame_target_(target());
            }
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (wp == k_device_timer)
            {
                if (gpu::device(false))
                {
                    KillTimer(h_, k_device_timer);
                    invalidate();
                }
                return 0;
            }
            if (wp != k_reads_timer) return DefWindowProcW(h_, msg, wp, lp);
            finish_reads(false);
            return 0;
        case wm_rapport_read_back:
            take_read_back();
            return 0;
        case WM_PRINTCLIENT:
            if (with_pane()) render_into(reinterpret_cast<HDC>(wp));
            return 0;
        case WM_SIZE:
        {
            trace::scope timing(trace::phase::window_size, uintptr_t(h_));
            // (Quiet, it's still sized: the window's last layout may find it
            // at its size already, and send nothing more.)
            if (cpu_) cpu_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));  // the GPU's resizes as it draws
            if (!g_canvases_quiet) sized();
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: press(lp, 1); return 0;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK: press(lp, 2); return 0;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK: press(lp, 3); return 0;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK:
        {
            const int button = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 4 : 5;
            x_taken_[button - 4] = press(lp, button);
            return TRUE;
        }
        case WM_LBUTTONUP: release(lp, 1); return 0;
        case WM_MBUTTONUP: release(lp, 2); return 0;
        case WM_RBUTTONUP: release(lp, 3); return 0;
        case WM_XBUTTONUP:
        {
            // Taken (its press or this), the button is the canvas's. Else
            // DefWindowProc makes it a WM_APPCOMMAND, which goes up to the
            // window as back or forward.
            // Each button on its own: one may be the canvas's while the
            // other isn't, held at once and let go in either order.
            const int button = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 4 : 5;
            const bool taken = release(lp, button);
            if (std::exchange(x_taken_[button - 4], false) || taken) return TRUE;
            break;
        }
        case WM_MOUSEMOVE:
            if (!inside_)
            {
                inside_ = true;
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h_, 0};
                TrackMouseEvent(&t);
                if (pointer_) pointer_(make_pointer_event(t::enter, lp, 0));
            }
            if (pointer_) pointer_(make_pointer_event(t::move, lp, 0));
            return 0;
        case WM_MOUSELEAVE:
            inside_ = false;
            if (pointer_) pointer_(pointer_event{t::leave, {}, 0, 1, key_modifiers()});
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        {
            if (!scroll_) return 0;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h_, &pt);
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            const float s = win_.scale();
            scroll_event e{{pt.x / s, pt.y / s}, 0, 0, delta % WHEEL_DELTA != 0, key_modifiers()};
            // Windows: positive = away from the user (up/right); rapport: +1 = down/right.
            if (msg == WM_MOUSEWHEEL)
                e.dy = -float(delta) / WHEEL_DELTA;
            else
                e.dx = float(delta) / WHEEL_DELTA;
            scroll_(e);
            return 0;
        }
        case WM_SETCURSOR:
            if (busy_cursor_here(lp)) return TRUE;
            if (LOWORD(lp) == HTCLIENT)
            {
                apply_cursor();
                return TRUE;
            }
            break;
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            // The Back and Forward keys are the window's: DefWindowProc makes
            // them WM_APPCOMMAND, which goes up to it.
            if (wp == VK_BROWSER_BACK || wp == VK_BROWSER_FORWARD) break;
            if (key_ && key_(make_key_event(wp, lp, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN))) return 0;
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS: return 0;
        }
        handled = false;
        return 0;
    }

    // Its size in pixels: the one it's drawing for.
    SIZE pixels() const
    {
        if (ahead_) return *ahead_;
        RECT r;
        GetClientRect(h_, &r);
        return {r.right, r.bottom};
    }
    // The app hears of a size once, whether ahead of the window or as it comes.
    void sized()
    {
        const SIZE px = pixels();
        if (px.cx == told_.cx && px.cy == told_.cy) return;
        told_ = px;
        if (resize_) resize_(size());
        if (frame_target_) frame_target_(target());
    }

    // On the GPU's device when there is one (see gpu), on the CPU until then.
    void render()
    {
        trace::scope timing(trace::phase::canvas_paint, uintptr_t(h_));
        const SIZE px = pixels();
        const UINT w = UINT(std::max<LONG>(px.cx, 1)), h = UINT(std::max<LONG>(px.cy, 1));
        // The first frame doesn't wait for the GPU: it's on the CPU, and the
        // GPU's device starts after it.
        const std::shared_ptr<gpu_shared> dev = gpu::device(painted_);
        if (gpu_ && gpu_->device() != dev) gpu_.reset();  // lost, by another canvas
        if (gpu_frame_ && gpu_frame_->device != dev) drop_gpu_frame();
        if (!gpu_ && dev && (gpu_ = gpu_target::make(dev, h_, w, h)))
            drop_cpu();
        if (gpu_)
        {
            if (gpu_frame_ && gpu_frame_->fence)
            {
                // The app's rendering of it first, on the GPU (no wait here).
                com<ID3D11DeviceContext> context;
                com<ID3D11DeviceContext4> context4;
                dev->d3d->GetImmediateContext(context.put());
                if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext4),
                                                      reinterpret_cast<void**>(context4.put()))))
                    context4->Wait(gpu_frame_->fence.get(), gpu_frame_->fence_value);
            }
            if (ID2D1DeviceContext* dc = gpu_->begin(w, h))
            {
                draw(dc, dev->key, dc);
                if (gpu_->end())
                {
                    painted_ = true;
                    finish_reads(false);
                    return;
                }
            }
            // Lost: on the CPU for now, and a new device is on its way.
            gpu::lost(gpu_->device());
            gpu_.reset();
            drop_gpu_frame();
        }
        if (!cpu_)
        {
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            d2d_factory()->CreateHwndRenderTarget(
                props, D2D1::HwndRenderTargetProperties(h_, D2D1::SizeU(w, h), D2D1_PRESENT_OPTIONS_IMMEDIATELY),
                cpu_.put());
            if (!cpu_) return;
            cpu_key_ = new_resource_key();
        }
        cpu_->BeginDraw();
        draw(cpu_.get(), cpu_key_);
        if (cpu_->EndDraw() == D2DERR_RECREATE_TARGET) drop_cpu();
        if (!painted_)
        {
            painted_ = true;
            gpu::device(true);
        }
        // An app waiting to render frames on the GPU hears of it once the
        // device is up (canvases otherwise take it at their next paint).
        if (frame_target_) SetTimer(h_, k_device_timer, 50, nullptr);
    }
    // In a scroll pane, among its controls (a histogram above its readings
    // and sliders), a canvas paints in WM_PAINT into the DC it's given, as
    // they do: on a swap chain of its own it showed on its own schedule,
    // ahead of or behind them as the pane was resized. The app's textures
    // are read back to pixels for it.
    bool with_pane() const
    {
        for (HWND p = GetParent(h_); p; p = GetParent(p))
            if (GetPropW(p, L"rapport-pane")) return true;
        return false;
    }
    struct read_back_slot
    {
        std::mutex m;
        std::shared_ptr<rap::gpu::device> device;
        HWND to = nullptr;  // null once the canvas has gone
        std::vector<uint8_t> pixels;  // rgba8, rows of w * 4
        int w = 0, h = 0;
        bool ready = false;

        uint64_t trace_cause = 0, trace_frame = 0;
        uint64_t generation = 0;
        uint64_t supersede()
        {
            std::lock_guard<std::mutex> lock(m);
            ready = false;
            device.reset();
            return ++generation;
        }
    };
    void read_back(rap::gpu::texture_ptr texture)
    {
        const std::shared_ptr<rap::gpu::device> dev = gpu_device();
        if (!dev || !texture || texture->format() != rap::gpu::format::rgba8)
        {
            clear_frame();  // supersede pending work and discard the previous pane pixels
            return;
        }
        if (!slot_)
        {
            slot_ = std::make_shared<read_back_slot>();
            slot_->to = h_;
        }
        const int w = texture->width(), h = texture->height();
        auto pixels = std::make_shared<std::vector<std::byte>>(size_t(w) * size_t(h) * 4);
        const uint64_t generation = slot_->supersede();
        std::weak_ptr<read_back_slot> slot = slot_;
        trace::scope submission(trace::phase::readback_submit);
        dev->readback(texture, *pixels, size_t(w) * 4, 0,
                      [dev, slot, pixels, texture, w, h, generation, cause = submission.id(),
                       frame = submission.arg()]
                      {
                          trace::scope timing(trace::phase::readback_complete, frame, cause);
                          const auto s = slot.lock();
                          if (!s || dev->lost()) return;
                          std::lock_guard<std::mutex> lock(s->m);
                          if (s->generation != generation) return;
                          s->trace_cause = cause;
                          s->trace_frame = frame;
                          s->device = dev;
                          s->pixels.assign(reinterpret_cast<const uint8_t*>(pixels->data()),
                                           reinterpret_cast<const uint8_t*>(pixels->data()) + pixels->size());
                          s->w = w, s->h = h, s->ready = true;
                          if (s->to) PostMessageW(s->to, wm_rapport_read_back, 0, 0);
                      });
    }
    // On the UI thread: the pixels read back, shown.
    void take_read_back()
    {
        if (!slot_) return;
        uint64_t cause, frame;
        {
            std::lock_guard<std::mutex> lock(slot_->m);
            if (!slot_->ready) return;
            slot_->ready = false;
            if (std::exchange(slot_->device, nullptr)->lost()) return;
            cause = slot_->trace_cause;
            frame = slot_->trace_frame;
            shown_pixels_.swap(slot_->pixels);
            shown_w_ = slot_->w, shown_h_ = slot_->h;
        }
        trace::scope publication(trace::phase::readback_publish, frame, cause);
        // The readback is ready to replace the previous CPU or GPU frame.
        frame_ = {};
        // Direct2D's bitmaps are BGRA.
        for (size_t i = 0; i + 3 < shown_pixels_.size(); i += 4) std::swap(shown_pixels_[i], shown_pixels_[i + 2]);
        shown_bitmap_.reset();
        readback_paint_cause_ = cause;
        InvalidateRect(h_, nullptr, FALSE);
    }
    void render_into(HDC dc)
    {
        trace::scope timing(trace::phase::canvas_paint, uintptr_t(h_),
                            std::exchange(readback_paint_cause_, 0));
        RECT c;
        GetClientRect(h_, &c);
        if (!dc_rt_)
        {
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            if (FAILED(d2d_factory()->CreateDCRenderTarget(&props, dc_rt_.put()))) return;
            dc_key_ = new_resource_key();
        }
        if (FAILED(dc_rt_->BindDC(dc, &c))) return;
        dc_rt_->BeginDraw();
        dc_rt_->Clear(d2d_color(theme_color_of(surface_of(h_))));
        if (!shown_pixels_.empty() && shown_w_ > 0 && shown_h_ > 0)
        {
            if (!shown_bitmap_)
                dc_rt_->CreateBitmap(D2D1::SizeU(UINT(shown_w_), UINT(shown_h_)), shown_pixels_.data(), UINT(shown_w_) * 4,
                                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
                                     shown_bitmap_.put());
            if (shown_bitmap_)
            {
                dc_rt_->SetDpi(96, 96);
                dc_rt_->SetTransform(D2D1::Matrix3x2F::Identity());
                dc_rt_->DrawBitmap(shown_bitmap_.get(), D2D1::RectF(0, 0, float(shown_w_), float(shown_h_)), 1,
                                   D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
        }
        if (paint_ || frame_)
        {
            d2d_painter p(dc_rt_.get(), win_.scale(), dc_key_);
            frame_.paint(p, size());
            if (paint_) paint_(p, size());
        }
        if (dc_rt_->EndDraw() == D2DERR_RECREATE_TARGET)
        {
            dc_rt_.reset();
            shown_bitmap_.reset();
            retire_resource_key(dc_key_);
            dc_key_ = 0;
        }
        painted_ = true;
    }
    uint64_t readback_paint_cause_ = 0;
    std::shared_ptr<read_back_slot> slot_;
    std::vector<uint8_t> shown_pixels_;
    int shown_w_ = 0, shown_h_ = 0;
    com<ID2D1Bitmap> shown_bitmap_;
    com<ID2D1DCRenderTarget> dc_rt_;
    uint64_t dc_key_ = 0;

    void draw(ID2D1RenderTarget* rt, uint64_t key, ID2D1DeviceContext* gpu = nullptr)
    {
        rt->Clear(d2d_color(theme_color_of(surface_of(h_))));
        if (gpu && gpu_frame_) draw_gpu_frame(gpu);
        if (paint_ || frame_)
        {
            d2d_painter p(rt, win_.scale(), key);
            frame_.paint(p, size());
            if (paint_) paint_(p, size());
        }
    }
    // The app's texture, one of its pixels to one of the window's, top left.
    void draw_gpu_frame(ID2D1DeviceContext* dc)
    {
        shown_frame& f = *gpu_frame_;
        if (!f.bitmap)
        {
            com<IDXGISurface> surface;
            const D2D1_BITMAP_PROPERTIES1 props =
                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(f.format, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(f.texture->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(surface.put()))) ||
                FAILED(dc->CreateBitmapFromDxgiSurface(surface.get(), &props, f.bitmap.put())))
                return;
        }
        const D2D1_SIZE_U px = f.bitmap->GetPixelSize();
        dc->SetDpi(96, 96);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->DrawBitmap(f.bitmap.get(), D2D1::RectF(0, 0, float(px.width), float(px.height)), 1,
                       D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
    }
    // The shown texture stops being drawn: it's released once the GPU has
    // finished what's queued, which includes every read of it.
    void retire_gpu_frame()
    {
        if (!gpu_frame_) return;
        std::unique_ptr<shown_frame> f = std::move(gpu_frame_);
        com<ID3D11DeviceContext> context;
        D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
        com<ID3D11Query> done;
        f->device->d3d->GetImmediateContext(context.put());
        if (FAILED(f->device->d3d->CreateQuery(&desc, done.put())))
        {
            context->Flush();
            if (f->release) f->release();  // nothing to wait with: the best there is
            return;
        }
        context->End(done.get());
        context->Flush();
        reads_.push_back({std::move(done), std::move(context), std::move(f->release)});
        SetTimer(h_, k_reads_timer, 1, nullptr);
    }
    // Its device is gone, and so are the reads.
    void drop_gpu_frame()
    {
        if (!gpu_frame_) return;
        if (gpu_frame_->release) gpu_frame_->release();
        gpu_frame_.reset();
    }
    // Releases the frames the GPU has finished reading. `wait` (the canvas
    // is going): all of them, after a while even unfinished, since each is
    // released exactly once and the app wants its buffers back.
    void finish_reads(bool wait)
    {
        const ULONGLONG until = GetTickCount64() + 250;
        while (!reads_.empty())
        {
            pending_read& r = reads_.front();
            const HRESULT hr = r.context->GetData(r.done.get(), nullptr, 0, 0);
            if (hr == S_FALSE && wait && GetTickCount64() < until)
            {
                Sleep(0);
                continue;
            }
            if (hr == S_FALSE && !wait) break;  // in order: the rest are later still
            auto release = std::move(r.release);
            reads_.erase(reads_.begin());
            if (release) release();
        }
        if (reads_.empty() && IsWindow(h_)) KillTimer(h_, k_reads_timer);
    }
    void drop_cpu()
    {
        cpu_.reset();
        if (cpu_key_) retire_resource_key(cpu_key_);
        cpu_key_ = 0;
    }

    bool focusable_;
    std::unique_ptr<gpu_target> gpu_;
    com<ID2D1HwndRenderTarget> cpu_;
    uint64_t cpu_key_ = 0;
    bool painted_ = false;
    std::optional<SIZE> ahead_;  // the size laid out for, before the window has it (see place)
    SIZE told_{};                // the size the app last heard of
    cursor cursor_ = cursor::default_;
    bool inside_ = false;
    bool x_taken_[2] = {};  // the app took buttons 4 and 5's last presses
    int clicks_ = 0, last_button_ = 0;
    DWORD last_time_ = 0;
    POINT last_pos_{};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::function<void(rap::painter&, rap::size_f)> paint_;
    std::function<void(size_f)> resize_;
    std::function<bool(const pointer_event&)> pointer_;
    std::function<bool(const scroll_event&)> scroll_;
    std::function<bool(const key_event&)> key_;
    frames::frame_layer frame_;
    std::function<void(const rap::frame_target&)> frame_target_;

    // A GPU frame being shown: the app's texture, opened on our device.
    struct shown_frame
    {
        std::shared_ptr<gpu_shared> device;
        com<ID3D11Texture2D> texture;
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
        com<ID3D11Fence> fence;
        uint64_t fence_value = 0;
        com<ID2D1Bitmap1> bitmap;
        std::function<void()> release;
    };
    std::unique_ptr<shown_frame> gpu_frame_;
    // Frames no longer shown, released when the GPU has done with them.
    struct pending_read
    {
        com<ID3D11Query> done;
        com<ID3D11DeviceContext> context;
        std::function<void()> release;
    };
    std::vector<pending_read> reads_;
    static constexpr UINT_PTR k_reads_timer = 0xf4a3, k_device_timer = 0xf4a4;
    bool gpu_reported_ = false;
};

// -- window -------------------------------------------------------------------

// 32-bit BGRA rows (top-down) as a PNG, alpha set opaque (GDI leaves it
// undefined).
static bool write_opaque_png(const std::string& path, uint8_t* px, int width, int height)
{
    for (int i = 0; i < width * height; ++i) px[i * 4 + 3] = 255;
    com<IWICBitmapEncoder> enc;
    com<IWICStream> stream;
    if (!wic_factory() || FAILED(wic_factory()->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromFilename(wide(path).c_str(), GENERIC_WRITE)) ||
        FAILED(wic_factory()->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) ||
        FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)))
        return false;
    com<IWICBitmapFrameEncode> frame;
    if (FAILED(enc->CreateNewFrame(frame.put(), nullptr)) || FAILED(frame->Initialize(nullptr)))
        return false;
    frame->SetSize(UINT(width), UINT(height));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    return SUCCEEDED(
               frame->WritePixels(UINT(height), UINT(width) * 4, UINT(width * height * 4), px)) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

class window_impl : public rap::window, public win_context
{
public:
    window_impl(const window_spec& spec, window_impl* parent)
        : parent(parent)
        , dialog_(spec.dialog)
        , margins_(spec.margins)
        , modal_spec_(spec.modal)
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.hInstance = instance();
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
            wc.lpfnWndProc = proc;
            wc.lpszClassName = L"rapportWindow";
            wc.hIcon = app_icon(SM_CXICON);
            wc.hIconSm = app_icon(SM_CXSMICON);
            RegisterClassExW(&wc);
            // A base surface window's (surface_of).
            wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
            wc.lpszClassName = L"rapportWindowBase";
            RegisterClassExW(&wc);
            registered = true;
        }
        const bool base = spec.base_surface && base_surface_supported();
        DWORD style = dialog_ ? (WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME) : WS_OVERLAPPEDWINDOW;
        if (!spec.resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT | (dialog_ ? WS_EX_DLGMODALFRAME : 0),
                                base ? L"rapportWindowBase" : L"rapportWindow", wide(spec.title).c_str(),
                                style | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                                parent ? parent->hwnd_ : nullptr, nullptr, instance(), this);
        // Before its content is made: the hosts' classes follow it.
        if (base) set_base_surface(hwnd_, true);
        // Before it's first shown, or the title bar flashes light.
        theme_top_window(hwnd_);
        frame_top_window(hwnd_, dialog_);
        dark_ = dark_mode();
        palette_ = palette_now();
        initial_w_ = spec.width;
        initial_h_ = spec.height;
        windows().insert(this);
    }
    ~window_impl() override
    {
        windows().erase(this);
        // WM_DESTROY can't see us once GWLP_USERDATA is cleared below.
        if (last_shown() == this) last_shown() = nullptr;
        items.clear();
        widgets.clear();
        if (hwnd_)
        {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            if (modal_ && parent) EnableWindow(parent->hwnd_, TRUE);
            if (menu_bar_ && GetMenu(hwnd_) != menu_bar_) DestroyMenu(menu_bar_);  // detached: ours to free
            RemovePropW(hwnd_, L"rapport-focus");
            set_base_surface(hwnd_, false);
            DestroyWindow(hwnd_);
        }
    }
    static std::set<window_impl*>& windows()
    {
        static std::set<window_impl*> w;
        return w;
    }

    // Every role's colour now, to tell a change that matters from the
    // many WM_SETTINGCHANGEs that don't.
    static std::array<color, theme_color_count> palette_now()
    {
        std::array<color, theme_color_count> p;
        for (int r = 0; r < theme_color_count; ++r) p[size_t(r)] = theme_color_of(theme_color(r));
        return p;
    }
    // The desktop's colours changed: controls whose style or colours were
    // set for the old mode get the new one's, and everything repaints.
    void retheme()
    {
        const auto now = palette_now();
        const bool mode = dark_ != dark_mode();
        if (!mode && !std::memcmp(now.data(), palette_.data(), sizeof now)) return;
        palette_ = now;
        if (mode)
        {
            dark_ = dark_mode();
            apply_app_mode();
            theme_top_window(hwnd_);
            traced_set_window_pos(hwnd_, nullptr, 0, 0, 0, 0,
                                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                                      SWP_FRAMECHANGED);
        }
        EnumChildWindows(
            hwnd_,
            [](HWND c, LPARAM) -> BOOL
            {
                theme_control(c);
                if (handler* h = handler_of(c)) h->theme_changed();
                return TRUE;
            },
            0);
        if (GetMenu(hwnd_)) DrawMenuBar(hwnd_);
        RedrawWindow(hwnd_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    }
    bool dark_ = false;  // the mode the frame and controls were last styled for
    std::array<color, theme_color_count> palette_{};  // the colours they were last given

    HWND hwnd() const override { return hwnd_; }
    void relayout() override
    {
        if (trace::enabled.load(std::memory_order_relaxed)) layout_cause_ = trace::current_id();
        ++g_layout_pass;
        if (relayout_pending_) return;
        relayout_pending_ = true;
        PostMessageW(hwnd_, wm_rapport_relayout, 0, 0);
    }
    // With drawing off, the old page goes and the new one comes (through
    // the style bit, silently) and everything then paints in one pass,
    // rather than control by control across several frames.
    void relayout_at_once() override
    {
        if (!IsWindowVisible(hwnd_))
        {
            relayout();
            return;
        }
        SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
        grow_to_content();
        layout_now();
        SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
        redraw_now(hwnd_, nullptr, nullptr,
                   RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    // A window made smaller than content it has since gained (a page built
    // the first time it's shown) grows to fit it, as resizing would.
    void grow_to_content()
    {
        if (!root || IsZoomed(hwnd_) || IsIconic(hwnd_)) return;
        RECT c;
        GetClientRect(hwnd_, &c);
        const SIZE n = root->natural();
        const int m = margin();
        const long w = std::max<long>(c.right, n.cx + 2 * m), h = std::max<long>(c.bottom, n.cy + 2 * m);
        if (w == c.right && h == c.bottom) return;
        RECT r{0, 0, w, h};
        adjust(r);
        traced_set_window_pos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void layout_now()
    {
        trace::scope timing(trace::phase::layout, uintptr_t(hwnd_),
                            std::exchange(layout_cause_, 0));
        relayout_pending_ = false;
        if (!root) return;
        RECT r = client();
        const int m = margin();
        InflateRect(&r, -m, -m);
        moves_in_one_pass batch(ahead_.has_value() && !grow_ahead_);
        root->show_all(true);
        root->place(r);
    }

    // Owned parts of the window.
    std::vector<std::unique_ptr<container_state>> container_states;
    item* root = nullptr;
    std::vector<item_ptr> items;
    std::vector<std::unique_ptr<widget>> widgets;  // plain handles for containers
    std::vector<button_impl*> role_buttons;
    std::vector<std::shared_ptr<deferred_build>> deferred;  // built when first idle (see show)
    std::map<std::string, std::vector<form_item*>> form_groups;  // forms aligned by key

    void set_title(const std::string& t) override { SetWindowTextW(hwnd_, wide(t).c_str()); }
    void flush_layout() override
    {
        if (relayout_pending_) layout_now();
    }
    // The spec's size, but never smaller than the content.
    SIZE first_size()
    {
        const SIZE n = root ? root->natural() : SIZE{200, 100};
        const int m = margin();
        return {std::max<long>(n.cx + 2 * m, extent_px(hwnd_, initial_w_, false)),
                std::max<long>(n.cy + 2 * m, extent_px(hwnd_, initial_h_, true))};
    }
    // Once built: sized and laid out, still hidden, so what's in it has its
    // size before the window shows (a program fitting the window to its content).
    void lay_out_hidden()
    {
        const SIZE s = first_size();
        RECT r{0, 0, s.cx, s.cy};
        AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(hwnd_, GWL_STYLE)), GetMenu(hwnd_) != nullptr,
                           DWORD(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
        traced_set_window_pos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        layout_now();
    }
    void show() override
    {
        finished_ = false;
        if (!shown_once_)
        {
            shown_once_ = true;
            // First size: one set by resize() before showing (a restored
            // size) as it is; otherwise the spec's, but never smaller than
            // the content.
            auto [w, h] = first_size();
            if (requested_) w = requested_->cx, h = requested_->cy;
            RECT r{0, 0, w, h};
            AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(hwnd_, GWL_STYLE)), GetMenu(hwnd_) != nullptr,
                               DWORD(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
            int x = 0, y = 0;
            if (parent)
            {
                // Centred over the parent.
                RECT pr;
                GetWindowRect(parent->hwnd_, &pr);
                x = (pr.left + pr.right - (r.right - r.left)) / 2;
                y = (pr.top + pr.bottom - (r.bottom - r.top)) / 2;
            }
            else if (!placed_)
            {
                // Centred on the monitor the pointer is on (where the user
                // just clicked, in a file manager say), rather than where
                // Windows cascades new windows, at the top left.
                POINT cursor{};
                GetCursorPos(&cursor);
                MONITORINFO mi{sizeof mi};
                GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
                const RECT& work = mi.rcWork;
                x = (work.left + work.right - (r.right - r.left)) / 2;
                y = (work.top + work.bottom - (r.bottom - r.top)) / 2;
            }
            else
                flags |= SWP_NOMOVE;
            traced_set_window_pos(hwnd_, nullptr, x, y, r.right - r.left, r.bottom - r.top, flags);
            layout_now();
        }
        if (dialog_ && parent && modal_spec_)
        {
            modal_ = true;
            EnableWindow(parent->hwnd_, FALSE);
        }
        // Snapshots and test runs (RAPPORT_NO_ACTIVATE) show without taking
        // the focus from whatever the user is doing.
        const bool snapshot = std::getenv("RAPPORT_SNAPSHOT") != nullptr || std::getenv("RAPPORT_NO_ACTIVATE") != nullptr;
        // The first time, painted, controls and all, as it's shown: the
        // compositor's opening animation starts from the window complete,
        // not from a frame its contents fill in a moment later. (Cloaking it
        // until painted showed it complete too, but without the animation.)
        ShowWindow(hwnd_, snapshot ? SW_SHOWNOACTIVATE : SW_SHOW);
        if (!painted_once_)
        {
            painted_once_ = true;
            redraw_now(hwnd_, nullptr, nullptr,
                       RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
            // Deferred boxes are built once nothing else is waiting: a
            // timer's message comes only when the queue is otherwise empty.
            if (!deferred.empty()) SetTimer(hwnd_, deferred_timer, USER_TIMER_MINIMUM, nullptr);
        }
        if (!snapshot) SetForegroundWindow(hwnd_);
        last_shown() = this;
        snapshot_if_asked();
    }
    void close() override
    {
        if (modal_ && parent)
        {
            EnableWindow(parent->hwnd_, TRUE);
            modal_ = false;
            SetForegroundWindow(parent->hwnd_);
        }
        ShowWindow(hwnd_, SW_HIDE);
    }
    size_f size() const override
    {
        const RECT r = client();
        const float s = scale();
        return {r.right / s, r.bottom / s};
    }
    // The client area, or the one it's about to have (see resize).
    RECT client() const
    {
        if (ahead_) return *ahead_;
        RECT r;
        GetClientRect(hwnd_, &r);
        return r;
    }
    // The least client area the window takes: never smaller than the content
    // (as GTK and Qt windows), nor than an explicit minimum (WM_GETMINMAXINFO).
    SIZE least_client() const
    {
        SIZE least{px(min_size_.w), px(min_size_.h)};
        if (root)
        {
            const SIZE n = root->natural();
            const int m = margin();
            least.cx = std::max<LONG>(least.cx, n.cx + 2 * m);
            least.cy = std::max<LONG>(least.cy, n.cy + 2 * m);
        }
        return least;
    }
    void resize(size_f sz) override
    {
        trace::scope timing(trace::phase::resize, uintptr_t(hwnd_));
        if (fullscreen_ || IsZoomed(hwnd_)) return;
        // The size it will have: the canvases draw ahead for that, so held to
        // the least the window takes (else drawn for one it never gets, then
        // again, the image placed anew, once Windows has held it there).
        const SIZE least = least_client();
        RECT r{0, 0, std::max<LONG>(px(sz.w), least.cx), std::max<LONG>(px(sz.h), least.cy)};
        if (!shown_once_) requested_ = SIZE{r.right, r.bottom};
        // Canvases draw for the new size before the window takes it (cut to
        // the window meanwhile, not scaled: see gpu_target), so its first
        // frame at that size has their new content. Nothing ties a window's
        // resize to its content's frame: drawn after, the last frame showed
        // stretched for several refreshes.
        const bool ahead = root && IsWindowVisible(hwnd_);
        if (ahead)
        {
            // First for the new size, nothing moved: canvases draw for it.
            ahead_ = r;
            layout_now();
            redraw_now(hwnd_, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
            // Then its controls are moved, cut to the window as it still is,
            // for the larger of its old size and its new one each way, the
            // canvases quiet (their frames drawn for the new size kept):
            // where it grows, all's in place as it does (moved after, they
            // showed a frame at their old places, a canvas at its old size);
            // where it shrinks, a canvas still reaches the old edge, its last
            // frame showing past the new one until the window cuts it there
            // (moved to the new size, the window's own background flashed
            // white in that margin). Laid out for the new size once it's
            // taken it.
            RECT now;
            GetClientRect(hwnd_, &now);
            grow_ahead_ = true;
            g_canvases_quiet = true;
            ahead_ = RECT{0, 0, std::max(r.right, now.right), std::max(r.bottom, now.bottom)};
            layout_now();
            redraw_now(hwnd_, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
            g_canvases_quiet = false;
            ahead_.reset();
            grow_ahead_ = false;
            // And the window takes the size once the canvases' frames for it
            // are done, not as they're queued: a slow one (a raw developed
            // anew) showed the window at its new size before its content.
            wait_for_gpu(gpu::device(false), 1000);
        }
        adjust(r);
        RECT now;
        GetWindowRect(hwnd_, &now);
        place_on_screen(now.left, now.top, r.right - r.left, r.bottom - r.top);
        // Laid out for the size it got: done in WM_SIZE if that changed
        // (normally to what was drawn for: nothing moves), but not if not.
        if (ahead) layout_now();
    }
    void set_min_size(size_f s) override { min_size_ = s; }
    std::optional<point_f> position() const override
    {
        RECT r;
        GetWindowRect(hwnd_, &r);
        const float s = scale();
        return point_f{r.left / s, r.top / s};
    }
    void move(point_f p) override
    {
        placed_ = true;
        RECT now;
        GetWindowRect(hwnd_, &now);
        place_on_screen(px(p.x), px(p.y), now.right - now.left, now.bottom - now.top);
    }
    // A window keeps clear of the taskbar: what would pass the edge of its
    // monitor's work area slides back inside (the visible frame, that is;
    // Windows' resize borders are invisible and may overhang).
    void place_on_screen(int x, int y, int w, int h)
    {
        RECT want{x, y, x + w, y + h};
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST), &mi);
        RECT inset{0, 0, 0, 0};  // invisible border on each side
        RECT outer, visible;
        using dwm_get_fn = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
        static dwm_get_fn dwm_get = reinterpret_cast<dwm_get_fn>(
            reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute")));
        if (dwm_get && GetWindowRect(hwnd_, &outer) &&
            SUCCEEDED(dwm_get(hwnd_, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &visible, sizeof visible)) && IsWindowVisible(hwnd_))
            inset = RECT{visible.left - outer.left, visible.top - outer.top, outer.right - visible.right, outer.bottom - visible.bottom};
        const RECT& work = mi.rcWork;
        const auto fit = [](int pos, int size, int lo, int hi, int before, int after)
        {
            if (pos + size - after > hi) pos = hi - size + after;  // past the far edge: back inside
            if (pos + before < lo) pos = lo - before;                 // the near edge wins if it's too big
            return pos;
        };
        x = fit(x, w, int(work.left), int(work.right), int(inset.left), int(inset.right));
        y = fit(y, h, int(work.top), int(work.bottom), int(inset.top), int(inset.bottom));
        traced_set_window_pos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    rect_f work_area() const override
    {
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        const float s = scale();
        return {mi.rcWork.left / s, mi.rcWork.top / s, (mi.rcWork.right - mi.rcWork.left) / s,
                (mi.rcWork.bottom - mi.rcWork.top) / s};
    }
    bool fullscreen() const override { return fullscreen_; }
    // Snapped (Aero Snap) shows as a size other than the restored one.
    bool maximized() const override
    {
        if (IsZoomed(hwnd_)) return true;
        WINDOWPLACEMENT wp{sizeof wp};
        RECT r;
        if (fullscreen_ || IsIconic(hwnd_) || !GetWindowPlacement(hwnd_, &wp) || !GetWindowRect(hwnd_, &r))
            return false;
        const RECT& n = wp.rcNormalPosition;
        return r.right - r.left != n.right - n.left || r.bottom - r.top != n.bottom - n.top;
    }
    void set_fullscreen(bool on) override
    {
        if (on == fullscreen_) return;
        fullscreen_ = on;
        if (on)
        {
            if (menu_bar_) SetMenu(hwnd_, nullptr);  // no chrome in fullscreen
            placement_.length = sizeof placement_;
            GetWindowPlacement(hwnd_, &placement_);
            style_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongPtrW(hwnd_, GWL_STYLE, (style_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
            frame_top_window(hwnd_, dialog_, true);
            traced_set_window_pos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                                  mi.rcMonitor.right - mi.rcMonitor.left,
                                  mi.rcMonitor.bottom - mi.rcMonitor.top,
                                  SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        }
        else
        {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style_);
            frame_top_window(hwnd_, dialog_);
            if (menu_bar_ && menu_bar_wanted_) SetMenu(hwnd_, menu_bar_);
            SetWindowPlacement(hwnd_, &placement_);
            traced_set_window_pos(hwnd_, nullptr, 0, 0, 0, 0,
                                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                                      SWP_FRAMECHANGED);
        }
        if (on_fullscreen_) on_fullscreen_(on);
    }
    void on_fullscreen_changed(std::function<void(bool)> f) override { on_fullscreen_ = std::move(f); }

    // The menu bar lives in the frame (SetMenu), outside the layout. Each
    // menu is refilled as it opens (WM_INITMENUPOPUP), so it shows the
    // app's state as it is then; its commands have ids from menu_bar_ids up,
    // a block per menu.
    static constexpr UINT menu_bar_ids = 0x4000, menu_bar_block = 0x400;
    void set_menu_bar(const std::vector<menu_bar_item>& menus)
    {
        if (menus.empty()) return;
        menu_bar_ = CreateMenu();
        for (const menu_bar_item& m : menus)
        {
            bar_menu bar{m, CreatePopupMenu(), {}};
            AppendMenuW(menu_bar_, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(bar.popup), mnemonic(m.title).c_str());
            bars_.push_back(std::move(bar));
        }
        SetMenu(hwnd_, menu_bar_);
    }
    void set_app_id(const std::string& id) override
    {
        // PKEY_AppUserModel_ID, spelled out: propkey.h's needs propsys.lib.
        static const PROPERTYKEY app_user_model_id{
            {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
        com<IPropertyStore> store;
        if (FAILED(SHGetPropertyStoreForWindow(hwnd_, __uuidof(IPropertyStore), reinterpret_cast<void**>(store.put()))))
            return;
        PROPVARIANT v{};  // VT_EMPTY: the app's group again
        const std::wstring w = wide(id);
        if (!w.empty())
            if (auto* copy = static_cast<wchar_t*>(CoTaskMemAlloc((w.size() + 1) * sizeof(wchar_t))))
            {
                std::memcpy(copy, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
                v.vt = VT_LPWSTR;
                v.pwszVal = copy;
            }
        store->SetValue(app_user_model_id, v);
        PropVariantClear(&v);
        app_id_ = !w.empty();
    }
    void set_menu_bar_visible(bool on) override
    {
        menu_bar_wanted_ = on;
        if (menu_bar_ && !fullscreen_) SetMenu(hwnd_, on ? menu_bar_ : nullptr);
    }
    bool menu_opening(HMENU popup)
    {
        for (size_t i = 0; i < bars_.size(); ++i)
        {
            bar_menu& bar = bars_[i];
            if (bar.popup != popup) continue;
            while (GetMenuItemCount(popup) > 0) DeleteMenu(popup, 0, MF_BYPOSITION);  // destroys submenus too
            bar.actions.clear();
            bar.bitmaps.held.clear();
            bar.bitmaps.dpi = dpi();
            fill_menu(popup, bar.item.menu ? bar.item.menu() : menu{}, bar.actions,
                      menu_bar_ids + UINT(i) * menu_bar_block, &bar.bitmaps);
            return true;
        }
        return false;
    }
    bool menu_command(UINT id)
    {
        if (id <= menu_bar_ids) return false;
        const size_t i = (id - menu_bar_ids - 1) / menu_bar_block, n = (id - menu_bar_ids) - i * menu_bar_block;
        if (i >= bars_.size() || n == 0 || n > bars_[i].actions.size()) return false;
        if (auto action = bars_[i].actions[n - 1]) action();  // a copy: the action may rebuild the menus
        return true;
    }
    void set_icon(const icon& icon) override
    {
        if (!icon) return;
        // (Not `small`: rpcndr.h defines it as char.)
        HICON big, little;
        if (!icon.draw)
        {
            // Windows has no icon theme: a named window icon is the program's
            // icon resource of that name (e.g. `MYAPP ICON "myapp.ico"`), and
            // without one the window keeps the program's own icon.
            const std::wstring name = wide(icon.name);
            auto load = [&](int metric)
            {
                const int px = GetSystemMetrics(metric);
                return HICON(LoadImageW(instance(), name.c_str(), IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
            };
            big = load(SM_CXICON);
            little = load(SM_CXSMICON);
            if (!big && !little) return;
        }
        else
        {
            big = render_icon_handle(icon, GetSystemMetrics(SM_CXICON), scale());
            little = render_icon_handle(icon, GetSystemMetrics(SM_CXSMICON), scale());
        }
        if (big) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        if (little) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(little));
    }
    void on_key(std::function<bool(const key_event&)> f) override { on_key_ = std::move(f); }
    void on_finished(std::function<void(dialog_role)> f) override { on_finished_ = std::move(f); }
    void on_close_request(std::function<bool()> f) override { on_close_request_ = std::move(f); }
    // Drops on any child control reach the top level as WM_DROPFILES.
    void on_files_dropped(std::function<void(const std::vector<std::string>&)> f) override
    {
        on_files_dropped_ = std::move(f);
        DragAcceptFiles(hwnd_, bool(on_files_dropped_));
        // An elevated window (run as administrator, or started by an
        // installer) gets no drops from Explorer unless it lets the drop
        // messages through Windows' privilege isolation.
        if (on_files_dropped_)
            for (UINT msg : {UINT(WM_DROPFILES), UINT(WM_COPYDATA), UINT(0x0049) /* WM_COPYGLOBALDATA */})
                ChangeWindowMessageFilterEx(hwnd_, msg, MSGFLT_ALLOW, nullptr);
    }

    // From the message loop, before the focused control sees the key.
    bool window_key(const MSG& m)
    {
        if (!on_key_) return false;
        wchar_t cls[32] = {};
        GetClassNameW(GetFocus(), cls, 32);
        const bool typing = (!_wcsicmp(cls, L"Edit") || !_wcsicmp(cls, L"RICHEDIT50W")) &&
                            !(GetWindowLongPtrW(GetFocus(), GWL_STYLE) & ES_READONLY);
        const bool not_text = m.wParam == VK_ESCAPE || (m.wParam >= VK_F1 && m.wParam <= VK_F12);
        if (typing && !not_text) return false;
        // The Back and Forward keys come as WM_APPCOMMAND, whatever has focus.
        if (m.wParam == VK_BROWSER_BACK || m.wParam == VK_BROWSER_FORWARD) return false;
        return on_key_(make_key_event(m.wParam, m.lParam, m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN));
    }

    void finish(dialog_role role)
    {
        close();
        if (finished_) return;
        finished_ = true;
        if (auto f = on_finished_) f(role);  // may delete this window: last
    }

private:
    static window_impl*& last_shown()
    {
        static window_impl* w = nullptr;
        return w;
    }

    int margin() const
    {
        if (!margins_) return 0;
        const metrics m = metrics::for_scale(scale());
        return dialog_ ? m.dialog_margin : m.margin;
    }
    void adjust(RECT& r) const
    {
        AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(hwnd_, GWL_STYLE)), GetMenu(hwnd_) != nullptr,
                           DWORD(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
    }
    void close_requested()
    {
        if (on_close_request_ && !on_close_request_()) return;
        if (dialog_)
            finish(dialog_role::cancel);
        else
            close();
    }

    // RAPPORT_SNAPSHOT=file.png: after RAPPORT_SNAPSHOT_DELAY ms (default 500),
    // save the window shown last as PNG and quit. For checking without a screen grabber.
    void snapshot_if_asked()
    {
        static bool taken = false;
        const char* path = std::getenv("RAPPORT_SNAPSHOT");
        if (taken || !path || !*path) return;
        taken = true;
        const char* delay = std::getenv("RAPPORT_SNAPSHOT_DELAY");
        SetTimer(hwnd_, 0xA11, delay ? UINT(std::atoi(delay)) : 500u, nullptr);
    }
    static void take_snapshot()
    {
        const char* path = std::getenv("RAPPORT_SNAPSHOT");
        if (!path) return;
        save_last_shown(path);
        PostQuitMessage(0);
    }

public:
    // The window shown last, as drawn now, to a PNG.
    static bool save_last_shown(const std::string& path)
    {
        window_impl* w = last_shown();
        if (!w) return false;
        bool ok = false;
        RECT r;
        GetWindowRect(w->hwnd_, &r);
        const int width = r.right - r.left, height = r.bottom - r.top;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(dc, bmp);
        PrintWindow(w->hwnd_, dc, 2 /* PW_RENDERFULLCONTENT */);
        SelectObject(dc, old);
        ok = write_opaque_png(path, static_cast<uint8_t*>(bits), width, height);
        DeleteObject(bmp);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return ok;
    }

private:

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<window_impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        LRESULT r = 0;
        if (self && forward_child_message(h, msg, wp, lp, r)) return r;
        if (self && dark_menu_bar(h, msg, wp, lp, r)) return r;
        if (self)
        {
            switch (msg)
            {
            case WM_SIZE:
            {
                trace::scope timing(trace::phase::window_size, uintptr_t(h));
                if (self->root) self->layout_now();
                return 0;
            }
            case wm_rapport_relayout:
                if (self->relayout_pending_) self->layout_now();
                return 0;
            case WM_GETMINMAXINFO:
            {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                const SIZE least = self->least_client();
                RECT rc{0, 0, least.cx, least.cy};
                if (rc.right > 0 || rc.bottom > 0)
                {
                    self->adjust(rc);
                    mm->ptMinTrackSize.x = std::max<LONG>(mm->ptMinTrackSize.x, rc.right - rc.left);
                    mm->ptMinTrackSize.y = std::max<LONG>(mm->ptMinTrackSize.y, rc.bottom - rc.top);
                }
                return 0;
            }
            case WM_DPICHANGED:
            {
                auto* nr = reinterpret_cast<RECT*>(lp);
                self->refont(HIWORD(wp));
                traced_set_window_pos(h, nullptr, nr->left, nr->top, nr->right - nr->left,
                                      nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_CLOSE: self->close_requested(); return 0;
            // Back and forward: the mouse's side buttons and the keyboard's
            // keys, from wherever in the window (DefWindowProc passes them up
            // from the controls). Not taken, they go on to the system's own
            // handling.
            case WM_APPCOMMAND:
            {
                const int command = GET_APPCOMMAND_LPARAM(lp);
                if (command != APPCOMMAND_BROWSER_BACKWARD && command != APPCOMMAND_BROWSER_FORWARD) break;
                if (!self->on_key_) break;
                key_event e;
                e.key = command == APPCOMMAND_BROWSER_BACKWARD ? key::back : key::forward;
                e.modifiers = key_modifiers();
                if (self->on_key_(e)) return TRUE;
                break;
            }
            // ui::set_busy: the app-starting pointer over the window and
            // its controls, which ask their parent first.
            case WM_SETCURSOR:
                if (busy_cursor_here(lp)) return TRUE;
                break;
            // The desktop's colours (accent, high contrast, the system
            // colours) may have changed: read them again and repaint all.
            // Dark mode ("ImmersiveColorSet") too: every control and the
            // frame take the new mode's style.
            case WM_SETTINGCHANGE:
            case WM_SYSCOLORCHANGE:
            case WM_DWMCOLORIZATIONCOLORCHANGED:
            case WM_THEMECHANGED:
                forget_theme_colors();
                self->retheme();
                break;
            case WM_DROPFILES:
            {
                auto* drop = reinterpret_cast<HDROP>(wp);
                std::vector<std::string> paths;
                const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i)
                {
                    std::wstring path(DragQueryFileW(drop, i, nullptr, 0), L'\0');
                    DragQueryFileW(drop, i, path.data(), UINT(path.size() + 1));
                    paths.push_back(narrow(path));
                }
                DragFinish(drop);
                if (!paths.empty() && self->on_files_dropped_) self->on_files_dropped_(paths);
                return 0;
            }
            // Keyboard focus belongs on a control: the one that had it when
            // the window was last left (or asked for before it showed), else
            // the first Tab stop (a dialog's first field).
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE)
                {
                    if (HWND f = GetFocus(); f && IsChild(h, f)) SetPropW(h, L"rapport-focus", f);
                }
                else if (!HIWORD(wp))  // not minimised
                {
                    auto f = static_cast<HWND>(GetPropW(h, L"rapport-focus"));
                    if (!f || !IsWindow(f) || !IsChild(h, f) || !IsWindowVisible(f) || !IsWindowEnabled(f))
                        f = GetNextDlgTabItem(h, nullptr, FALSE);
                    if (f) SetFocus(f);
                    return 0;
                }
                break;
            case WM_INITMENUPOPUP:
                if (self->menu_opening(reinterpret_cast<HMENU>(wp))) return 0;
                break;
            case WM_COMMAND:
                if (!lp && HIWORD(wp) == 0 && self->menu_command(LOWORD(wp))) return 0;
                if (LOWORD(wp) == IDCANCEL && !lp)
                {
                    if (self->dialog_) self->close_requested();
                    return 0;
                }
                if (LOWORD(wp) == IDOK && !lp)
                {
                    for (button_impl* b : self->role_buttons)
                        if (b->is_default) SendMessageW(b->hwnd(), BM_CLICK, 0, 0);
                    return 0;
                }
                break;
            case WM_TIMER:
                if (wp == 0xA11)
                {
                    KillTimer(h, wp);
                    take_snapshot();
                    return 0;
                }
                if (wp == deferred_timer)
                {
                    KillTimer(h, wp);
                    for (auto& d : std::exchange(self->deferred, {})) d->run();
                    return 0;
                }
                break;
            case WM_DESTROY:
                if (last_shown() == self) last_shown() = nullptr;
                // Windows asks for a window's own taskbar id back before it goes.
                if (self->app_id_) self->set_app_id({});
                break;
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    // Every control gets the font for the new DPI.
    void refont(UINT dpi)
    {
        prepare_wanted_shell_icons(dpi);  // before anything paints at it
        ++g_layout_pass;  // new fonts, new sizes
        EnumChildWindows(
            hwnd_,
            +[](HWND c, LPARAM d) -> BOOL
            {
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font(UINT(d))), TRUE);
                return TRUE;
            },
            LPARAM(dpi));
    }

public:
    window_impl* parent;

private:
    HWND hwnd_ = nullptr;
    bool dialog_, margins_, modal_spec_;
    uint64_t layout_cause_ = 0;  // latest request for a deferred layout
    bool modal_ = false, finished_ = false, shown_once_ = false, fullscreen_ = false, relayout_pending_ = false;
    bool painted_once_ = false;  // painted as it was first shown (see show)
    std::optional<RECT> ahead_;  // the client area being laid out for, before the window has it (see resize)
    bool grow_ahead_ = false;    // and its controls moved for it then too (see resize)
    static constexpr UINT_PTR deferred_timer = 0xDEF;
    std::optional<SIZE> requested_;  // client size from resize() before the first show
    bool placed_ = false;            // move()d by the app: left where it put it
    struct bar_menu
    {
        menu_bar_item item;
        HMENU popup;
        std::vector<std::function<void()>> actions;
        menu_bitmaps bitmaps;  // the popup's icons, until it's rebuilt
    };
    HMENU menu_bar_ = nullptr;  // destroyed with the window while attached
    std::vector<bar_menu> bars_;
    bool menu_bar_wanted_ = true;
    extent initial_w_, initial_h_;
    size_f min_size_{};
    WINDOWPLACEMENT placement_{};
    LONG_PTR style_ = 0;
    std::function<void(bool)> on_fullscreen_;
    std::function<bool(const key_event&)> on_key_;
    std::function<void(dialog_role)> on_finished_;
    std::function<bool()> on_close_request_;
    std::function<void(const std::vector<std::string>&)> on_files_dropped_;
    bool app_id_ = false;  // a taskbar group of its own (set_app_id), cleared before it goes
};

// -- builder ------------------------------------------------------------------

// A dialog button's label in the user's language. OK, Cancel and Close
// have no access key in Windows dialogs (Enter and Esc are theirs).
std::string stock_label(dialog_role r)
{
    std::string s = rap::i18n::stock_label(r);
    if (r == dialog_role::ok || r == dialog_role::cancel || r == dialog_role::close)
        std::erase(s, '_');
    return s;
}

// Windows order: OK Cancel Apply, right-aligned; Help and Reset on the left.
int windows_order(dialog_role r)
{
    switch (r)
    {
    case dialog_role::help: return 0;
    case dialog_role::reset: return 1;
    case dialog_role::ok:
    case dialog_role::yes: return 10;
    case dialog_role::no: return 11;
    case dialog_role::cancel: return 12;
    case dialog_role::close: return 13;
    case dialog_role::apply: return 14;
    default: return 5;
    }
}

class builder
{
public:
    builder(window_impl& win, build_sink& sink, container_state* scope = nullptr)
        : win_(win)
        , sink_(sink)
        , scope_(scope)
        , metrics_(metrics::for_scale(win.scale()))
    {
    }

    item* build(const node& n, HWND parent, orientation parent_dir)
    {
        container_state* scope = scope_;
        if (rap::detail::needs_container_wrapper(n))
        {
            auto state = std::make_unique<container_state>(parent, scope);
            scope = state.get();
            win_.container_states.push_back(std::move(state));
        }
        builder nested = *this;
        nested.scope_ = scope;
        control_scope building(scope);
        item* item = std::visit([&](const auto& s) { return nested.make(n, s, parent, parent_dir); }, n.spec);
        item->stretch = n.common.stretch;
        item->fixed_w = extent_px(win_.hwnd(), n.common.width, false);
        item->fixed_h = extent_px(win_.hwnd(), n.common.height, true);
        return item;
    }

private:
    template <class T, class... Args>
    T* own(Args&&... args)
    {
        auto p = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = p.get();
        win_.items.push_back(std::move(p));
        return raw;
    }
    template <class T, class... Args>
    T* widget(const node& n, Args&&... args)
    {
        T* w = own<T>(std::forward<Args>(args)...);
        sink_.created(n, *w);
        return w;
    }
    void handle(const node& n, item* item, std::shared_ptr<deferred_build> pending = nullptr)
    {
        if (!rap::detail::needs_container_wrapper(n) && !n.common.handle && n.common.visible) return;
        auto* container = dynamic_cast<container_item*>(item);
        if (container) container->state = scope_;
        auto w = std::make_unique<plain_widget>(*item, container ? scope_ : nullptr, win_, std::move(pending));
        sink_.created(n, *w);
        win_.widgets.push_back(std::move(w));
    }

    item* make(const node& n, const spec::box& s, HWND parent, orientation)
    {
        auto* box = own<box_item>(s.dir, metrics_.spacing, s.margins ? metrics_.margin : 0);
        for (const node& c : s.items) box->children.push_back(build(c, parent, s.dir));
        if (!s.deferred)
        {
            handle(n, box);
            return box;
        }
        // Built later; a first ListView alone costs ~15 ms.
        auto pending = std::make_shared<deferred_build>(
            [&win = win_, &sink = sink_, scope = scope_, box, parent, make = s.deferred, dir = s.dir]
            {
                box->children.push_back(builder(win, sink, scope).build(make(), parent, dir));
                // Showing now, the window grows if the content doesn't fit.
                if (box->visible)
                    win.relayout_at_once();
                else
                    win.relayout();
            });
        win_.deferred.push_back(pending);
        handle(n, box, std::move(pending));
        return box;
    }
    item* make(const node& n, const spec::form& s, HWND parent, orientation)
    {
        auto* form = own<form_item>(metrics_.form_column_gap, metrics_.row_gap);
        if (!s.align_group.empty())
        {
            auto& group = win_.form_groups[s.align_group];
            group.push_back(form);
            form->group = &group;
        }
        for (size_t i = 0; i < s.fields.size(); ++i)
        {
            form_item::row row;
            if (i < s.label_widgets.size() && s.label_widgets[i])
            {
                row.label = build(*s.label_widgets[i], parent, orientation::horizontal);
                row.label_at_start = true;
            }
            else if (i < s.labels.size() && !s.labels[i].empty())
            {
                auto* l = own<label_impl>(win_, parent, spec::label{s.labels[i]});
                row.label = l;
            }
            row.field = build(s.fields[i], parent, orientation::horizontal);
            form->rows.push_back(row);
        }
        handle(n, form);
        return form;
    }
    item* make(const node& n, const spec::splitter& s, HWND parent, orientation)
    {
        auto* sp = own<splitter_impl>(win_, parent, s.dir, s.thin);
        for (size_t i = 0; i < s.panes.size(); ++i)
        {
            sp->panes.push_back(build(s.panes[i], parent, s.dir));
            if (i + 1 < s.panes.size()) sp->add_bar();
        }
        sink_.created(n, *sp);
        return sp;
    }
    item* make(const node& n, const spec::group& s, HWND parent, orientation)
    {
        auto* g = own<group_item>(win_, parent, s.title);
        // The first control the content makes follows the last one before it.
        HWND first = GetWindow(parent, GW_CHILD);
        HWND before = first ? GetWindow(first, GW_HWNDLAST) : nullptr;
        if (!s.content.empty()) g->content = build(s.content.front(), parent, orientation::vertical);
        if (HWND made = before ? GetWindow(before, GW_HWNDNEXT) : GetWindow(parent, GW_CHILD))
            name_from_frame(made, wide(s.title));
        handle(n, g);
        return g;
    }
    item* make(const node& n, const spec::scroll& s, HWND parent, orientation)
    {
        auto* sc = own<scroll_item>(win_, parent, s.horizontal);
        if (!s.content.empty()) sc->content = build(s.content.front(), sc->host(), orientation::vertical);
        handle(n, sc);
        return sc;
    }
    item* make(const node& n, const spec::tabs& s, HWND parent, orientation)
    {
        if (s.sidebar) return pages_for(widget<side_tabs_impl>(n, win_, parent, s), s, parent);
        auto* t = pages_for(widget<tabs_impl>(n, win_, parent, s), s, parent);
        // The tab control was created first; pages must paint above it.
        traced_set_window_pos(t->hwnd(), HWND_BOTTOM, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return t;
    }
    template <class T>
    T* pages_for(T* t, const spec::tabs& s, HWND parent)
    {
        for (const node& p : s.pages) t->pages.push_back(build(p, parent, orientation::vertical));
        t->lazy = s.lazy;
        t->buildPage = [&win = win_, &sink = sink_, scope = scope_, parent](const node& page)
        { return builder(win, sink, scope).build(page, parent, orientation::vertical); };
        return t;
    }
    item* make(const node&, const spec::spacer&, HWND, orientation)
    {
        auto* sp = own<spacer_item>();
        sp->is_spacer = true;
        return sp;
    }
    item* make(const node&, const spec::separator&, HWND parent, orientation parent_dir)
    {
        return own<separator_item>(win_, parent,
                                  parent_dir == orientation::vertical ? orientation::horizontal : orientation::vertical);
    }
    item* make(const node& n, const spec::label& s, HWND parent, orientation) { return widget<label_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::button& s, HWND parent, orientation)
    {
        auto* b = widget<button_impl>(n, win_, parent, s.icon_only ? std::string() : s.text, s.flat ? BS_FLAT : 0);
        if (s.icon) b->set_icon(s.icon);
        if (s.icon_only && n.common.tooltip.empty()) b->set_tooltip(s.text);
        return b;
    }
    item* make(const node& n, const spec::toolbar& s, HWND parent, orientation)
    {
        auto* box = own<box_item>(orientation::horizontal, win_.px(2));
        // Each run of buttons and separators is one native toolbar.
        tool_strip* strip = nullptr;
        for (const node& c : s.items)
        {
            const auto* b = std::get_if<spec::tool_button>(&c.spec);
            if (b || std::holds_alternative<spec::separator>(c.spec))
            {
                if (!strip) box->children.push_back(strip = own<tool_strip>(win_, parent));
                if (b)
                {
                    strip_button* button = strip->add_button(*b);
                    win_.items.emplace_back(static_cast<strip_entry*>(button));
                    sink_.created(c, *button);
                }
                else
                {
                    auto* sep = own<strip_entry>();
                    strip->add_separator(sep);
                    handle(c, sep);
                }
                continue;
            }
            if (strip) strip->finish(), strip = nullptr;
            item* it = build(c, parent, orientation::horizontal);
            if (std::holds_alternative<spec::spacer>(c.spec)) it->stretch = 1;
            box->children.push_back(it);
        }
        if (strip) strip->finish();
        handle(n, box);
        return box;
    }
    item* make(const node& n, const spec::tool_button& s, HWND parent, orientation)
    {
        return widget<tool_button_impl>(n, win_, parent, s);
    }
    item* make(const node& n, const spec::slider& s, HWND parent, orientation) { return widget<slider_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::spin& s, HWND parent, orientation) { return widget<spin_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::color_button& s, HWND parent, orientation)
    {
        return widget<color_button_impl>(n, win_, parent, s);
    }
    item* make(const node& n, const spec::check& s, HWND parent, orientation) { return widget<check_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::entry& s, HWND parent, orientation) { return widget<entry_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::drop_down& s, HWND parent, orientation) { return widget<drop_down_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::combo& s, HWND parent, orientation) { return widget<combo_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::text_view& s, HWND parent, orientation)
    {
        return widget<text_view_impl>(n, win_, parent, s);
    }
    item* make(const node& n, const spec::list& s, HWND parent, orientation) { return widget<list_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::menu_button& s, HWND parent, orientation)
    {
        return widget<menu_button_impl>(n, win_, parent, s);
    }
    item* make(const node& n, const spec::progress_bar&, HWND parent, orientation)
    {
        return widget<progress_bar_impl>(n, win_, parent);
    }
    item* make(const node& n, const spec::date_picker& s, HWND parent, orientation)
    {
        return widget<date_picker_impl>(n, win_, parent, s);
    }
    item* make(const node& n, const spec::tree& s, HWND parent, orientation) { return widget<tree_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::canvas& s, HWND parent, orientation) { return widget<canvas_impl>(n, win_, parent, s); }
    item* make(const node&, const spec::buttons& s, HWND parent, orientation)
    {
        auto* row = own<box_item>(orientation::horizontal, metrics_.spacing);
        std::vector<const spec::dialog_button*> order;
        for (const auto& b : s.buttons) order.push_back(&b);
        std::stable_sort(order.begin(), order.end(),
                         [](auto* a, auto* b) { return windows_order(a->role) < windows_order(b->role); });
        bool spaced = false;
        for (const spec::dialog_button* b : order)
        {
            if (!spaced && windows_order(b->role) >= 5)
            {
                row->children.push_back(own<spacer_item>());
                spaced = true;
            }
            const std::string text = b->text.empty() ? stock_label(b->role) : b->text;
            const bool is_default = b->role == dialog_role::ok || b->role == dialog_role::yes;
            auto* w = own<button_impl>(win_, parent, text, is_default ? BS_DEFPUSHBUTTON : 0);
            w->is_default = is_default;
            w->role = b->role;
            if (b->on_click)
                w->on_click(b->on_click);
            else
            {
                const dialog_role role = b->role;
                window_impl* win = &win_;
                w->on_click([win, role] { win->finish(role); });
            }
            sink_.created(*b, *w);
            win_.role_buttons.push_back(w);
            row->children.push_back(w);
        }
        if (!spaced) row->children.insert(row->children.begin(), own<spacer_item>());
        return row;
    }

    window_impl& win_;
    build_sink& sink_;
    container_state* scope_;
    metrics metrics_;
};

// -- backend ------------------------------------------------------------------

class win32_backend : public backend
{
public:
    static backend* create(int*, char**, const look_options& look)
    {
        trace::initialize();
        trace::install_message_hooks();
        palette::app_scheme() = palette::scheme_number(look.color_scheme);
        // Per-monitor DPI awareness (v2) where the system has it.
        using fn_t = BOOL(WINAPI*)(HANDLE);
        if (auto fn = reinterpret_cast<fn_t>(reinterpret_cast<void*>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))))
            fn(reinterpret_cast<HANDLE>(-4) /* PER_MONITOR_AWARE_V2 */);
        OleInitialize(nullptr);  // COM, and the system's drag and drop
        enable_visual_styles();
        g_comctl.resolve();
        INITCOMMONCONTROLSEX icc{sizeof icc,
                                 ICC_WIN95_CLASSES | ICC_DATE_CLASSES | ICC_PROGRESS_CLASS |
                                     ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES |
                                     ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES | ICC_LINK_CLASS};
        InitCommonControlsEx(&icc);
        apply_app_mode();
        if (!d2d_factory() || !dwrite_factory()) return nullptr;
        return new win32_backend;
    }

    win32_backend()
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.hInstance = instance();
        wc.lpfnWndProc = message_proc;
        wc.lpszClassName = L"rapportMessages";
        RegisterClassExW(&wc);
        messages_ = CreateWindowExW(0, L"rapportMessages", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance(), this);
        warm_d2d();
        frames_ = std::make_unique<frame_clock>(messages_);
        g_frames = frames_.get();
    }
    ~win32_backend() override
    {
        trace::remove_message_hooks();
        g_frames = nullptr;
        frames_.reset();
        DestroyWindow(messages_);
    }

    std::string_view name() const override { return "win32"; }

    std::unique_ptr<rap::window> create_window(const window_spec& spec, rap::window* parent, build_sink& sink) override
    {
        auto win = std::make_unique<window_impl>(spec, static_cast<window_impl*>(parent));
        win->set_menu_bar(spec.menu_bar);
        win->root = builder(*win, sink).build(spec.content, win->hwnd(), orientation::vertical);
        win->lay_out_hidden();
        return win;
    }

    int run() override
    {
        if (quit_) return code_;
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0)
        {
            trace::scope dispatch(trace::phase::dispatch, m.message);
            // Esc during a row drag cancels it, before window keys or a
            // dialog's Esc-to-close see it.
            if (g_dragging && m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE)
            {
                g_dragging->cancel_drag();
                continue;
            }
            if (m.message >= WM_KEYFIRST && m.message <= WM_KEYLAST && m.message != WM_CHAR)
            {
                HWND top = GetAncestor(m.hwnd, GA_ROOT);
                bool consumed = false;
                for (window_impl* w : window_impl::windows())
                    if (w->hwnd() == top)
                    {
                        consumed = w->window_key(m);
                        break;
                    }
                if (consumed) continue;
            }
            HWND top = GetAncestor(m.hwnd, GA_ROOT);
            if (top && IsDialogMessageW(top, &m)) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        return code_;
    }
    // As RAPPORT_THEME does: the palette, menus, title bars and controls,
    // in every window now (before one is shown, it starts so).
    void set_color_scheme(color_scheme s) override
    {
        palette::app_scheme() = palette::scheme_number(s);
        forget_theme_colors();
        apply_app_mode();
        for (window_impl* w : window_impl::windows()) w->retheme();
    }
    void quit(int code) override
    {
        quit_ = true;
        code_ = code;
        // A system dialog open (print, files, a message) runs a loop of its
        // own, which the quit doesn't end: it's cancelled, as its close box
        // would, and the quit comes once it has returned (WM_QUIT waits for
        // the messages posted before it).
        EnumThreadWindows(
            GetCurrentThreadId(),
            [](HWND h, LPARAM) -> BOOL
            {
                wchar_t cls[8] = {};
                if (IsWindowVisible(h) && GetClassNameW(h, cls, 8) && std::wcscmp(cls, L"#32770") == 0)
                    PostMessageW(h, WM_CLOSE, 0, 0);
                return TRUE;
            },
            0);
        PostQuitMessage(code);
    }
    void post(std::function<void()> f) override
    {
        PostMessageW(messages_, wm_rapport_post, 0, reinterpret_cast<LPARAM>(new std::function<void()>(std::move(f))));
    }

    void message(rap::window* parent, const message_spec& m, std::function<void(dialog_role)> done) override
    {
        UINT type = MB_OK;
        auto has = [&](dialog_role r) { return std::find(m.buttons.begin(), m.buttons.end(), r) != m.buttons.end(); };
        if (has(dialog_role::yes) && has(dialog_role::no))
            type = has(dialog_role::cancel) ? MB_YESNOCANCEL : MB_YESNO;
        else if (has(dialog_role::ok) && has(dialog_role::cancel))
            type = MB_OKCANCEL;
        switch (m.icon)
        {
        case message_spec::icon::info: type |= MB_ICONINFORMATION; break;
        case message_spec::icon::warning: type |= MB_ICONWARNING; break;
        case message_spec::icon::error: type |= MB_ICONERROR; break;
        case message_spec::icon::question: type |= MB_ICONQUESTION; break;
        }
        const std::wstring text = wide(m.text + (m.detail.empty() ? "" : "\n\n" + m.detail));
        const int r = MessageBoxW(parent ? static_cast<window_impl*>(parent)->hwnd() : nullptr, text.c_str(), wide(m.title).c_str(), type);
        if (!done) return;
        switch (r)
        {
        case IDOK: done(dialog_role::ok); break;
        case IDYES: done(dialog_role::yes); break;
        case IDNO: done(dialog_role::no); break;
        default: done(dialog_role::cancel); break;
        }
    }

    void choose_files(rap::window* parent, const file_dialog_spec& f, std::function<void(std::vector<std::string>)> done) override
    {
        std::vector<std::string> out;
        const bool save = f.mode == file_dialog_spec::mode::save;
        com<IFileDialog> dlg;
        if (SUCCEEDED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       save ? IID_IFileSaveDialog : IID_IFileOpenDialog,
                                       reinterpret_cast<void**>(dlg.put()))))
        {
            DWORD opts = 0;
            dlg->GetOptions(&opts);
            opts |= FOS_FORCEFILESYSTEM;
            if (f.mode == file_dialog_spec::mode::open_many) opts |= FOS_ALLOWMULTISELECT;
            if (f.mode == file_dialog_spec::mode::folder) opts |= FOS_PICKFOLDERS;
            dlg->SetOptions(opts);
            if (!f.title.empty()) dlg->SetTitle(wide(f.title).c_str());
            std::vector<std::wstring> names, specs;
            for (const auto& flt : f.filters)
            {
                names.push_back(wide(flt.name));
                specs.push_back(wide(flt.pattern));
            }
            std::vector<COMDLG_FILTERSPEC> fs;
            for (size_t i = 0; i < names.size(); ++i) fs.push_back({names[i].c_str(), specs[i].c_str()});
            if (!fs.empty()) dlg->SetFileTypes(UINT(fs.size()), fs.data());
            // Saving: a name typed without an extension gets the chosen
            // type's (the dialog follows the type from this default).
            if (f.mode == file_dialog_spec::mode::save && !f.filters.empty())
            {
                const std::string& first = f.filters.front().pattern;
                const size_t dot = first.find("*.");
                const std::string ext = dot == std::string::npos ? "" : first.substr(dot + 2, first.find(';', dot) - dot - 2);
                if (!ext.empty() && ext.find_first_of("*?") == std::string::npos) dlg->SetDefaultExtension(wide(ext).c_str());
            }
            if (!f.initial_path.empty())
            {
                const std::wstring path = wide(f.initial_path);
                const DWORD attrs = GetFileAttributesW(path.c_str());
                std::wstring folder = path, file;
                if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
                {
                    const size_t slash = path.find_last_of(L"\\/");
                    folder = slash == std::wstring::npos ? L"" : path.substr(0, slash);
                    file = slash == std::wstring::npos ? path : path.substr(slash + 1);
                }
                com<IShellItem> item;
                if (!folder.empty() &&
                    SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_IShellItem,
                                                          reinterpret_cast<void**>(item.put()))))
                    dlg->SetFolder(item.get());
                if (!file.empty()) dlg->SetFileName(file.c_str());
            }
            HWND owner = parent ? static_cast<window_impl*>(parent)->hwnd() : nullptr;
            if (SUCCEEDED(dlg->Show(owner)))
            {
                auto path_of = [](IShellItem* item)
                {
                    PWSTR p = nullptr;
                    std::string s;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)))
                    {
                        s = narrow(p);
                        CoTaskMemFree(p);
                    }
                    return s;
                };
                if (f.mode == file_dialog_spec::mode::open_many)
                {
                    com<IShellItemArray> items;
                    if (SUCCEEDED(static_cast<IFileOpenDialog*>(dlg.get())->GetResults(items.put())))
                    {
                        DWORD n = 0;
                        items->GetCount(&n);
                        for (DWORD i = 0; i < n; ++i)
                        {
                            com<IShellItem> it;
                            if (SUCCEEDED(items->GetItemAt(i, it.put()))) out.push_back(path_of(it.get()));
                        }
                    }
                }
                else
                {
                    com<IShellItem> it;
                    if (SUCCEEDED(dlg->GetResult(it.put()))) out.push_back(path_of(it.get()));
                }
            }
        }
        if (done) done(std::move(out));
    }

    void set_clipboard(const std::string& s) override
    {
        const std::wstring w = wide(s);
        if (!OpenClipboard(messages_)) return;
        EmptyClipboard();
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
        std::memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
        CloseClipboard();
    }

    // Another program holding the clipboard open (a clipboard manager, say)
    // is common and brief: a few tries, about 100 ms in all.
    bool open_clipboard()
    {
        bool open = false;
        for (int i = 0; i < 10 && !(open = OpenClipboard(messages_)); ++i) Sleep(10);
        return open;
    }

    void read_clipboard_text(std::function<void(std::optional<std::string>)> done) override
    {
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !open_clipboard())
            return done(std::nullopt);
        std::optional<std::string> text;
        if (HANDLE mem = GetClipboardData(CF_UNICODETEXT))
            if (auto* w = static_cast<const wchar_t*>(GlobalLock(mem)))
            {
                text = narrow(std::wstring(w));
                GlobalUnlock(mem);
            }
        CloseClipboard();
        done(std::move(text));
    }

    bool save_snapshot(const std::string& path) override { return window_impl::save_last_shown(path); }

    // -- printing ----------------------------------------------------------------

    // Page `page` of `job` into a 32-bit DIB on a memory DC (w x h pixels at
    // `dpi`), through a Direct2D target bound to it: the painter is the
    // screen's, its unit the point. False if it couldn't.
    static bool draw_page_bitmap(HDC mem, int w, int h, float dpi, print_job& job, int page,
                                 size_f printable)
    {
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        com<ID2D1DCRenderTarget> rt;
        if (!d2d_factory() || FAILED(d2d_factory()->CreateDCRenderTarget(&props, rt.put())))
            return false;
        const RECT r{0, 0, w, h};
        if (FAILED(rt->BindDC(mem, &r))) return false;
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(1, 1, 1));
        {
            d2d_painter p(rt.get(), dpi / 72.f,
                          0);  // images get a bitmap for this target, not kept
            p.set_printing();
            job.draw_page(p, page, printable);
        }
        return SUCCEEDED(rt->EndDraw());
    }

    // A page as pixels on a DIB section: what draw_page_bitmap draws into.
    struct page_bitmap
    {
        HDC dc = nullptr;
        HBITMAP bmp = nullptr;
        HGDIOBJ old = nullptr;
        void* bits = nullptr;
        BITMAPINFO info{};
        page_bitmap(int w, int h)
        {
            HDC screen = GetDC(nullptr);
            dc = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
            info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
            bmp = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bmp) old = SelectObject(dc, bmp);
        }
        ~page_bitmap()
        {
            if (bmp)
            {
                SelectObject(dc, old);
                DeleteObject(bmp);
            }
            DeleteDC(dc);
        }
    };

    // Page `page` onto a printer's DC (w x h pixels at `dpi`): drawn at the
    // printer's resolution, or as near as 48 megapixels allow (192 MB; A4 at
    // 600 dpi is 35, A3 at 600 dpi 70), then stretched to the page.
    // Direct2D's DC target won't bind a printer's DC, and every driver takes a
    // bitmap.
    static bool draw_print_page(HDC dc, int w, int h, float dpi, print_job& job, int page,
                                size_f printable)
    {
        const double most = 48e6;
        const float rdpi = double(w) * h <= most ? dpi : float(dpi * std::sqrt(most / (double(w) * h)));
        const int rw = std::max(1, int(std::lround(w * rdpi / dpi))),
                  rh = std::max(1, int(std::lround(h * rdpi / dpi)));
        page_bitmap b(rw, rh);
        if (!b.bmp || !draw_page_bitmap(b.dc, rw, rh, rdpi, job, page, printable)) return false;
        SetStretchBltMode(dc, HALFTONE);
        return StretchDIBits(dc, 0, 0, w, h, 0, 0, rw, rh, b.bits, &b.info, DIB_RGB_COLORS,
                             SRCCOPY) != GDI_ERROR;
    }

    // RAPPORT_PRINT_TO=out.png: each page as A4 at 144 dpi, no dialog
    // (out.png, out-2.png...), for tests.
    static void print_to_png(const std::string& path, print_job& job)
    {
        const bool landscape = job.orientation == page_orientation::landscape;
        const size_f paper = landscape ? size_f{842, 595} : size_f{595, 842};
        const float dpi = 144;
        const int w = int(std::lround(paper.w * dpi / 72)),
                  h = int(std::lround(paper.h * dpi / 72));
        int pages = job.pages;
        if (job.on_begin)
            if (const int n = job.on_begin(paper); n >= 0) pages = n;
        bool ok = true;
        for (int page = 0; page < pages && ok; ++page)
        {
            page_bitmap b(w, h);
            ok = b.bmp && draw_page_bitmap(b.dc, w, h, dpi, job, page, paper);
            std::string out = path;
            if (page > 0)
            {
                const size_t dot = path.find_last_of('.');
                const std::string n = "-" + std::to_string(page + 1);
                out = dot == std::string::npos ? path + n
                                               : path.substr(0, dot) + n + path.substr(dot);
            }
            ok = ok && write_opaque_png(out, static_cast<uint8_t*>(b.bits), w, h);
        }
        if (job.done) job.done(ok, ok ? "" : rap::i18n::format(rap::i18n::tr("rapport", "couldn't write {0}"), path));
    }

    void print(rap::window* parent, print_job job) override
    {
        // Once the caller has returned, as everywhere: the dialog is modal.
        post([this, parent, job = std::move(job)]() mutable { run_print(parent, job); });
    }

    // The default printer's settings, movable memory for PRINTDLGEX, turned
    // to `orientation`; null without a default printer.
    static HGLOBAL default_devmode(page_orientation orientation)
    {
        DWORD size = 0;
        GetDefaultPrinterW(nullptr, &size);
        if (!size) return nullptr;
        std::wstring name(size, L'\0');
        if (!GetDefaultPrinterW(name.data(), &size)) return nullptr;
        name.resize(wcslen(name.c_str()));
        HANDLE printer = nullptr;
        if (!OpenPrinterW(name.data(), &printer, nullptr)) return nullptr;
        HGLOBAL mem = nullptr;
        if (const LONG bytes =
                DocumentPropertiesW(nullptr, printer, name.data(), nullptr, nullptr, 0);
            bytes > 0)
            if ((mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size_t(bytes))))
            {
                auto* dm = static_cast<DEVMODEW*>(GlobalLock(mem));
                if (DocumentPropertiesW(nullptr, printer, name.data(), dm, nullptr,
                                        DM_OUT_BUFFER) == IDOK)
                {
                    dm->dmFields |= DM_ORIENTATION;
                    dm->dmOrientation = orientation == page_orientation::landscape
                                            ? DMORIENT_LANDSCAPE
                                            : DMORIENT_PORTRAIT;
                    GlobalUnlock(mem);
                }
                else
                {
                    GlobalUnlock(mem);
                    GlobalFree(mem);
                    mem = nullptr;
                }
            }
        ClosePrinter(printer);
        return mem;
    }

    void run_print(rap::window* parent, print_job& job)
    {
        const auto finish = [&](bool ok, std::string error)
        {
            if (job.done) job.done(ok, std::move(error));
        };
        if (!job.draw_page) return finish(false, rap::i18n::tr("rapport", "nothing to print"));
        if (const char* to = std::getenv("RAPPORT_PRINT_TO"); to && *to)
        {
            const std::string path = to;
            const bool pdf =
                path.size() > 4 && _stricmp(path.c_str() + path.size() - 4, ".pdf") == 0;
            return pdf ? print_to_pdf(path, job) : print_to_png(path, job);
        }

        PRINTDLGEXW pd{};
        pd.lStructSize = sizeof pd;
        // A dialog needs a real window to belong to (not the message-only one).
        pd.hwndOwner = parent ? static_cast<window_impl*>(parent)->hwnd() : GetDesktopWindow();
        // The default printer's settings, from the spooler, to start the dialog
        // in the job's orientation; without one it starts as the system has it.
        pd.hDevMode = job.orientation == page_orientation::automatic
                          ? nullptr
                          : default_devmode(job.orientation);
        const DWORD last = job.pages > 0 ? DWORD(job.pages) : 9999;  // unknown until on_begin: any
        PRINTPAGERANGE range{1, last};
        pd.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOSELECTION | PD_NOCURRENTPAGE |
                   (last == 1 ? PD_NOPAGENUMS : 0);
        pd.nMaxPageRanges = 1;
        pd.lpPageRanges = &range;
        pd.nMinPage = 1;
        pd.nMaxPage = last;
        pd.nCopies = 1;
        pd.nStartPage = START_PAGE_GENERAL;
        const HRESULT hr = PrintDlgExW(&pd);
        const auto release = [&]
        {
            if (pd.hDC) DeleteDC(pd.hDC);
            if (pd.hDevMode) GlobalFree(pd.hDevMode);
            if (pd.hDevNames) GlobalFree(pd.hDevNames);
        };
        if (FAILED(hr) || pd.dwResultAction != PD_RESULT_PRINT || !pd.hDC)
        {
            release();
            // Cancelled is no error; a dialog that couldn't open says why.
            return finish(
                false, FAILED(hr) ? rap::i18n::format(
                                        rap::i18n::tr("rapport", "the print dialog failed ({0})"),
                                        std::format("0x{:08X}", uint32_t(hr)))
                                  : std::string());
        }

        // Only the pages chosen (one range: nMaxPageRanges is 1), else all.
        const bool some = pd.Flags & PD_PAGENUMS;
        std::string error = print_on_dc(pd.hDC, job, some ? int(range.nFromPage) : 1,
                                        some ? int(range.nToPage) : INT_MAX, nullptr);
        release();
        finish(error.empty(), error);
    }

    // The job's pages `from`..`to` (from 1, clamped to the count) as a
    // document on a printer's DC, to `output` (a file) when given. An error,
    // or empty.
    static std::string print_on_dc(HDC dc, print_job& job, int from, int to, const wchar_t* output)
    {
        const float dpi = float(GetDeviceCaps(dc, LOGPIXELSX));
        const int w = GetDeviceCaps(dc, HORZRES), h = GetDeviceCaps(dc, VERTRES);
        const size_f printable{w * 72 / dpi, h * 72 / dpi};
        int pages = job.pages;
        if (job.on_begin)
            if (const int n = job.on_begin(printable); n >= 0) pages = n;
        const std::wstring title = wide(job.title.empty() ? std::string(rap::i18n::tr("rapport", "Document")) : job.title);
        DOCINFOW doc{};
        doc.cbSize = sizeof doc;
        doc.lpszDocName = title.c_str();
        doc.lpszOutput = output;
        if (StartDocW(dc, &doc) <= 0) return rap::i18n::tr("rapport", "the printer didn't start the document");
        bool ok = true;
        for (int page = std::max(1, from); ok && page <= std::min(to, pages); ++page)
            ok = StartPage(dc) > 0 && draw_print_page(dc, w, h, dpi, job, page - 1, printable) &&
                 EndPage(dc) > 0;
        if (!ok)
        {
            AbortDoc(dc);
            return rap::i18n::tr("rapport", "printing failed");
        }
        EndDoc(dc);
        return {};
    }

    // RAPPORT_PRINT_TO=out.pdf: through Windows' own PDF printer, no dialog.
    static void print_to_pdf(const std::string& path, print_job& job)
    {
        const wchar_t* printer = L"Microsoft Print to PDF";
        std::string error;
        HDC dc = nullptr;
        HANDLE h = nullptr;
        std::vector<uint8_t> mode;
        if (OpenPrinterW(const_cast<wchar_t*>(printer), &h, nullptr))
        {
            const LONG size =
                DocumentPropertiesW(nullptr, h, const_cast<wchar_t*>(printer), nullptr, nullptr, 0);
            if (size > 0)
            {
                mode.resize(size_t(size));
                auto* dm = reinterpret_cast<DEVMODEW*>(mode.data());
                if (DocumentPropertiesW(nullptr, h, const_cast<wchar_t*>(printer), dm, nullptr,
                                        DM_OUT_BUFFER) == IDOK &&
                    job.orientation != page_orientation::automatic)
                {
                    dm->dmFields |= DM_ORIENTATION;
                    dm->dmOrientation = job.orientation == page_orientation::landscape
                                            ? DMORIENT_LANDSCAPE
                                            : DMORIENT_PORTRAIT;
                }
            }
            ClosePrinter(h);
            dc = CreateDCW(L"WINSPOOL", printer, nullptr,
                           mode.empty() ? nullptr : reinterpret_cast<DEVMODEW*>(mode.data()));
        }
        if (!dc)
            error = rap::i18n::tr("rapport", "there's no Microsoft Print to PDF printer");
        else
        {
            const std::wstring out = wide(path);
            error = print_on_dc(dc, job, 1, INT_MAX, out.c_str());
            DeleteDC(dc);
        }
        if (job.done) job.done(error.empty(), error);
    }

    void set_clipboard_image(int w, int h, pixel_format format, const uint8_t* pixels, size_t stride) override
    {
        // CF_DIBV5, 32-bit with straight alpha, bottom-up.
        const size_t bytes = size_t(w) * h * 4;
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + bytes);
        auto* hdr = static_cast<BITMAPV5HEADER*>(GlobalLock(mem));
        *hdr = BITMAPV5HEADER{};
        hdr->bV5Size = sizeof(BITMAPV5HEADER);
        hdr->bV5Width = w;
        hdr->bV5Height = h;
        hdr->bV5Planes = 1;
        hdr->bV5BitCount = 32;
        hdr->bV5Compression = BI_BITFIELDS;
        hdr->bV5RedMask = 0x00ff0000;
        hdr->bV5GreenMask = 0x0000ff00;
        hdr->bV5BlueMask = 0x000000ff;
        hdr->bV5AlphaMask = 0xff000000;
        hdr->bV5CSType = LCS_sRGB;
        hdr->bV5SizeImage = DWORD(bytes);
        auto* dst = reinterpret_cast<uint8_t*>(hdr + 1);
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* src = pixels + size_t(y) * stride;
            uint8_t* row = dst + size_t(h - 1 - y) * w * 4;
            for (int x = 0; x < w; ++x)
            {
                const uint8_t* p = src + x * 4;
                if (format == pixel_format::rgba8)
                {
                    row[x * 4 + 0] = p[2];
                    row[x * 4 + 1] = p[1];
                    row[x * 4 + 2] = p[0];
                    row[x * 4 + 3] = p[3];
                }
                else
                {
                    const unsigned a = p[3];
                    auto un = [a](unsigned c) { return uint8_t(a ? std::min(255u, (c * 255 + a / 2) / a) : 0); };
                    row[x * 4 + 0] = un(p[0]);
                    row[x * 4 + 1] = un(p[1]);
                    row[x * 4 + 2] = un(p[2]);
                    row[x * 4 + 3] = uint8_t(a);
                }
            }
        }
        GlobalUnlock(mem);
        if (!OpenClipboard(messages_))
        {
            GlobalFree(mem);
            return;
        }
        EmptyClipboard();
        SetClipboardData(CF_DIBV5, mem);
        CloseClipboard();
    }

    // A DIB (CF_DIBV5, then CF_DIB, which Windows also makes of a bitmap
    // put there), else the PNG browsers and many programs add, decoded
    // once the clipboard is let go.
    void read_clipboard_image(std::function<void(std::optional<sys::decoded_image>)> done) override
    {
        static const UINT png = RegisterClipboardFormatW(L"PNG");
        const UINT format = IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5
                            : IsClipboardFormatAvailable(CF_DIB) ? CF_DIB
                            : IsClipboardFormatAvailable(png)    ? png
                                                                 : 0;
        if (!format || !open_clipboard()) return done(std::nullopt);
        std::vector<uint8_t> bytes;
        if (HANDLE mem = GetClipboardData(format))
            if (const auto* p = static_cast<const uint8_t*>(GlobalLock(mem)))
            {
                bytes.assign(p, p + GlobalSize(mem));
                GlobalUnlock(mem);
            }
        CloseClipboard();
        done(format == png ? sys::decode_image(bytes) : dib_image(bytes));
    }

    // A DIB's pixels as straight RGBA: 16, 24 or 32 bits a pixel, either
    // way up, the channels where BI_BITFIELDS's masks put them. Alpha only
    // where a V3 header or later gives it a mask, and straight, as
    // set_clipboard_image writes it; all 0, it's an opaque image whose
    // writer left the byte unset. nullopt for any other kind.
    static std::optional<sys::decoded_image> dib_image(std::span<const uint8_t> dib)
    {
        DWORD size = 0;
        if (dib.size() >= sizeof size) std::memcpy(&size, dib.data(), sizeof size);
        if (size < sizeof(BITMAPINFOHEADER) || size > dib.size()) return std::nullopt;
        BITMAPV5HEADER h{};
        std::memcpy(&h, dib.data(), std::min<size_t>(size, sizeof h));
        const int bits = h.bV5BitCount, w = h.bV5Width;
        const bool fields = h.bV5Compression == BI_BITFIELDS;
        if (w <= 0 || h.bV5Height == 0 || h.bV5Height == INT_MIN ||
            (bits != 16 && bits != 24 && bits != 32) || (!fields && h.bV5Compression != BI_RGB) ||
            (fields && bits == 24))
            return std::nullopt;
        const int rows = std::abs(h.bV5Height);
        // The masks: in the header past BITMAPINFOHEADER's fields (alpha's
        // from V3 on), else after it for BI_BITFIELDS, else BI_RGB's 5-5-5
        // or 8-8-8.
        DWORD masks[4] = {h.bV5RedMask, h.bV5GreenMask, h.bV5BlueMask,
                          size >= offsetof(BITMAPV5HEADER, bV5CSType) ? h.bV5AlphaMask : 0};
        size_t at = size;
        if (fields && size == sizeof(BITMAPINFOHEADER))
        {
            if (dib.size() < at + 12) return std::nullopt;
            std::memcpy(masks, dib.data() + at, 12);
            at += 12;
        }
        else if (!fields)
        {
            masks[0] = bits == 16 ? 0x7c00 : 0xff0000;
            masks[1] = bits == 16 ? 0x03e0 : 0x00ff00;
            masks[2] = bits == 16 ? 0x001f : 0x0000ff;
            if (bits != 32) masks[3] = 0;
        }
        at += size_t(std::min<DWORD>(h.bV5ClrUsed, 256)) * 4;  // an optional palette
        const size_t stride = (size_t(w) * bits + 31) / 32 * 4;
        // The CF_DIBV5 Windows makes of a bitmap has the masks again after
        // its header, as BITMAPINFOHEADER would (set_clipboard_image's
        // doesn't): skipped where they're there and the pixels still fit.
        if (fields && size > sizeof(BITMAPINFOHEADER) && dib.size() >= at + 12 &&
            std::memcmp(dib.data() + at, masks, 12) == 0 &&
            (dib.size() - at - 12) / stride >= size_t(rows))
            at += 12;
        if (dib.size() < at || (dib.size() - at) / stride < size_t(rows)) return std::nullopt;

        // Each channel to 8 bits: its top 8, or a narrower one scaled up.
        auto channel = [](uint32_t px, DWORD mask) -> uint8_t
        {
            if (!mask) return 255;
            const int width = std::popcount(mask);
            const uint32_t v = (px & mask) >> std::countr_zero(mask);
            return uint8_t(width >= 8 ? v >> (width - 8) : v * 255 / ((1u << width) - 1));
        };
        sys::decoded_image out{w, rows, pixel_format::rgba8, {}};
        out.pixels.resize(size_t(w) * rows * 4);
        bool alpha = false;
        for (int y = 0; y < rows; ++y)
        {
            const uint8_t* src =
                dib.data() + at + size_t(h.bV5Height > 0 ? rows - 1 - y : y) * stride;
            uint8_t* dst = out.pixels.data() + size_t(y) * w * 4;
            for (int x = 0; x < w; ++x, src += bits / 8, dst += 4)
            {
                uint32_t px = 0;
                std::memcpy(&px, src, size_t(bits / 8));  // little-endian
                for (int c = 0; c < 4; ++c) dst[c] = channel(px, masks[c]);
                alpha |= dst[3] != 0;
            }
        }
        if (!alpha)
            for (size_t i = 3; i < out.pixels.size(); i += 4) out.pixels[i] = 255;
        return out;
    }

    std::unique_ptr<rap::image> create_image(int w, int h, pixel_format format, const uint8_t* pixels, size_t stride) override
    {
        return std::make_unique<d2d_image>(w, h, format, pixels, stride);
    }

    subscription timer(int ms, bool repeat, std::function<void()> fn) override
    {
        const UINT_PTR id = ++next_timer_;
        timers_[id] = {std::move(fn), repeat};
        SetTimer(messages_, id, UINT(std::max(ms, 1)), nullptr);
        return subscription(
            [this, id, alive = alive_]
            {
                if (!*alive) return;
                if (timers_.erase(id)) KillTimer(messages_, id);
            });
    }
    // Windows' app-starting pointer, which each window gives in
    // WM_SETCURSOR (its children ask it first); the one under the pointer
    // asked again now, so it changes without waiting for a move.
    void set_busy(bool on) override
    {
        if (app_busy == on) return;
        app_busy = on;
        POINT p;
        if (!GetCursorPos(&p)) return;
        HWND under = WindowFromPoint(p);
        DWORD pid = 0;
        if (!under || !GetWindowThreadProcessId(under, &pid) || pid != GetCurrentProcessId())
            return;
        const LRESULT hit =
            SendMessageW(under, WM_NCHITTEST, 0, MAKELPARAM(short(p.x), short(p.y)));
        SendMessageW(under, WM_SETCURSOR, WPARAM(under), MAKELPARAM(LOWORD(hit), WM_MOUSEMOVE));
    }

private:
    // Common controls v6 (themed controls) without an application manifest:
    // activate the one shell32 carries for itself.
    static void enable_visual_styles()
    {
        wchar_t dir[MAX_PATH];
        GetSystemDirectoryW(dir, MAX_PATH);
        const std::wstring shell32 = std::wstring(dir) + L"\\shell32.dll";
        ACTCTXW ctx{};
        ctx.cbSize = sizeof ctx;
        ctx.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_SET_PROCESS_DEFAULT | ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID;
        ctx.lpSource = shell32.c_str();
        ctx.lpAssemblyDirectory = dir;
        ctx.lpResourceName = MAKEINTRESOURCEW(124);
        HANDLE h = CreateActCtxW(&ctx);
        if (h != INVALID_HANDLE_VALUE)
        {
            ULONG_PTR cookie = 0;
            ActivateActCtx(h, &cookie);
        }
    }

    static LRESULT CALLBACK message_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<win32_backend*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        switch (msg)
        {
        case wm_rapport_post:
        {
            std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lp));
            (*fn)();
            return 0;
        }
        case wm_rapport_frame:
            if (self && self->frames_) self->frames_->tick();
            return 0;
        case WM_TIMER:
            if (self)
            {
                auto it = self->timers_.find(wp);
                if (it == self->timers_.end())
                {
                    KillTimer(h, wp);
                    return 0;
                }
                auto fn = it->second.fn;  // may cancel itself
                if (!it->second.repeat)
                {
                    KillTimer(h, wp);
                    self->timers_.erase(it);
                }
                fn();
            }
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    struct timer_entry
    {
        std::function<void()> fn;
        bool repeat;
    };
    HWND messages_ = nullptr;
    std::unique_ptr<frame_clock> frames_;
    std::map<UINT_PTR, timer_entry> timers_;
    UINT_PTR next_timer_ = 0x100;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    bool quit_ = false;
    int code_ = 0;
};

}  // namespace
}  // namespace rap::win32

RAPPORT_BACKEND_ENTRY(win32, rap::win32::win32_backend)

// Private harness lookup: the caller keeps the splitter alive and drives input on its UI thread.
extern "C" __declspec(dllexport) void* rapport_win32_trace_splitter_bar(rap::splitter* splitter,
                                                                      unsigned index)
{
    auto* native = dynamic_cast<rap::win32::splitter_impl*>(splitter);
    return native ? native->trace_bar(index) : nullptr;
}
