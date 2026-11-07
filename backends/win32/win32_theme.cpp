// Windows' dark mode for native chrome: the app's menus, title bars, the
// common controls' dark visual styles and a dark menu bar. Windows
// documents only the title bar's (DWMWA_USE_IMMERSIVE_DARK_MODE); the rest
// is what Explorer, Notepad and the Windows Terminal use: uxtheme's exports
// by ordinal, and the DarkMode_* visual style classes. Each is looked for
// before use and left alone where missing, which leaves light chrome.
// Windows 11's frame too: rounded corners and Mica behind the title bar,
// left square and plain before it.
#include "win32_common.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <richedit.h>
#include <uxtheme.h>
#include <vssym32.h>

namespace rap::win32
{

DWORD windows_build()
{
    static const DWORD build = []
    {
        using fn = LONG(WINAPI*)(OSVERSIONINFOW*);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto get = ntdll ? reinterpret_cast<fn>(
                               reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
                         : nullptr;
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof v;
        return get && get(&v) == 0 ? v.dwBuildNumber : DWORD(0);
    }();
    return build;
}

namespace
{

// uxtheme's dark mode exports, which have ordinals and no names, from
// Windows 10 1809 (build 17763). 135 is SetPreferredAppMode from 1903
// (18362), AllowDarkModeForApp(BOOL) in 1809: an int either way.
struct uxtheme_dark
{
    using set_app_mode_fn = int(WINAPI*)(int mode);
    using allow_window_fn = BOOL(WINAPI*)(HWND, BOOL);
    using void_fn = void(WINAPI*)();
    set_app_mode_fn set_app_mode = nullptr;  // 135
    allow_window_fn allow_window = nullptr;  // 133: AllowDarkModeForWindow
    void_fn flush_menus = nullptr;           // 136: FlushMenuThemes
    void_fn refresh_policy = nullptr;        // 104: RefreshImmersiveColorPolicyState
};

const uxtheme_dark& uxtheme()
{
    static const uxtheme_dark u = []
    {
        uxtheme_dark u;
        if (windows_build() < 17763) return u;
        HMODULE m = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return u;
        auto ordinal = [m](int n)
        { return reinterpret_cast<void*>(GetProcAddress(m, MAKEINTRESOURCEA(n))); };
        u.set_app_mode = reinterpret_cast<uxtheme_dark::set_app_mode_fn>(ordinal(135));
        u.allow_window = reinterpret_cast<uxtheme_dark::allow_window_fn>(ordinal(133));
        u.flush_menus = reinterpret_cast<uxtheme_dark::void_fn>(ordinal(136));
        u.refresh_policy = reinterpret_cast<uxtheme_dark::void_fn>(ordinal(104));
        return u;
    }();
    return u;
}

bool class_is(HWND h, const wchar_t* name)
{
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return !_wcsicmp(cls, name);
}

// Windows 11's frame attributes (build 22000 on), by number: older SDKs and
// MinGW's headers don't name them, and they're enumerators, not macros.
constexpr DWORD dwmwa_window_corner_preference = 33;
constexpr DWORD dwmwa_systembackdrop_type = 38;
constexpr DWORD dwmwcp_default = 0, dwmwcp_round = 2, dwmwcp_roundsmall = 3;
constexpr DWORD dwmsbt_mainwindow = 2, dwmsbt_transientwindow = 3;

}  // namespace

void apply_app_mode()
{
    const uxtheme_dark& u = uxtheme();
    if (!u.set_app_mode) return;
    // Menus follow the app mode: Default 0 (light), AllowDark 1 (as the
    // system says), ForceDark 2. RAPPORT_THEME=dark forces dark; light is
    // Default rather than ForceLight, which left the tab control without
    // its labels; high contrast keeps Windows' own. 1809's
    // AllowDarkModeForApp takes a BOOL.
    const int forced = palette::forced_scheme();
    int mode = high_contrast() || forced == 2 ? 0 : forced == 1 ? 2 : 1;
    if (windows_build() < 18362) mode = dark_mode() ? 1 : 0;
    u.set_app_mode(mode);
    if (u.refresh_policy) u.refresh_policy();
    if (u.flush_menus) u.flush_menus();
}

void theme_top_window(HWND h)
{
    const BOOL dark = dark_mode();
    if (uxtheme().allow_window) uxtheme().allow_window(h, dark);
    if (windows_build() >= 17763)
    {
        // DWMWA_USE_IMMERSIVE_DARK_MODE: 20, documented from Windows 11
        // and 20H1 (build 18985); 19 before.
        const DWORD attribute = windows_build() >= 18985 ? 20 : 19;
        DwmSetWindowAttribute(h, attribute, &dark, sizeof dark);
    }
}

void frame_top_window(HWND h, bool dialog, bool fullscreen)
{
    // Older builds don't know the attributes and refuse them: square, plain.
    if (windows_build() < 22000) return;
    // Square in fullscreen, where the window is the whole monitor.
    const DWORD corners = fullscreen ? dwmwcp_default : dwmwcp_round;
    DwmSetWindowAttribute(h, dwmwa_window_corner_preference, &corners, sizeof corners);
    // Mica behind the title bar only (22621 on): the frame isn't extended
    // into the client area, which the controls paint opaque. It takes the
    // title bar's dark mode, and high contrast turns it off.
    if (windows_build() >= 22621)
    {
        const DWORD backdrop = dialog ? dwmsbt_transientwindow : dwmsbt_mainwindow;
        DwmSetWindowAttribute(h, dwmwa_systembackdrop_type, &backdrop, sizeof backdrop);
    }
}

void frame_popup(HWND h)
{
    if (windows_build() < 22000) return;
    const DWORD corners = dwmwcp_roundsmall;
    DwmSetWindowAttribute(h, dwmwa_window_corner_preference, &corners, sizeof corners);
}

namespace
{

// A list's column headers in dark mode: the ItemsView style draws their
// text dark unless told the colour (the header's custom draw, which goes to
// the list).
LRESULT CALLBACK dark_header_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_NOTIFY && dark_mode())
    {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom == ListView_GetHeader(h))
        {
            auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
            {
                SetTextColor(cd->hdc, color_ref(theme_color_of(theme_color::header_text)));
                return CDRF_DODEFAULT;
            }
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// A sunken edge (WS_EX_CLIENTEDGE) in dark mode: rich text and list boxes
// draw theirs white; over it, the border colour outside and the field's
// inside.
void paint_dark_edge(HWND h, HDC dc)
{
    RECT w;
    GetWindowRect(h, &w);
    OffsetRect(&w, -w.left, -w.top);
    FrameRect(dc, &w, theme_brush(theme_color::border));
    InflateRect(&w, -1, -1);
    FrameRect(dc, &w, theme_brush(theme_color::base));
}
// On screen (WM_NCPAINT), and printed (WM_PRINT with PRF_NONCLIENT: a scroll
// pane drawn off screen, which validates the frame's paint as well).
LRESULT CALLBACK dark_edge_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    const bool print = msg == WM_PRINT && (lp & PRF_NONCLIENT);
    if ((msg != WM_NCPAINT && !print) || !dark_mode() ||
        !(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_CLIENTEDGE))
        return DefSubclassProc(h, msg, wp, lp);
    const LRESULT r = DefSubclassProc(h, msg, wp, lp);
    if (print)
        paint_dark_edge(h, reinterpret_cast<HDC>(wp));
    else if (HDC dc = GetWindowDC(h))
    {
        paint_dark_edge(h, dc);
        ReleaseDC(h, dc);
    }
    return r;
}

// A separator (an etched static) in dark mode: its etched edge is drawn
// white whatever WM_CTLCOLORSTATIC says, so the line is drawn here, a
// pixel of the divider colour across its middle.
// The whole window, on `dc` (0, 0 its top left).
void paint_dark_separator(HWND h, HDC dc)
{
    RECT w;
    GetWindowRect(h, &w);
    OffsetRect(&w, -w.left, -w.top);
    FillRect(dc, &w, theme_brush(theme_color::window));
    const bool vertical = (GetWindowLongPtrW(h, GWL_STYLE) & SS_TYPEMASK) == SS_ETCHEDVERT;
    RECT line = w;
    if (vertical)
    {
        line.left = (w.left + w.right - 1) / 2;
        line.right = line.left + 1;
    }
    else
    {
        line.top = (w.top + w.bottom - 1) / 2;
        line.bottom = line.top + 1;
    }
    FillRect(dc, &line, theme_brush(theme_color::divider));
}
LRESULT CALLBACK dark_separator_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (!dark_mode()) return DefSubclassProc(h, msg, wp, lp);
    switch (msg)
    {
    case WM_ERASEBKGND: return 1;
    case WM_NCPAINT: return 0;  // (no frame: the etched edge is all there is)
    case WM_PAINT:
    {
        // Validated, then the whole window painted: the etched edge's
        // shadow row isn't left to the static.
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        if (HDC dc = GetWindowDC(h))
        {
            paint_dark_separator(h, dc);
            ReleaseDC(h, dc);
        }
        return 0;
    }
    // Printed as painted (a scroll pane drawn off screen): no frame, so the
    // client is the window.
    case WM_PRINT:
    case WM_PRINTCLIENT:
        if (lp & PRF_CLIENT) paint_dark_separator(h, reinterpret_cast<HDC>(wp));
        return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

}  // namespace

void theme_control(HWND h)
{
    const bool dark = dark_mode();
    if (uxtheme().allow_window) uxtheme().allow_window(h, dark);
    // Light is each control's default style (nullptr), except trees,
    // which have Explorer's.
    const wchar_t* explorer = dark ? L"DarkMode_Explorer" : nullptr;
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    if (class_is(h, WC_LISTVIEWW))
    {
        SetWindowTheme(h, explorer, nullptr);
        if (HWND header = ListView_GetHeader(h))
            SetWindowTheme(header, dark ? L"DarkMode_ItemsView" : nullptr, nullptr);
        if (HWND tip = ListView_GetToolTips(h)) SetWindowTheme(tip, explorer, nullptr);
        SetWindowSubclass(h, dark_header_proc, 0x7d, 0);
        ListView_SetBkColor(h, color_ref(theme_color_of(theme_color::base)));
        ListView_SetTextBkColor(h, color_ref(theme_color_of(theme_color::base)));
        ListView_SetTextColor(h, color_ref(theme_color_of(theme_color::text)));
    }
    else if (class_is(h, WC_TREEVIEWW))
    {
        SetWindowTheme(h, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        TreeView_SetBkColor(h, dark ? color_ref(theme_color_of(theme_color::base)) : COLORREF(-1));
        TreeView_SetTextColor(h,
                              dark ? color_ref(theme_color_of(theme_color::text)) : COLORREF(-1));
        if (HWND tip = TreeView_GetToolTips(h)) SetWindowTheme(tip, explorer, nullptr);
    }
    else if (class_is(h, L"Button"))
    {
        const LONG_PTR type = style & BS_TYPEMASK;
        if (type != BS_OWNERDRAW && type != BS_GROUPBOX) SetWindowTheme(h, explorer, nullptr);
    }
    else if (class_is(h, L"Edit"))
        // A field's frame and fill from the file dialogs' dark style; a
        // multi-line one's scroll bars from Explorer's.
        SetWindowTheme(h,
                       (style & ES_MULTILINE) ? explorer
                       : dark                 ? L"DarkMode_CFD"
                                              : nullptr,
                       nullptr);
    else if (class_is(h, L"ComboBox"))
    {
        SetWindowTheme(h, dark ? L"DarkMode_CFD" : nullptr, nullptr);
        COMBOBOXINFO info{};
        info.cbSize = sizeof info;
        if (GetComboBoxInfo(h, &info) && info.hwndList)
            SetWindowTheme(info.hwndList, explorer, nullptr);
    }
    else if (class_is(h, L"Static"))
    {
        const LONG_PTR type = style & SS_TYPEMASK;
        if (type == SS_ETCHEDHORZ || type == SS_ETCHEDVERT)
        {
            SetWindowSubclass(h, dark_separator_proc, 0x7f, 0);
            InvalidateRect(h, nullptr, TRUE);
        }
    }
    else if (class_is(h, L"ListBox"))
    {
        SetWindowTheme(h, explorer, nullptr);
        SetWindowSubclass(h, dark_edge_proc, 0x7e, 0);
        RedrawWindow(h, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
    }
    else if (class_is(h, L"RICHEDIT50W"))
    {
        SetWindowTheme(h, explorer, nullptr);
        SetWindowSubclass(h, dark_edge_proc, 0x7e, 0);
        RedrawWindow(h, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
        SendMessageW(h, EM_SETBKGNDCOLOR, 0, LPARAM(color_ref(theme_color_of(theme_color::base))));
    }
    else if (class_is(h, WC_LINK))
    {
        // The link role (the accent's text step, as Windows' own links),
        // not the system's fixed link blue, unreadable on dark besides: the
        // link takes its text colour from WM_CTLCOLORSTATIC, as
        // LIS_DEFAULTCOLORS asks.
        // Themed (light), it takes the visual style's link blue whatever it's
        // told: unthemed, it draws as themed but in the colour it's given.
        if (!dark) SetWindowTheme(h, L"", L"");
        LITEM item{};
        item.mask = LIF_ITEMINDEX | LIF_STATE;
        item.iLink = 0;
        item.state = LIS_DEFAULTCOLORS;
        item.stateMask = LIS_DEFAULTCOLORS;
        SendMessageW(h, LM_SETITEM, 0, reinterpret_cast<LPARAM>(&item));
        InvalidateRect(h, nullptr, TRUE);
    }
    else if (class_is(h, TOOLTIPS_CLASSW) || class_is(h, L"rapportHost") ||
             class_is(h, L"ScrollBar") || class_is(h, UPDOWN_CLASSW))
        SetWindowTheme(h, explorer, nullptr);
}

// -- the menu bar -------------------------------------------------------------

namespace
{

// The menu bar's owner-draw messages, which Windows sends to every
// window with a menu bar and documents nowhere (as Notepad++, the Windows
// Terminal and others use them). Their layouts, as uxtheme has them:
constexpr UINT wm_uahdrawmenu = 0x0091;
constexpr UINT wm_uahdrawmenuitem = 0x0092;
struct uah_menu
{
    HMENU menu;
    HDC dc;
    DWORD flags;
};
struct uah_menu_item
{
    int position;  // within the menu
    DWORD metrics[8];
    DWORD popup_widths[4];
    DWORD update_max_widths;
};
struct uah_draw_menu_item
{
    DRAWITEMSTRUCT dis;
    uah_menu um;
    uah_menu_item umi;
};

// The menu bar's rectangle, in window coordinates.
bool menu_bar_rect(HWND h, RECT& bar)
{
    MENUBARINFO info{};
    info.cbSize = sizeof info;
    if (!GetMenuBarInfo(h, OBJID_MENU, 0, &info)) return false;
    RECT window;
    GetWindowRect(h, &window);
    bar = info.rcBar;
    OffsetRect(&bar, -window.left, -window.top);
    return true;
}

}  // namespace

bool dark_menu_bar(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result)
{
    if (!dark_mode() || !GetMenu(h)) return false;
    switch (msg)
    {
    case wm_uahdrawmenu:
    {
        auto* m = reinterpret_cast<uah_menu*>(lp);
        RECT bar;
        if (menu_bar_rect(h, bar)) FillRect(m->dc, &bar, theme_brush(theme_color::window));
        result = 0;
        return true;
    }
    case wm_uahdrawmenuitem:
    {
        auto* d = reinterpret_cast<uah_draw_menu_item*>(lp);
        wchar_t text[256] = {};
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof mii;
        mii.fMask = MIIM_STRING;
        mii.dwTypeData = text;
        mii.cch = UINT(std::size(text) - 1);
        GetMenuItemInfoW(d->um.menu, UINT(d->umi.position), TRUE, &mii);
        const UINT state = d->dis.itemState;
        const bool open = state & (ODS_HOTLIGHT | ODS_SELECTED);
        const bool off = state & (ODS_INACTIVE | ODS_GRAYED | ODS_DISABLED);
        FillRect(d->um.dc, &d->dis.rcItem,
                 theme_brush(state & ODS_SELECTED ? theme_color::pressed
                             : open               ? theme_color::hover
                                                  : theme_color::window));
        DWORD flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
        if (state & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
        SetBkMode(d->um.dc, TRANSPARENT);
        SetTextColor(d->um.dc, color_ref(theme_color_of(off ? theme_color::disabled_text
                                                            : theme_color::window_text)));
        DrawTextW(d->um.dc, text, -1, &d->dis.rcItem, flags);
        result = 0;
        return true;
    }
    case WM_NCPAINT:
    case WM_NCACTIVATE:
    {
        // The frame's own painting leaves a light line between the menu
        // bar and the client area: over it in the window colour.
        result = DefWindowProcW(h, msg, wp, lp);
        RECT client;
        GetClientRect(h, &client);
        MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&client), 2);
        RECT window;
        GetWindowRect(h, &window);
        OffsetRect(&client, -window.left, -window.top);
        RECT line = client;
        line.bottom = line.top;
        line.top = line.top - 1;
        if (HDC dc = GetWindowDC(h))
        {
            FillRect(dc, &line, theme_brush(theme_color::window));
            ReleaseDC(h, dc);
        }
        return true;
    }
    }
    return false;
}

}  // namespace rap::win32
