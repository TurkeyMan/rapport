// Windows' dark mode for native chrome: the app's menus, title bars, the
// common controls' dark visual styles and a dark menu bar. Windows
// documents only the title bar's (DWMWA_USE_IMMERSIVE_DARK_MODE); the rest
// is what Explorer, Notepad and the Windows Terminal use: uxtheme's exports
// by ordinal, and the DarkMode_* visual style classes. Each is looked for
// before use and left alone where missing, which leaves light chrome.
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
LRESULT CALLBACK dark_edge_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg != WM_NCPAINT || !dark_mode() ||
        !(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_CLIENTEDGE))
        return DefSubclassProc(h, msg, wp, lp);
    const LRESULT r = DefSubclassProc(h, msg, wp, lp);
    if (HDC dc = GetWindowDC(h))
    {
        RECT w;
        GetWindowRect(h, &w);
        OffsetRect(&w, -w.left, -w.top);
        FrameRect(dc, &w, theme_brush(theme_color::border));
        InflateRect(&w, -1, -1);
        FrameRect(dc, &w, theme_brush(theme_color::base));
        ReleaseDC(h, dc);
    }
    return r;
}

// A separator (an etched static) in dark mode: its etched edge is drawn
// white whatever WM_CTLCOLORSTATIC says, so the line is drawn here, a
// pixel of the divider colour across its middle.
LRESULT CALLBACK dark_separator_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (!dark_mode() || (msg != WM_PAINT && msg != WM_ERASEBKGND))
        return DefSubclassProc(h, msg, wp, lp);
    if (msg == WM_ERASEBKGND) return 1;
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT c;
    GetClientRect(h, &c);
    FillRect(dc, &c, theme_brush(theme_color::window));
    const bool vertical = (GetWindowLongPtrW(h, GWL_STYLE) & SS_TYPEMASK) == SS_ETCHEDVERT;
    RECT line = c;
    if (vertical)
    {
        line.left = (c.left + c.right) / 2;
        line.right = line.left + 1;
    }
    else
    {
        line.top = (c.top + c.bottom) / 2;
        line.bottom = line.top + 1;
    }
    FillRect(dc, &line, theme_brush(theme_color::divider));
    EndPaint(h, &ps);
    return 0;
}

// comctl32 opens the scroll bars of lists and trees with OpenNcThemeData,
// as "ScrollBar" whatever the window's style: in dark mode they'd be
// light (a light thumb line, a black segment). Its call is redirected
// here, to Explorer's (dark, for a window allowed dark mode), as the
// Windows Terminal and Notepad++ do.
using open_nc_theme_fn = HTHEME(WINAPI*)(HWND, LPCWSTR);
open_nc_theme_fn g_open_nc_theme = nullptr;

HTHEME WINAPI open_nc_theme(HWND h, LPCWSTR classes)
{
    if (dark_mode() && classes && !wcscmp(classes, L"ScrollBar"))
        return g_open_nc_theme(nullptr, L"Explorer::ScrollBar");
    return g_open_nc_theme(h, classes);
}

// The address in `module`'s delay-load table that its calls to `dll`'s
// export `ordinal` go through; nullptr without one.
void** delay_import(HMODULE module, const char* dll, WORD ordinal)
{
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;
    for (auto* d = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dir.VirtualAddress);
         d->DllNameRVA; ++d)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + d->DllNameRVA), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportNameTableRVA);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportAddressTableRVA);
        for (; names->u1.Ordinal; ++names, ++addresses)
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) &&
                IMAGE_ORDINAL(names->u1.Ordinal) == ordinal)
                return reinterpret_cast<void**>(&addresses->u1.Function);
    }
    return nullptr;
}

}  // namespace

void dark_scroll_bars()
{
    static bool done = false;
    if (done || windows_build() < 17763) return;
    done = true;
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE comctl = GetModuleHandleW(L"comctl32.dll");  // v6, under the activation context
    if (!uxtheme || !comctl) return;
    g_open_nc_theme = reinterpret_cast<open_nc_theme_fn>(
        reinterpret_cast<void*>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(49))));
    void** slot = g_open_nc_theme ? delay_import(comctl, "uxtheme.dll", 49) : nullptr;
    if (!slot) return;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return;
    *slot = reinterpret_cast<void*>(&open_nc_theme);
    VirtualProtect(slot, sizeof *slot, old, &old);
}

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
        // The system's link blue is unreadable on dark: in dark mode the
        // link takes its text colour from WM_CTLCOLORSTATIC (the link
        // role), as LIS_DEFAULTCOLORS asks.
        LITEM item{};
        item.mask = LIF_ITEMINDEX | LIF_STATE;
        item.iLink = 0;
        item.state = dark ? LIS_DEFAULTCOLORS : 0;
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
