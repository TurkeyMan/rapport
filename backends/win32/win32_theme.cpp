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
    // Menus follow the app mode: Default 0, AllowDark 1 (as the system
    // says), ForceDark 2, ForceLight 3. RAPPORT_THEME forces one; high
    // contrast keeps Windows' own. 1809's AllowDarkModeForApp takes a BOOL.
    const int forced = palette::forced_scheme();
    int mode = high_contrast() ? 0 : forced == 1 ? 2 : forced == 2 ? 3 : 1;
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
    else if (class_is(h, L"RICHEDIT50W"))
    {
        SetWindowTheme(h, explorer, nullptr);
        SendMessageW(h, EM_SETBKGNDCOLOR, 0, LPARAM(color_ref(theme_color_of(theme_color::base))));
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
