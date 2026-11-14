// A window's document tabs in its caption on Windows 11 (window_spec::tabs,
// stage 2), and where they stay as they were (stage 1, in the client area):
// before Windows 11, under a menu bar, in a dialog.
//
// On Windows 11 (build 22000 on): the title bar's gone, the client area from
// the window's top; the strip there, as tall as Explorer's (39 px at 100%),
// up to the caption buttons, its tabs as wide as Explorer's while they fit;
// the window's title still set. Hit tests: the strip's empty part, between
// two tabs and past "+" let the mouse through to the window, which says
// caption there and frame (to resize by) along the top; a tab, its close
// button and "+" are the strip's. rapport's own pixels, printed (as a pane's
// copy is) and, shown, on screen: the strip's empty part clear (alpha 0) for
// the backdrop, opaque in high contrast; the current tab the band's colour
// with its edge the divider's, one with the band under the strip (a toolbar
// band's: WinUI's LayerOnMicaBaseAltFillColorDefault at its own alpha, premultiplied, its
// title grey; on screen, that over the backdrop); the line
// between strip and band elsewhere, under the caption buttons too; WinUI's
// separator (TabViewItemSeparator) between two tabs neither current nor
// hovered, and only there. Its
// text: the titles in the UI font (Segoe UI Variable Text, else Segoe UI),
// as heavy as that face is, the current one in WinUI's TextFillColorPrimary,
// the others TextFillColorSecondary; ClearType on the current tab when the
// runner's own font smoothing is ClearType (read, never changed), grey over
// the backdrop, a hovered tab too (its fill WinUI's translucent
// LayerOnMicaBaseAltFillColorSecondary; the hover a WM_MOUSEMOVE sent to
// the strip, not input); "+" Segoe Fluent Icons' at 16 in
// TextFillColorPrimary, as Explorer's, the close button its glyph at 12 in
// TextFillColorSecondary; the rendering
// parameters read again on a WM_SETTINGCHANGE. The backdrop (22621 on):
// Mica Alt with the tabs in the caption, as Explorer's, none in high
// contrast, Mica once they leave it. UI
// Automation, from a client on a thread of its own: the Tab, its TabItems by
// name, Select, each Close tab and New tab invoked, each handler once; the
// window's native controls still reached; a tab dragged to the front (the
// strip sent the mouse's messages), ChildrenReordered raised on the Tab and
// its TabItems in the new order, on_reorder once. Light and dark (RAPPORT_THEME),
// on a base surface (--base); high contrast by running it under
// rapport-win32-high-contrast-run.
//
// --transitions, on a CI runner only (it changes system settings: needs
// GITHUB_ACTIONS=true and RAPPORT_TEST_SET_DPI=1 or
// RAPPORT_TEST_SET_HIGH_CONTRAST=1; skipped, 77, otherwise): the display's
// scale really changed (the window's DPI with it, not a WM_DPICHANGED sent),
// and high contrast turned on and off, with the window open: the strip laid
// out, coloured and its frame set anew each time, and everything restored.
//
// Before Windows 11: the tabs are stage 1's, a tab control in the client
// area under the title bar, and no strip.
//
// Needs no Mica: nothing reads the backdrop's pixels (Server 2025 has none).
// The window's shown without activating (RAPPORT_NO_ACTIVATE), and given no
// input but messages.
//   rapport-win32-caption-tabs-test [--base] [--transitions]
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which they need.
#include <commctrl.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <ole2.h>
#include <uiautomation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(                                                                                   \
    linker,                                                                                        \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{

std::mutex out_lock;
std::atomic<int> fails{0};

void expect(bool ok, const std::string& what)
{
    std::lock_guard g(out_lock);
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
}

void note(const std::string& what)
{
    std::lock_guard g(out_lock);
    std::printf("     %s\n", what.c_str());
    std::fflush(stdout);
}

std::string hex(DWORD v)
{
    char b[16];
    std::snprintf(b, sizeof b, "%06lx", v);
    return b;
}
std::string str(long v)
{
    return std::to_string(v);
}

void pump(int rounds = 20)
{
    for (int round = 0; round < rounds; ++round)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

DWORD windows_build()
{
    using fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto get =
        ntdll
            ? reinterpret_cast<fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
            : nullptr;
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    return get && get(&v) == 0 ? v.dwBuildNumber : 0;
}

bool high_contrast()
{
    HIGHCONTRASTW hc{sizeof hc};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) &&
           (hc.dwFlags & HCF_HIGHCONTRASTON);
}

std::string env(const char* name)
{
    char v[256]{};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    return n > 0 && n < sizeof v ? std::string(v, n) : std::string();
}
bool env_is(const char* name, const char* value)
{
    return env(name) == value;
}

// 0xRRGGBB, as pixels read.
DWORD rgb(COLORREF c)
{
    return (DWORD(GetRValue(c)) << 16) | (DWORD(GetGValue(c)) << 8) | GetBValue(c);
}

int px(float logical, UINT dpi)
{
    return int(std::lround(logical * float(dpi) / 96.f));
}

// -- what's expected ------------------------------------------------------

bool g_dark = false, g_base = false;

// WinUI's colours (#AARRGGBB), as the strip's drawn chrome has them
// (backends/win32/win32_chrome_palette.h): TextFillColorPrimary,
// TextFillColorSecondary, ControlStrokeColorSecondary.
struct argb
{
    DWORD rgb;  // 0xRRGGBB
    int a;
};
constexpr argb light_primary{0x000000, 0xE4}, light_secondary{0x000000, 0x9E}, light_stroke{0x000000, 0x29};
constexpr argb dark_primary{0xFFFFFF, 0xFF}, dark_secondary{0xFFFFFF, 0xC5}, dark_stroke{0xFFFFFF, 0x18};
// LayerOnMicaBaseAltFillColorSecondary: a tab not current, hovered.
constexpr argb light_hover{0x000000, 0x0A}, dark_hover{0xFFFFFF, 0x0F};
// DividerStrokeColorDefault: WinUI's TabViewItemSeparator, between two tabs.
constexpr argb light_divider{0x000000, 0x0F}, dark_divider{0xFFFFFF, 0x15};

int channel(DWORD v, int shift)
{
    return int((v >> shift) & 0xff);
}
// `top` over opaque `under`, 0xRRGGBB.
DWORD over(argb top, DWORD under)
{
    DWORD out = 0;
    for (int shift : {16, 8, 0})
        out |= DWORD((channel(top.rgb, shift) * top.a + channel(under, shift) * (255 - top.a) + 127) / 255) << shift;
    return out;
}

// The colours rapport's roles give, as win32-base-surface has them:
// Explorer's dark ones (backends/common/theme_palette.h), else the system
// colours; high contrast's own. The strip's line, text and glyphs WinUI's,
// the line drawn opaque over the band.
struct palette
{
    DWORD empty = 0;  // the strip's empty part, where it's opaque
    bool clear = true;
    DWORD band = 0, line = 0, current = 0;
    // The current tab's fill as printed, premultiplied ARGB: `current`,
    // opaque, or the toolbar band's translucent layer (current_opaque false).
    DWORD current_px = 0;
    bool current_opaque = true;
    // The current tab's title; "+" at rest; the current tab's close button;
    // another tab's title; a tab not current hovered (none in high contrast).
    argb title{}, glyph{}, close{}, other{}, hover{};
    argb add{};  // "+" at rest
    // The separator between two tabs neither current nor hovered, as
    // printed: premultiplied ARGB over the clear strip, else opaque.
    DWORD separator_px = 0;
};
palette expected(bool hc)
{
    palette p;
    if (hc)
    {
        p.clear = false;
        p.empty = rgb(GetSysColor(COLOR_WINDOW));
        p.band = rgb(GetSysColor(g_base ? COLOR_WINDOW : COLOR_BTNFACE));
        p.line = rgb(GetSysColor(COLOR_WINDOWTEXT));
        p.current = rgb(GetSysColor(COLOR_HIGHLIGHT));
        p.title = {rgb(GetSysColor(COLOR_HIGHLIGHTTEXT)), 255};
        p.glyph = p.add = {rgb(GetSysColor(COLOR_WINDOWTEXT)), 255};
        p.close = p.title;
        p.other = {rgb(GetSysColor(COLOR_WINDOWTEXT)), 255};
        p.current_px = 0xFF000000u | p.current;
        // WinUI's HighContrast TabViewItemSeparator: SystemColorWindowTextColor.
        p.separator_px = 0xFF000000u | rgb(GetSysColor(COLOR_WINDOWTEXT));
        return p;
    }
    p.band = g_base ? (g_dark ? 0x191919 : rgb(GetSysColor(COLOR_WINDOW)))
                    : (g_dark ? 0x202020 : rgb(GetSysColor(COLOR_BTNFACE)));
    p.line = over(g_dark ? dark_stroke : light_stroke, p.band);
    p.current = p.band;
    p.current_px = 0xFF000000u | p.current;
    p.title = g_dark ? dark_primary : light_primary;
    p.glyph = p.close = p.other = g_dark ? dark_secondary : light_secondary;
    // "+", as Explorer's: TextFillColorPrimary.
    p.add = g_dark ? dark_primary : light_primary;
    p.hover = g_dark ? dark_hover : light_hover;
    {
        const argb d = g_dark ? dark_divider : light_divider;
        DWORD premultiplied = DWORD(d.a) << 24;
        for (int shift : {16, 8, 0})
            premultiplied |= DWORD((channel(d.rgb, shift) * d.a + 127) / 255) << shift;
        p.separator_px = premultiplied;
    }
    return p;
}

// The toolbar band (Explorer's address row): WinUI's
// LayerOnMicaBaseAltFillColorDefault, #B3FFFFFF (dark #733A3A3A), at its own
// alpha over the extended frame, where the compositor puts Mica Alt under
// it, as Explorer's measures (#F0F5F9 and #F1F5F9 over a strip of #CEDCEC:
// alpha 0.70); high contrast's window colour, opaque; its line the strip's,
// opaque.
constexpr argb light_layer{0xFFFFFF, 0xB3}, dark_layer{0x3A3A3A, 0x73};
argb layer()
{
    return g_dark ? dark_layer : light_layer;
}
// Its premultiplied ARGB, as printed into a 32-bit bitmap, exactly:
// 0xB3B3B3B3, and 0x731A1A1A (0x3A * 0x73 / 255, 26.2).
constexpr DWORD light_layer_px = 0xB3B3B3B3, dark_layer_px = 0x731A1A1A;
// The band's fill as printed.
DWORD band_px(bool hc)
{
    return hc ? 0xFF000000u | rgb(GetSysColor(COLOR_WINDOW)) : g_dark ? dark_layer_px : light_layer_px;
}
// The same family's opaque one, where GDI paints it (a native control's
// background before its glass): LayerOnMicaBaseAltFillColorTertiary,
// #F9F9F9, dark #2C2C2C.
DWORD band_fill(bool hc)
{
    if (hc) return rgb(GetSysColor(COLOR_WINDOW));
    return g_dark ? 0x2C2C2C : 0xF9F9F9;
}
// Two pixels the same, each channel (alpha too) within `tolerance` levels.
bool near_px(DWORD a, DWORD b, int tolerance = 1)
{
    for (int shift : {24, 16, 8, 0})
        if (std::abs(channel(a, shift) - channel(b, shift)) > tolerance) return false;
    return true;
}
std::string argb_hex(DWORD v)
{
    char b[16];
    std::snprintf(b, sizeof b, "%08lx", v);
    return b;
}

// -- the windows ----------------------------------------------------------

bool g_shown = true;  // shown: the compositor's frame, and the screen's pixels

void show(rap::window& w)
{
    w.show();
}

HWND top_of(const char* title)
{
    return FindWindowW(nullptr, std::wstring(title, title + std::strlen(title)).c_str());
}

HWND strip_of(HWND top)
{
    return top ? FindWindowExW(top, nullptr, L"rapportCaptionTabs", nullptr) : nullptr;
}

// Stage 1's: the tab control with fixed widths.
HWND tab_control_of(HWND top)
{
    HWND found = nullptr;
    EnumChildWindows(
        top,
        [](HWND h, LPARAM p) -> BOOL
        {
            wchar_t name[32]{};
            GetClassNameW(h, name, 32);
            if (_wcsicmp(name, WC_TABCONTROLW) || !(GetWindowLongPtrW(h, GWL_STYLE) & TCS_FIXEDWIDTH))
                return TRUE;
            *reinterpret_cast<HWND*>(p) = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// The strip's own account of itself (caption_tabs::query_message).
UINT query()
{
    static const UINT m = RegisterWindowMessageW(L"rapport-caption-tabs");
    return m;
}
int ask(HWND strip, int what, int index = 0)
{
    return int(SendMessageW(strip, query(), MAKEWPARAM(what, index), 0));
}
RECT part(HWND strip, int what, int index = 0)
{
    RECT r{};
    SendMessageW(strip, query(), MAKEWPARAM(what, index), reinterpret_cast<LPARAM>(&r));
    return r;
}
enum
{
    q_count = 0,
    q_current = 1,
    q_focus = 2,
    q_tab = 3,
    q_close = 4,
    q_add = 5,
    q_params_made = 6,
    q_fonts = 7,
    q_fail_copy = 8
};
// What q_fonts fills (caption_tabs_fonts in win32_caption_tabs.h).
struct strip_fonts
{
    wchar_t text[64];
    wchar_t glyphs[64];
    float text_size, glyph_size;
    int text_weight;
    int cleartype;
    float add_size;
};

// The title bar's height: the client area's top below the window's.
long title_bar(HWND top)
{
    RECT w;
    GetWindowRect(top, &w);
    POINT o{0, 0};
    ClientToScreen(top, &o);
    return o.y - w.top;
}

// A point of `from`'s client area hit-tested by `h` (WM_NCHITTEST).
LRESULT hit(HWND h, HWND from, POINT p)
{
    ClientToScreen(from, &p);
    return SendMessageW(h, WM_NCHITTEST, 0, MAKELPARAM(short(p.x), short(p.y)));
}

POINT centre(const RECT& r)
{
    return {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
}

// The strip printed (WM_PRINTCLIENT) into a 32-bit bitmap cleared to zero:
// its pixels, alpha and all.
struct printed
{
    std::vector<DWORD> px;
    int w = 0, h = 0;
    DWORD at(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0xDEADBEEF;
        return px[size_t(y) * w + x];
    }
};
printed print(HWND strip)
{
    printed out;
    RECT c;
    GetClientRect(strip, &c);
    out.w = c.right;
    out.h = c.bottom;
    if (out.w <= 0 || out.h <= 0) return out;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), out.w, -out.h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    std::memset(bits, 0, size_t(out.w) * out.h * 4);
    SendMessageW(strip, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT);
    GdiFlush();
    out.px.assign(static_cast<DWORD*>(bits), static_cast<DWORD*>(bits) + size_t(out.w) * out.h);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return out;
}

// A pixel of `h`'s client area as on the screen.
DWORD on_screen(HWND h, POINT p)
{
    ClientToScreen(h, &p);
    HDC screen = GetDC(nullptr);
    const COLORREF c = GetPixel(screen, p.x, p.y);
    ReleaseDC(nullptr, screen);
    return c == CLR_INVALID ? 0xDEADBEEF : rgb(c);
}

// The strip as it should be laid out at the window's DPI: where it is, how
// tall, the tabs' widths, the caption buttons left alone; `when` says when.
void check_layout(HWND top, HWND strip, const std::string& when)
{
    const UINT dpi = GetDpiForWindow(top);
    const std::string at = " (" + when + ", " + str(long(dpi)) + " dpi)";
    RECT s;
    GetWindowRect(strip, &s);
    MapWindowPoints(nullptr, top, reinterpret_cast<POINT*>(&s), 2);
    const int height = px(39, dpi);
    // Maximised, the client area's top is the window's, past the monitor's
    // by the frame's thickness: the strip starts that far down, at the
    // monitor's top.
    const int down = IsZoomed(top) ? GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                                         GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi)
                                   : 0;
    expect(s.left == 0 && s.top == down && s.bottom - s.top == height,
           "the strip at the client area's left, " + str(down) + " down, " + str(height) + " tall" + at + ": " +
               str(s.left) + "," + str(s.top) + " " + str(s.bottom - s.top) + " tall");
    expect(title_bar(top) == 0, "no title bar: the client area from the window's top" + at + " (" +
                                    str(title_bar(top)) + ")");
    if (IsZoomed(top))
    {
        RECT on;
        GetWindowRect(strip, &on);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(top, MONITOR_DEFAULTTONEAREST), &mi);
        expect(on.top == mi.rcWork.top, "maximised, the strip at the work area's top" + at + ": " +
                                            str(long(on.top)) + ", the work area's " + str(long(mi.rcWork.top)));
    }
    // The caption buttons, from the compositor once shown.
    RECT b{};
    if (SUCCEEDED(DwmGetWindowAttribute(top, DWMWA_CAPTION_BUTTON_BOUNDS, &b, sizeof b)) && b.right > b.left)
    {
        RECT w;
        POINT o{0, 0};
        GetWindowRect(top, &w);
        ClientToScreen(top, &o);
        OffsetRect(&b, w.left - o.x, w.top - o.y);
        expect(s.right <= b.left, "the strip ends before the caption buttons" + at + ": at " + str(s.right) +
                                      ", the buttons from " + str(b.left));
    }
    else
        expect(!g_shown, "the caption buttons' bounds known, the window shown" + at);
    const int n = ask(strip, q_count);
    const int available = (s.right - s.left) - px(8, dpi) - px(8, dpi) - px(28, dpi) - px(4, dpi);
    const int want = std::clamp(available / std::max(1, n), px(80, dpi), px(240, dpi));
    for (int i = 0; i < n; ++i)
    {
        const RECT r = part(strip, q_tab, i);
        if ((i + 1) * want > available) break;  // cut at the row's end
        expect(r.right - r.left == want && r.top == px(8, dpi) && r.bottom == height &&
                   r.left == px(8, dpi) + i * want,
               "tab " + str(i) + " " + str(want) + " wide, from " + str(px(8, dpi) + i * want) + ", " +
                   str(px(8, dpi)) + " down" + at + ": " + str(r.left) + ".." + str(r.right) + ", " +
                   str(r.top) + ".." + str(r.bottom));
    }
}

// rapport's own pixels of the strip, printed: `pal` as expected.
// The middle of the strip's empty part past "+", which layout may leave as
// little as 8 logical px: -1 (a failed check) when there's none to sample.
int past_add(HWND strip, const std::string& when, bool check = true)
{
    RECT c;
    GetClientRect(strip, &c);
    const RECT add = part(strip, q_add);
    const int x = int(add.right + c.right) / 2;
    const bool room = c.right - add.right >= 2 && x > add.right && x < c.right;
    if (check) expect(room, "an empty part past \"+\" to sample, " + str(long(add.right)) + ".." + str(long(c.right)) + " (" +
                     when + ")");
    return room ? x : -1;
}

void check_pixels(HWND strip, const palette& pal, const std::string& when)
{
    const UINT dpi = GetDpiForWindow(strip);
    const printed p = print(strip);
    const int current = ask(strip, q_current);
    const RECT tab = part(strip, q_tab, current);
    const int h = p.h, line = std::max(1, px(1, dpi)), mid = (tab.top + h - line) / 2;
    const std::string at = " (" + when + ")";
    auto colour = [](DWORD v) { return v & 0xffffff; };
    auto alpha = [](DWORD v) { return v >> 24; };
    auto is = [&](DWORD v, DWORD want) { return alpha(v) == 255 && colour(v) == want; };
    // The empty part: above the tabs, and past "+".
    const int past = past_add(strip, when);
    for (const POINT e : {POINT{tab.left + px(24, dpi), px(3, dpi)}, POINT{past, h / 2}})
    {
        if (e.x < 0) continue;
        const DWORD v = p.at(e.x, e.y);
        if (pal.clear)
            expect(alpha(v) == 0, "the strip's empty part is clear for the backdrop at " + str(e.x) + "," +
                                      str(e.y) + at + ": " + hex(v) + " alpha " + str(long(alpha(v))));
        else
            expect(is(v, pal.empty), "the strip's empty part is " + hex(pal.empty) + ", opaque, at " + str(e.x) +
                                         "," + str(e.y) + at + ": " + hex(colour(v)) + " alpha " +
                                         str(long(alpha(v))));
    }
    // The current tab: filled, to the bottom row (one with the band under
    // it, no line), its sides the divider's. Over the toolbar band, its
    // layer at its own alpha (premultiplied), translucent.
    const DWORD inside = p.at(tab.left + px(4, dpi), mid), bottom = p.at(tab.left + px(4, dpi), h - 1);
    const std::string fill_is = pal.current_opaque ? hex(pal.current) + ", opaque"
                                                   : argb_hex(pal.current_px) + " (premultiplied ARGB), translucent";
    expect(inside == pal.current_px, "the current tab is " + fill_is + at + ": " + argb_hex(inside));
    expect(bottom == pal.current_px, "and so is its bottom row, where the line isn't" + at + ": " +
                                                argb_hex(bottom));
    if (pal.current == pal.band || !pal.current_opaque)
    {
        const DWORD side = p.at(tab.left, mid);
        expect(is(side, pal.line), "its sides are the divider's, " + hex(pal.line) + ", opaque" + at + ": " +
                                       argb_hex(side));
    }
    // The line along the bottom elsewhere: past "+", under another tab.
    for (const int x : {past, current == 0 ? int(part(strip, q_tab, 1).left) + px(20, dpi) : px(16, dpi)})
    {
        if (x < 0) continue;
        const DWORD v = p.at(x, h - 1);
        expect(is(v, pal.line), "the line between strip and band, " + hex(pal.line) + ", at " + str(x) + at +
                                    ": " + hex(colour(v)) + " alpha " + str(long(alpha(v))));
    }
    // The title, drawn on it.
    int ink = 0;
    for (int y = mid - px(5, dpi); y <= mid + px(5, dpi); ++y)
        for (int x = tab.left + px(12, dpi); x < tab.left + px(120, dpi); ++x)
            if (!near_px(p.at(x, y), pal.current_px, 0)) ++ink;
    expect(ink > 10, "the current tab's title is drawn" + at + " (" + str(ink) + " pixels)");
}

// The separators between tabs, as WinUI's TabView has them
// (TabViewItemSeparator): a line's width at the boundary of two tabs (the
// left one's last column), 8 logical px short of the tab's top and bottom
// (TabViewItemSeparatorMargin, 0,8,0,8), in DividerStrokeColorDefault
// (translucent, premultiplied over the clear strip; high contrast's
// SystemColorWindowTextColor); none beside the current tab, nor beside a
// hovered one (the mouse's move sent as a message, no input). Alpha is
// current: Beta|Gamma has one, Alpha|Beta none.
void check_separators(HWND strip, const palette& pal, const std::string& when)
{
    const UINT dpi = GetDpiForWindow(strip);
    const std::string at = " (" + when + ")";
    const int line = std::max(1, px(1, dpi));
    const RECT alpha = part(strip, q_tab, 0), beta = part(strip, q_tab, 1), gamma = part(strip, q_tab, 2);
    expect(ask(strip, q_current) == 0 && !IsRectEmpty(&gamma) && beta.right == gamma.left &&
               alpha.right == beta.left,
           "Alpha current, the three tabs side by side" + at);
    if (IsRectEmpty(&gamma)) return;
    const int top = beta.top + px(8, dpi), bottom = beta.bottom - px(8, dpi);
    // The rows of column x unlike those of `ref` (a column of the strip's
    // ground beside it), between the tab's top and the line.
    auto marked = [&](const printed& p, int x, int ref)
    {
        std::vector<int> rows;
        for (int y = beta.top; y < p.h - line; ++y)
            if (p.at(x, y) != p.at(ref, y)) rows.push_back(y);
        return rows;
    };
    auto span = [](const std::vector<int>& rows)
    {
        return rows.empty() ? std::string("none") : str(rows.front()) + ".." + str(rows.back());
    };
    {
        const printed p = print(strip);
        // Beta|Gamma: in Beta's last column(s), Gamma's first untouched.
        for (int x = beta.right - line; x < beta.right; ++x)
        {
            const std::vector<int> rows = marked(p, x, beta.right - px(4, dpi));
            const bool contiguous = !rows.empty() && rows.back() - rows.front() + 1 == int(rows.size());
            expect(contiguous && rows.front() == top && rows.back() == bottom - 1,
                   "a separator between Beta and Gamma at x " + str(x) + ", rows " + str(top) + ".." +
                       str(bottom - 1) + " (8 px in from the tab's top and bottom, scaled)" + at + ": " +
                       span(rows));
            for (int y : {top, (top + bottom) / 2, bottom - 1})
                expect(near_px(p.at(x, y), pal.separator_px),
                       "in DividerStrokeColorDefault, " + argb_hex(pal.separator_px) +
                           (pal.clear ? " (premultiplied, translucent)" : "") + ", at " + str(x) + "," + str(y) +
                           at + ": " + argb_hex(p.at(x, y)));
        }
        {
            const std::vector<int> rows = marked(p, gamma.left, gamma.left + px(4, dpi));
            expect(rows.empty(), "Gamma's first column past the separator untouched" + at + ": " + span(rows));
        }
        // Alpha|Beta: none beside the current tab; the columns there as they
        // are below the separator's rows (the current tab's side, or its
        // fill), give or take the corner's antialiasing (a level).
        for (int x = alpha.right - line; x < alpha.right + line; ++x)
        {
            int unlike = 0;
            for (int y = top; y < bottom; ++y)
                if (!near_px(p.at(x, y), p.at(x, bottom + px(2, dpi)), 2)) ++unlike;
            expect(unlike == 0, "no separator beside the current tab, between Alpha and Beta, at x " + str(x) + at +
                                    ": " + str(unlike) + " rows marked");
        }
    }
    // Hovered, Beta or Gamma: none between them.
    for (const bool first : {true, false})
    {
        const RECT& r = first ? beta : gamma;
        const char* name = first ? "Beta" : "Gamma";
        const POINT in{r.left + px(60, dpi), (r.top + beta.bottom) / 2};
        SendMessageW(strip, WM_MOUSEMOVE, 0, MAKELPARAM(in.x, in.y));
        const printed p = print(strip);  // before the leave TrackMouseEvent may post
        SendMessageW(strip, WM_MOUSELEAVE, 0, 0);
        pump(2);
        for (int x = beta.right - line; x < beta.right + line; ++x)
        {
            int unlike = 0;
            for (int y = top; y < bottom; ++y)
                if (!near_px(p.at(x, y), p.at(x, beta.top + px(2, dpi)), 2)) ++unlike;
            expect(unlike == 0, std::string("no separator between Beta and Gamma, ") + name + " hovered, at x " +
                                    str(x) + at + ": " + str(unlike) + " rows marked");
        }
    }
}

// -- the toolbar band -------------------------------------------------------

void check_text(HWND strip, const palette& pal, const std::string& when);

// The window's background as its erase paints it (WM_ERASEBKGND into a
// bitmap, under its controls too): the band and its line, or none.
printed print_erase(HWND top)
{
    printed out;
    RECT c;
    GetClientRect(top, &c);
    out.w = c.right;
    out.h = c.bottom;
    if (out.w <= 0 || out.h <= 0) return out;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), out.w, -out.h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    std::fill_n(static_cast<DWORD*>(bits), size_t(out.w) * out.h, 0x00ABCDEFu);
    SendMessageW(top, WM_ERASEBKGND, reinterpret_cast<WPARAM>(mem), 0);
    GdiFlush();
    out.px.assign(static_cast<DWORD*>(bits), static_cast<DWORD*>(bits) + size_t(out.w) * out.h);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return out;
}

// A child's rectangle in `top`'s client area.
RECT rect_in(HWND top, HWND child)
{
    RECT r{};
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, top, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

// A child control's client area printed (WM_PRINTCLIENT, its background
// too) into a 32-bit bitmap.
printed print_control(HWND h)
{
    printed out;
    RECT c;
    GetClientRect(h, &c);
    out.w = c.right;
    out.h = c.bottom;
    if (out.w <= 0 || out.h <= 0) return out;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), out.w, -out.h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    std::fill_n(static_cast<DWORD*>(bits), size_t(out.w) * out.h, 0x00ABCDEFu);
    SendMessageW(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT | PRF_ERASEBKGND);
    GdiFlush();
    out.px.assign(static_cast<DWORD*>(bits), static_cast<DWORD*>(bits) + size_t(out.w) * out.h);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return out;
}

// Down the window's background from `from` (its client y) near its right
// edge: with a band, its fill from `from` to the toolbar's bottom (at
// least), then the line, a pixel scaled, then the surface; without one,
// the surface all the way, no fill and no line.
void check_band_rows(HWND top, int from, bool band, const palette& pal, HWND toolbar, const std::string& when)
{
    const UINT dpi = GetDpiForWindow(top);
    const std::string at = " (" + when + ")";
    const printed p = print_erase(top);
    const int x = p.w - px(4, dpi), line = std::max(1, px(1, dpi));
    auto colour = [&](int y) { return p.at(x, y) & 0xffffff; };
    if (!band)
    {
        int y = from, last = std::min(p.h, from + px(120, dpi));
        while (y < last && colour(y) == pal.band) ++y;
        expect(y == last, "no band: the window's background is " + hex(pal.band) + " from " + str(from) +
                              " down, no fill and no line" + at +
                              (y < last ? ": " + hex(colour(y)) + " at " + str(y) : std::string()));
        return;
    }
    // Its fill at the layer's alpha, premultiplied (opaque in high
    // contrast); the line opaque.
    const DWORD fill = band_px(high_contrast()), line_px = 0xFF000000u | pal.line;
    int y = from;
    while (y < p.h && p.at(x, y) == fill) ++y;
    const int fill_end = y;
    const RECT tb = toolbar ? rect_in(top, toolbar) : RECT{};
    expect(fill_end > from && (!toolbar || (fill_end >= tb.bottom && fill_end <= tb.bottom + px(8, dpi))),
           "the band " + argb_hex(fill) + " (premultiplied ARGB) from the strip's bottom, " + str(from) +
               ", to the toolbar's, " + str(long(tb.bottom)) + at + ": to " + str(fill_end) + ", then " +
               argb_hex(p.at(x, fill_end)));
    while (y < p.h && p.at(x, y) == line_px) ++y;
    expect(y - fill_end == line, "and under it the line, " + hex(pal.line) + ", opaque, " + str(line) + " px" + at +
                                     ": " + str(y - fill_end) + " px");
    expect(colour(y) == pal.band, "then the window's surface, " + hex(pal.band) + at + ": " + hex(colour(y)) +
                                      " at " + str(y));
    // Across the window, edge to edge.
    for (const int across : {0, p.w / 2, p.w - 1})
        expect(p.at(across, from) == fill && p.at(across, fill_end) == line_px,
               "the band and its line at x " + str(across) + at + ": " + argb_hex(p.at(across, from)) + ", " +
                   argb_hex(p.at(across, fill_end)));
}

// The child of `top` with the WS_VISIBLE style whose rectangle holds `p`
// (client coordinates), null if none: what would cover the window's own
// pixel there. By style, so it says the same of a window never shown, whose
// children IsWindowVisible (and ChildWindowFromPointEx's CWP_SKIPINVISIBLE)
// would all count out.
HWND covering_child(HWND top, POINT p)
{
    for (HWND c = GetWindow(top, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
    {
        if (!(GetWindowLongPtrW(c, GWL_STYLE) & WS_VISIBLE)) continue;
        const RECT r = rect_in(top, c);
        if (PtInRect(&r, p)) return c;
    }
    return nullptr;
}
// `p` is the window's own pixel: no child over it, by style and, shown,
// as the system finds it (ChildWindowFromPointEx).
bool window_own(HWND top, POINT p, const std::string& what)
{
    HWND by_style = covering_child(top, p);
    HWND found = IsWindowVisible(top) ? ChildWindowFromPointEx(top, p, CWP_SKIPINVISIBLE) : top;
    const bool own = !by_style && found == top;
    expect(own, what + " at " + str(p.x) + "," + str(p.y) + " is the window's own pixel, no child over it" +
                    (own ? std::string() : ": covered"));
    return own;
}

// A point in the band's fill that no child covers, as near its right end as
// there is one (the toolbar's controls fill most of it): between `from`
// (the strip's bottom) and the line, read from the printed background.
// {-1, -1} if there's none.
POINT band_point(HWND top, int from)
{
    const printed p = print_erase(top);
    const DWORD fill = band_px(high_contrast());
    for (int y = from; y < p.h && near_px(p.at(0, y), fill); ++y)
        for (int x = p.w - 1; x >= 0; --x)
            if (near_px(p.at(x, y), fill) && !covering_child(top, POINT{x, y})) return POINT{x, y};
    return POINT{-1, -1};
}

// A window whose first content item is a toolbar across it: a band where
// `band` says so (Windows 11, the tabs in the caption, a base surface),
// the current tab its fill, the toolbar's own background too.
void check_band(HWND top, bool band, const std::string& when)
{
    const bool hc = high_contrast();
    const std::string at = " (" + when + ")";
    HWND strip = strip_of(top);
    HWND toolbar = FindWindowExW(top, nullptr, TOOLBARCLASSNAMEW, nullptr);
    expect(toolbar != nullptr, "the toolbar's native control" + at);
    palette pal = expected(hc);
    // On the band, the current tab its layer, translucent.
    if (band && !hc)
    {
        pal.current = band_fill(false);
        pal.current_px = band_px(false);
        pal.current_opaque = false;
    }
    const int from = strip ? int(rect_in(top, strip).bottom) : 0;
    check_band_rows(top, from, band, pal, toolbar, when);
    if (band)
    {
        // Where the band's on screen to be sampled (see main): uncovered.
        const POINT b = band_point(top, from);
        expect(b.x >= 0, "a point of the band's fill no child covers" + at + ": " + str(b.x) + "," + str(b.y));
        if (b.x >= 0) window_own(top, b, "that point" + at);
    }
    if (!strip) return;
    check_pixels(strip, pal, when);
    check_text(strip, pal, when);
    // The current tab dragged (the strip sent the mouse's messages, then
    // Escape: no reorder), drawn under the pointer: still the band's fill.
    {
        const UINT dpi = GetDpiForWindow(strip);
        const int current = ask(strip, q_current);
        const RECT tab = part(strip, q_tab, current);
        const POINT a = centre(tab);
        const int moved = px(40, dpi);
        SendMessageW(strip, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(a.x, a.y));
        SendMessageW(strip, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(a.x + moved, a.y));
        const printed p = print(strip);
        SendMessageW(strip, WM_KEYDOWN, VK_ESCAPE, 0);
        pump(2);
        const int line = std::max(1, px(1, dpi)), mid = (tab.top + p.h - line) / 2;
        const DWORD inside = p.at(tab.left + moved + px(4, dpi), mid),
                    bottom = p.at(tab.left + moved + px(4, dpi), p.h - 1);
        expect(inside == pal.current_px && bottom == pal.current_px,
               "the current tab dragged " + str(moved) + " px is still " + argb_hex(pal.current_px) +
                   ", to its bottom row" + at + ": " + argb_hex(inside) + ", " + argb_hex(bottom));
        expect(ask(strip, q_current) == current && part(strip, q_tab, current).left == tab.left,
               "and put back by Escape" + at);
    }
    // Dragged part way over the next tab (Beta, not far enough to swap them),
    // the current tab hides Beta's icon: its translucent fill over the
    // strip's ground, not over that icon.
    {
        const UINT dpi = GetDpiForWindow(strip);
        const int current = ask(strip, q_current);
        const RECT tab = part(strip, q_tab, current), next = part(strip, q_tab, current + 1);
        const int w = tab.right - tab.left, icon = px(16, dpi);
        // Beta's icon, where the strip draws it: inset, centred down the tab.
        const int line = std::max(1, px(1, dpi)), il = next.left + px(12, dpi);
        const printed before = print(strip);
        const int it = (next.top + before.h - line - icon) / 2;
        // Under the dragged tab, left of its close button (24 px, 6 in).
        const int moved = il + icon - (tab.right - px(6 + 24 + 8, dpi));
        expect(moved > 0 && moved < w / 2, "a drag of " + str(moved) + " px, less than half the tab's " + str(w) +
                                               " (no swap)" + at);
        const POINT a = centre(tab);
        SendMessageW(strip, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(a.x, a.y));
        SendMessageW(strip, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(a.x + moved, a.y));
        const printed p = print(strip);
        SendMessageW(strip, WM_KEYDOWN, VK_ESCAPE, 0);
        pump(2);
        const DWORD ground = before.at(next.left + px(4, dpi), it);
        int drawn = 0, shown = 0;
        for (int y = it; y < it + icon; ++y)
            for (int x = il; x < il + icon; ++x)
            {
                drawn += before.at(x, y) != ground;
                shown += p.at(x, y) != pal.current_px;
            }
        expect(drawn > 0, "Beta's icon drawn where it's looked for" + at + ": " + str(drawn) + " px");
        expect(shown == 0, "the current tab dragged " + str(moved) + " px over Beta's icon hides it" + at + ": " +
                               str(shown) + " of its pixels not " + argb_hex(pal.current_px));
        expect(ask(strip, q_current) == current, "and put back by Escape, over Beta" + at);
    }
    if (band && toolbar)
    {
        // Printed as on screen: on glass, the band's layer (premultiplied)
        // where it draws nothing; in high contrast, GDI's, opaque.
        const printed t = print_control(toolbar);
        const DWORD v = t.at(1, 1);
        if (hc)
            expect((v & 0xffffff) == band_fill(hc), "the toolbar's background is the band's, " + hex(band_fill(hc)) +
                                                        at + ": " + hex(v & 0xffffff));
        else
            expect(near_px(v, band_px(false)), "the toolbar's background is the band's layer, " +
                                                   argb_hex(band_px(false)) + at + ": " + argb_hex(v));
    }
}

// -- the band's native controls on glass -----------------------------------

// A control of the band printed (WM_PRINTCLIENT, as a pane's copy is, its
// glass the same as on screen) in the state `state` says: a layer control
// (`layer`) none of its pixels below the layer's alpha, each pixel at that
// alpha the layer's own (its background, nothing of its content lost), and
// something drawn; an opaque one (a field) opaque throughout.
void check_glass_print(HWND h, bool layer, const std::string& what, const std::string& state)
{
    const printed p = print_control(h);
    const DWORD bg = band_px(false);
    const int la = int(bg >> 24);
    long below = 0, lost = 0, drawn = 0, transparent = 0;
    std::string first_below, first_lost;
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x)
        {
            const DWORD v = p.at(x, y);
            const int a = int(v >> 24);
            if (a == 0) ++transparent;
            if (!near_px(v, bg, 2)) ++drawn;
            if (layer)
            {
                if (a < la - 1 && below++ == 0) first_below = str(x) + "," + str(y) + " " + argb_hex(v);
                if (std::abs(a - la) <= 1 && !near_px(v, bg, 2) && lost++ == 0)
                    first_lost = str(x) + "," + str(y) + " " + argb_hex(v);
            }
            else if (a != 255 && below++ == 0)
                first_below = str(x) + "," + str(y) + " " + argb_hex(v);
        }
    const std::string at = what + ", " + state + " (" + str(p.w) + "x" + str(p.h) + ")";
    expect(p.w > 0 && p.h > 0 && transparent == 0, "no pixel at alpha 0: " + at + " (" + str(transparent) + ")");
    if (layer)
    {
        expect(below == 0, "none below the layer's alpha, " + str(la) + ": " + at +
                               (below ? " (" + str(below) + ", first " + first_below + ")" : std::string()));
        expect(lost == 0, "each at the layer's alpha is the layer, " + argb_hex(bg) + ": " + at +
                              (lost ? " (" + str(lost) + ", first " + first_lost + ")" : std::string()));
    }
    else
        expect(below == 0, "opaque throughout: " + at +
                               (below ? " (" + str(below) + " not, first " + first_below + ")" : std::string()));
    expect(drawn > 0, "and it drew something: " + at + " (" + str(drawn) + " pixels)");
}

std::wstring class_name(HWND h)
{
    wchar_t c[64]{};
    GetClassNameW(h, c, 64);
    return c;
}

// `h` lies wholly in the band's fill: from the strip's bottom to the line,
// as the window's background prints them.
void color_in_band(HWND top, HWND h, const char* what)
{
    const printed p = print_erase(top);
    const int from = strip_of(top) ? int(rect_in(top, strip_of(top)).bottom) : 0;
    int to = from;
    while (to < p.h && near_px(p.at(p.w - 1, to), band_px(false))) ++to;
    const RECT r = rect_in(top, h);
    expect(r.top >= from && r.bottom <= to && r.right > r.left,
           std::string("and wholly in the band's fill, ") + str(from) + ".." + str(to) + ": " + what + ", " +
               str(long(r.top)) + ".." + str(long(r.bottom)));
}

// The child of `top` of class `cls` whose text starts with `text` (null:
// any), the `skip`th such.
HWND child_of(HWND top, const wchar_t* cls, const wchar_t* text = nullptr, int skip = 0)
{
    for (HWND c = FindWindowExW(top, nullptr, cls, nullptr); c; c = FindWindowExW(top, c, cls, nullptr))
    {
        wchar_t t[64]{};
        GetWindowTextW(c, t, 64);
        if (text && std::wcsncmp(t, text, std::wcslen(text))) continue;
        if (skip-- == 0) return c;
    }
    return nullptr;
}

// Every kind of control a toolbar can hold, in the band, each printed at
// rest, hovered, pressed, focused and disabled (messages, no input).
void check_glass(HWND top)
{
    const bool hc = high_contrast();
    if (hc)
    {
        note("high contrast: the band's opaque and off the extended frame; its controls GDI's, as before");
        return;
    }
    struct control
    {
        const char* what;
        HWND h;
        bool layer;
    };
    HWND combo_list = nullptr, combo_edit = nullptr;
    for (HWND c = FindWindowExW(top, nullptr, L"ComboBox", nullptr); c; c = FindWindowExW(top, c, L"ComboBox", nullptr))
        ((GetWindowLongPtrW(c, GWL_STYLE) & 3) == CBS_DROPDOWNLIST ? combo_list : combo_edit) = c;
    HWND edit = FindWindowExW(top, nullptr, L"Edit", nullptr);
    const control controls[] = {
        {"the tool strip (a native toolbar)", FindWindowExW(top, nullptr, TOOLBARCLASSNAMEW, nullptr), true},
        {"a push button", child_of(top, L"Button", L"Go"), true},
        {"a menu button", child_of(top, L"Button", L"Menu"), true},
        {"a check box", child_of(top, L"Button", L"Check"), true},
        {"a tool button (owner-drawn)", child_of(top, L"Button", L"Tool"), true},
        {"a label", child_of(top, L"Static", L"Label"), true},
        {"a drop-down list", combo_list, true},
        {"a combo box (with an edit)", combo_edit, false},
        {"a text field", edit, false},
        {"a text field's clear button", edit ? FindWindowExW(edit, nullptr, L"Button", nullptr) : nullptr, false},
    };
    for (const control& c : controls)
    {
        expect(c.h != nullptr, std::string("in the band: ") + c.what);
        if (!c.h) continue;
        color_in_band(top, c.h, c.what);
        check_glass_print(c.h, c.layer, c.what, "at rest");
        // Hovered: the mouse's move sent (a toolbar's hot item set).
        RECT r;
        GetClientRect(c.h, &r);
        if (!std::wcscmp(class_name(c.h).c_str(), TOOLBARCLASSNAMEW))
            SendMessageW(c.h, TB_SETHOTITEM, 0, 0);
        else
            SendMessageW(c.h, WM_MOUSEMOVE, 0, MAKELPARAM(r.right / 2, r.bottom / 2));
        check_glass_print(c.h, c.layer, c.what, "hovered");
        if (!std::wcscmp(class_name(c.h).c_str(), TOOLBARCLASSNAMEW))
            SendMessageW(c.h, TB_SETHOTITEM, WPARAM(-1), 0);
        SendMessageW(c.h, WM_MOUSELEAVE, 0, 0);
        // Pressed: a button's state, a toolbar's first button.
        if (!std::wcscmp(class_name(c.h).c_str(), TOOLBARCLASSNAMEW))
        {
            TBBUTTON b{};
            SendMessageW(c.h, TB_GETBUTTON, 0, reinterpret_cast<LPARAM>(&b));
            SendMessageW(c.h, TB_PRESSBUTTON, WPARAM(b.idCommand), TRUE);
            check_glass_print(c.h, c.layer, c.what, "pressed");
            SendMessageW(c.h, TB_PRESSBUTTON, WPARAM(b.idCommand), FALSE);
        }
        else if (class_name(c.h) == L"Button")
        {
            SendMessageW(c.h, BM_SETSTATE, TRUE, 0);
            check_glass_print(c.h, c.layer, c.what, "pressed");
            SendMessageW(c.h, BM_SETSTATE, FALSE, 0);
        }
        // Focused, its focus rectangle shown.
        SendMessageW(c.h, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
        SendMessageW(c.h, WM_SETFOCUS, 0, 0);
        check_glass_print(c.h, c.layer, c.what, "focused");
        SendMessageW(c.h, WM_KILLFOCUS, 0, 0);
        // Disabled.
        EnableWindow(c.h, FALSE);
        check_glass_print(c.h, c.layer, c.what, "disabled");
        EnableWindow(c.h, TRUE);
    }
    // The text field's border, printed with its frame (WM_PRINT): the
    // theme's edit border drawn off the screen and put over the band's
    // layer, as on screen. None of it below the layer's alpha (GDI's would
    // be 0), its edge lines opaque. (That it never touches the window's own
    // DC on the way can't be watched from here: GDI's calls on another
    // window's DC can't be counted without hooking GDI itself. paint_border
    // only ever writes to the DC it's given once, through one buffer.)
    if (edit)
    {
        RECT w;
        GetWindowRect(edit, &w);
        const int ww = w.right - w.left, wh = w.bottom - w.top;
        POINT o{0, 0};
        ClientToScreen(edit, &o);
        RECT c;
        GetClientRect(edit, &c);
        OffsetRect(&c, o.x - w.left, o.y - w.top);
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), ww, -wh, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        std::fill_n(static_cast<DWORD*>(bits), size_t(ww) * wh, 0x00ABCDEFu);
        SendMessageW(edit, WM_PRINT, reinterpret_cast<WPARAM>(mem), PRF_NONCLIENT | PRF_CLIENT | PRF_ERASEBKGND);
        GdiFlush();
        const auto* px = static_cast<const DWORD*>(bits);
        const int la = int(band_px(false) >> 24);
        long ring = 0, below = 0, opaque = 0, translucent = 0;
        for (int y = 0; y < wh; ++y)
            for (int x = 0; x < ww; ++x)
            {
                if (PtInRect(&c, POINT{x, y})) continue;
                const int a = int(px[size_t(y) * ww + x] >> 24);
                ++ring;
                below += a < la - 1;
                opaque += a >= 250;
                translucent += a < 250;
            }
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        // (Light, its rounded corners are translucent; dark's DarkMode_CFD
        // border is square, opaque throughout.)
        expect(ring > 0 && below == 0 && opaque > 0 && (g_dark || translucent > 0),
               "the text field's border printed on glass: the theme's, over the layer (" + str(ring) + " pixels: " +
                   str(below) + " below its alpha, " + str(opaque) + " opaque, " + str(translucent) + " translucent)");
    }
    // A label's text grey on the layer (CONTRIBUTING.md), not ClearType.
    if (HWND label = child_of(top, L"Static", L"Label"))
    {
        const printed p = print_control(label);
        int chroma = 0;
        long ink = 0;
        for (const DWORD v : p.px)
        {
            const int hi = std::max({channel(v, 16), channel(v, 8), channel(v, 0)});
            const int lo = std::min({channel(v, 16), channel(v, 8), channel(v, 0)});
            chroma = std::max(chroma, hi - lo);
            if (!near_px(v, band_px(false), 2)) ++ink;
        }
        expect(ink > 10 && chroma <= 1, "the label's text on the band is grey, not ClearType: chroma " + str(chroma) +
                                            ", ink " + str(ink));
    }
    // Tooltips: top-level windows of their own, never in the band's frame.
    if (HWND bar = FindWindowExW(top, nullptr, TOOLBARCLASSNAMEW, nullptr))
        if (HWND tip = reinterpret_cast<HWND>(SendMessageW(bar, TB_GETTOOLTIPS, 0, 0)))
            expect(GetAncestor(tip, GA_ROOT) == tip && (GetWindowLongPtrW(tip, GWL_STYLE) & WS_POPUP),
                   "the toolbar's tooltip is a top-level popup, with its own surface");
}

// -- the text ---------------------------------------------------------------

IDWriteFactory* dwrite()
{
    static IDWriteFactory* f = []
    {
        IDWriteFactory* p = nullptr;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&p));
        return p;
    }();
    return f;
}
// DirectWrite's system fonts have a family of this name.
bool dwrite_has(const std::wstring& name)
{
    IDWriteFontCollection* fonts = nullptr;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (dwrite() && SUCCEEDED(dwrite()->GetSystemFontCollection(&fonts, FALSE)))
    {
        fonts->FindFamilyName(name.c_str(), &index, &exists);
        fonts->Release();
    }
    return exists != FALSE;
}

// The runner's own font smoothing: ClearType, or not. Read, never changed.
bool runner_cleartype()
{
    BOOL smoothing = FALSE;
    UINT type = 0;
    SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smoothing, 0);
    SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &type, 0);
    return smoothing && type == FE_FONTSMOOTHINGCLEARTYPE;
}

LOGFONTW message_font(UINT dpi)
{
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    return ncm.lfMessageFont;
}

std::string narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += c < 128 ? char(c) : '?';
    return s;
}

// How much ink `s` takes in `face` at `size` px, `weight`: DirectWrite's
// own coverage of its glyphs (a glyph run's alpha texture, before any
// gamma), summed in pixels. A yardstick for the strip's, in whichever
// family it has; 0 if the face can't be had.
double reference_ink(const std::wstring& face, float size, int weight, const wchar_t* s)
{
    IDWriteFontCollection* fonts = nullptr;
    IDWriteFontFamily* family = nullptr;
    IDWriteFont* font = nullptr;
    IDWriteFontFace* ff = nullptr;
    IDWriteGlyphRunAnalysis* analysis = nullptr;
    double mass = 0;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (dwrite() && SUCCEEDED(dwrite()->GetSystemFontCollection(&fonts, FALSE)) &&
        SUCCEEDED(fonts->FindFamilyName(face.c_str(), &index, &exists)) && exists &&
        SUCCEEDED(fonts->GetFontFamily(index, &family)) &&
        SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT(weight), DWRITE_FONT_STRETCH_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, &font)) &&
        SUCCEEDED(font->CreateFontFace(&ff)))
    {
        const UINT32 n = UINT32(wcslen(s));
        std::vector<UINT32> points(s, s + n);
        std::vector<UINT16> glyphs(n);
        std::vector<DWRITE_GLYPH_METRICS> metrics(n);
        std::vector<float> advances(n);
        DWRITE_FONT_METRICS fm{};
        ff->GetMetrics(&fm);
        ff->GetGlyphIndices(points.data(), n, glyphs.data());
        ff->GetDesignGlyphMetrics(glyphs.data(), n, metrics.data(), FALSE);
        for (UINT32 i = 0; i < n; ++i) advances[i] = metrics[i].advanceWidth * size / fm.designUnitsPerEm;
        DWRITE_GLYPH_RUN run{ff, size, n, glyphs.data(), advances.data(), nullptr, FALSE, 0};
        if (SUCCEEDED(dwrite()->CreateGlyphRunAnalysis(&run, 1.f, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                                                       DWRITE_MEASURING_MODE_NATURAL, 0.f, 0.f, &analysis)))
        {
            RECT b{};
            analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &b);
            std::vector<BYTE> alpha(size_t(b.right - b.left) * (b.bottom - b.top) * 3);
            if (!alpha.empty() &&
                SUCCEEDED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &b, alpha.data(),
                                                       UINT32(alpha.size()))))
                for (BYTE v : alpha) mass += v / (255.0 * 3);
        }
    }
    for (IUnknown* u : {static_cast<IUnknown*>(analysis), static_cast<IUnknown*>(ff), static_cast<IUnknown*>(font),
                        static_cast<IUnknown*>(family), static_cast<IUnknown*>(fonts)})
        if (u) u->Release();
    return mass;
}

// Ink in a rectangle of the printed strip. On an opaque `bg`, each
// channel's way from bg towards `ink` over it; where the strip is clear
// (bg < 0), its alpha against ink's.
struct ink_stats
{
    double mass = 0;     // coverage summed, in pixels
    double peak = 0;     // the most covered pixel's coverage
    int chroma = 0;      // the most a pixel's channels part from grey ink (levels)
    bool within = true;  // every channel between the ground and the ink (2 levels' grace)
    std::string worst;   // the first pixel that wasn't
};
ink_stats ink_in(const printed& p, RECT r, argb ink, long long bg)
{
    ink_stats s;
    const DWORD full = bg >= 0 ? over(ink, DWORD(bg)) : ink.rgb;
    for (int y = r.top; y < r.bottom; ++y)
        for (int x = r.left; x < r.right; ++x)
        {
            const DWORD v = p.at(x, y);
            if (v == 0xDEADBEEF) continue;
            const int a = int(v >> 24);
            double cov = 0;
            if (bg < 0)
            {
                // Premultiplied: grey ink's channels are all ink * alpha.
                cov = double(a) / ink.a;
                const int hi = std::max({channel(v, 16), channel(v, 8), channel(v, 0)});
                const int lo = std::min({channel(v, 16), channel(v, 8), channel(v, 0)});
                s.chroma = std::max(s.chroma, hi - lo);
                if (a > ink.a + 2 && s.within)
                {
                    s.within = false;
                    s.worst = str(x) + "," + str(y) + " alpha " + str(a);
                }
            }
            else
            {
                double sum = 0, lo = 2, hi = -1;
                int n = 0;
                for (int shift : {16, 8, 0})
                {
                    const int b = channel(DWORD(bg), shift), f = channel(full, shift), c = channel(v, shift);
                    if (c < std::min(b, f) - 2 || c > std::max(b, f) + 2)
                        if (s.within)
                        {
                            s.within = false;
                            s.worst = str(x) + "," + str(y) + " " + hex(v & 0xffffff);
                        }
                    if (std::abs(f - b) < 24) continue;
                    const double k = double(c - b) / (f - b);
                    sum += k;
                    lo = std::min(lo, k);
                    hi = std::max(hi, k);
                    ++n;
                }
                if (n == 0) continue;
                cov = sum / n;
                if (n > 1) s.chroma = std::max(s.chroma, int(std::lround((hi - lo) * 255 * 0.5)));
            }
            cov = std::clamp(cov, 0.0, 1.5);
            s.mass += cov;
            s.peak = std::max(s.peak, cov);
        }
    return s;
}

// Ink in a rectangle of the printed strip over a translucent fill, `bg` as
// printed (premultiplied ARGB): each channel's way, alpha too, from bg
// towards `ink` over it; its chroma the most a pixel's colour channels part
// (premultiplied grey is equal in all three).
ink_stats ink_on(const printed& p, RECT r, argb ink, DWORD bg)
{
    ink_stats s;
    // ink over bg, premultiplied.
    DWORD full = 0;
    full |= DWORD(ink.a + (channel(bg, 24) * (255 - ink.a) + 127) / 255) << 24;
    for (int shift : {16, 8, 0})
        full |= DWORD((channel(ink.rgb, shift) * ink.a + channel(bg, shift) * (255 - ink.a) + 127) / 255) << shift;
    for (int y = r.top; y < r.bottom; ++y)
        for (int x = r.left; x < r.right; ++x)
        {
            const DWORD v = p.at(x, y);
            if (v == 0xDEADBEEF) continue;
            for (int shift : {24, 16, 8, 0})
            {
                const int b = channel(bg, shift), f = channel(full, shift), c = channel(v, shift);
                if ((c < std::min(b, f) - 2 || c > std::max(b, f) + 2) && s.within)
                {
                    s.within = false;
                    s.worst = str(x) + "," + str(y) + " " + argb_hex(v);
                }
            }
            const int hi = std::max({channel(v, 16), channel(v, 8), channel(v, 0)});
            const int lo = std::min({channel(v, 16), channel(v, 8), channel(v, 0)});
            s.chroma = std::max(s.chroma, hi - lo);
            // The coverage from alpha, which "over" makes linear in it
            // whatever the colours (as for text over the clear strip).
            const int b = channel(bg, 24), f = channel(full, 24);
            if (f == b) continue;
            const double cov = std::clamp(double(channel(v, 24) - b) / (f - b), 0.0, 1.5);
            s.mass += cov;
            s.peak = std::max(s.peak, cov);
        }
    return s;
}

void check_text(HWND strip, const palette& pal, const std::string& when)
{
    const UINT dpi = GetDpiForWindow(strip);
    const std::string at = " (" + when + ")";
    // The faces: asked of the strip, checked against DirectWrite's fonts.
    strip_fonts f{};
    expect(SendMessageW(strip, query(), MAKEWPARAM(q_fonts, 0), reinterpret_cast<LPARAM>(&f)) != 0,
           "the strip tells its fonts" + at);
    const LOGFONTW mf = message_font(dpi);
    std::wstring want = mf.lfFaceName;
    if (!_wcsicmp(mf.lfFaceName, L"Segoe UI") && windows_build() >= 22000 && dwrite_has(L"Segoe UI Variable Text"))
        want = L"Segoe UI Variable Text";
    if (!dwrite_has(want)) want = L"Segoe UI";
    expect(f.text == want && dwrite_has(f.text),
           "the titles in " + narrow(want) + ", a family DirectWrite has" + at + ": " + narrow(f.text));
    const float size = float(mf.lfHeight < 0 ? -mf.lfHeight : mf.lfHeight);
    const int weight = mf.lfWeight ? int(mf.lfWeight) : 400;
    expect(std::fabs(f.text_size - size) < 0.01f && f.text_weight == weight,
           "at the message font's size and weight, " + str(long(size)) + " px " + str(weight) + at + ": " +
               str(std::lround(f.text_size)) + " px " + str(f.text_weight));
    const std::wstring icons = dwrite_has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    const float glyph = 12.f * float(dpi) / 96.f;
    expect(f.glyphs == icons && std::fabs(f.glyph_size - glyph) < 0.01f,
           "the glyphs in " + narrow(icons) + ", " + str(std::lround(glyph)) + " px (WinUI's 12)" + at + ": " +
               narrow(f.glyphs) + ", " + str(std::lround(f.glyph_size)));
    const bool cleartype = runner_cleartype();
    note(std::string("the runner's font smoothing: ") + (cleartype ? "ClearType" : "not ClearType"));
    expect((f.cleartype != 0) == cleartype, std::string("ClearType ") + (cleartype ? "on" : "off") +
                                                " in the strip, as the runner's setting" + at);

    const printed p = print(strip);
    const int current = ask(strip, q_current), line = std::max(1, px(1, dpi));
    const RECT tab = part(strip, q_tab, current), close = part(strip, q_close, current), add = part(strip, q_add);
    // The current tab's title (past its icon, before its close button), on
    // its fill.
    const RECT title{tab.left + px(36, dpi), tab.top + px(4, dpi), close.left - px(2, dpi), p.h - line};
    const ink_stats t =
        pal.current_opaque ? ink_in(p, title, pal.title, pal.current) : ink_on(p, title, pal.title, pal.current_px);
    // Either family's weight: the band spans both (and the user's face, if
    // that's what it is), with room for gamma and ClearType's contrast.
    std::vector<double> refs;
    for (const std::wstring face : {std::wstring(L"Segoe UI"), std::wstring(L"Segoe UI Variable Text"), want})
        if (const double m = reference_ink(face, size, weight, L"Alpha"); m > 0) refs.push_back(m);
    if (refs.empty()) refs.push_back(1);
    const double lo = *std::min_element(refs.begin(), refs.end()) * 0.8,
                 hi = *std::max_element(refs.begin(), refs.end()) * 1.3;
    expect(t.mass >= lo && t.mass <= hi, "the title's strokes as heavy as the face's at that weight, " +
                                             str(std::lround(lo)) + ".." + str(std::lround(hi)) + at + ": " +
                                             str(std::lround(t.mass)));
    expect(t.within && t.peak >= 0.6,
           "the title in " + hex(pal.title.rgb) + " at alpha " + str(pal.title.a) +
               " (TextFillColorPrimary) on the tab, never past it" + at + ": peak " +
               str(std::lround(t.peak * 100)) + "%" + (t.within ? "" : ", past at " + t.worst));
    // A monitor with no subpixel order (FLAT: a virtual display's, say) has
    // ClearType drawn without colour.
    DWRITE_PIXEL_GEOMETRY geometry = DWRITE_PIXEL_GEOMETRY_RGB;
    IDWriteRenderingParams* params = nullptr;
    if (dwrite() && SUCCEEDED(dwrite()->CreateMonitorRenderingParams(
                        MonitorFromWindow(strip, MONITOR_DEFAULTTONEAREST), &params)))
    {
        geometry = params->GetPixelGeometry();
        params->Release();
    }
    note(std::string("the monitor's pixel order: ") +
         (geometry == DWRITE_PIXEL_GEOMETRY_BGR ? "BGR" : geometry == DWRITE_PIXEL_GEOMETRY_RGB ? "RGB" : "flat"));
    if (!pal.current_opaque)
        // Its layer's translucent, Mica Alt under it the compositor's: grey,
        // ClearType or not (CONTRIBUTING.md).
        expect(t.chroma == 0, "the current tab's title is grey on the band's translucent layer, ClearType or not" +
                                  at + ": chroma " + str(t.chroma));
    else if (cleartype && geometry == DWRITE_PIXEL_GEOMETRY_FLAT)
        note("ClearType on a flat monitor: no colour to look for" + at);
    else if (cleartype)
        expect(t.chroma > 2, "the current tab's title is ClearType (its ink has colour)" + at + ": chroma " +
                                 str(t.chroma));
    else
        expect(t.chroma <= 1, "the current tab's title is grey, the runner's ClearType off" + at + ": chroma " +
                                  str(t.chroma));
    // Another tab's title, over the backdrop: grey, ClearType or not, in
    // TextFillColorSecondary (WinUI's TabViewItemHeaderForeground).
    const int other_i = current == 0 ? 1 : 0;
    const RECT other = part(strip, q_tab, other_i);
    // (Alpha and Beta have icons: the title after its icon.)
    const RECT other_box{other.left + px(12 + 16 + 8, dpi), other.top + px(4, dpi), other.left + px(120, dpi),
                         p.h - line};
    if (pal.clear)
    {
        const ink_stats o = ink_in(p, other_box, pal.other, -1);
        expect(o.mass > 10 && o.chroma == 0 && o.within && o.peak >= 0.6,
               "a title over the backdrop is grey (no chroma) in TextFillColorSecondary, alpha " +
                   str(pal.other.a) + at + ": chroma " + str(o.chroma) + ", ink " + str(std::lround(o.mass)) +
                   ", peak " + str(std::lround(o.peak * 100)) + "%" + (o.within ? "" : ", past at " + o.worst));
        // Hovered (the mouse's move sent to the strip as a message, no
        // input): WinUI's translucent LayerOnMicaBaseAltFillColorSecondary,
        // so its title stays grey rather than turning ClearType.
        const POINT in{other.left + px(60, dpi), (other.top + p.h) / 2};
        SendMessageW(strip, WM_MOUSEMOVE, 0, MAKELPARAM(in.x, in.y));
        const printed hp = print(strip);  // before the leave TrackMouseEvent may post
        SendMessageW(strip, WM_MOUSELEAVE, 0, 0);
        pump(2);
        const DWORD fill = hp.at(other.left + px(30, dpi), other.top + px(2, dpi));  // above its title
        const int fa = int(fill >> 24);
        const DWORD premultiplied = pal.hover.rgb ? 0x010101u * DWORD(fa) : 0;
        expect(std::abs(fa - pal.hover.a) <= 1 && (fill & 0xffffff) == premultiplied,
               "a tab hovered is filled with LayerOnMicaBaseAltFillColorSecondary, " + hex(pal.hover.rgb) +
                   " at alpha " + str(pal.hover.a) + ", translucent" + at + ": " + hex(fill & 0xffffff) + " alpha " +
                   str(fa));
        int chroma = 0;
        double mass = 0;
        for (int y = other_box.top; y < other_box.bottom; ++y)
            for (int x = other_box.left; x < other_box.right; ++x)
            {
                const DWORD v = hp.at(x, y);
                chroma = std::max(chroma, std::max({channel(v, 16), channel(v, 8), channel(v, 0)}) -
                                              std::min({channel(v, 16), channel(v, 8), channel(v, 0)}));
                mass += std::max(0, int(v >> 24) - fa) / double(pal.other.a);
            }
        expect(mass > 10 && chroma == 0, "and its title stays grey, not ClearType" + at + ": chroma " + str(chroma) +
                                             ", ink " + str(std::lround(mass)));
    }
    else
        note("no clear area to see grey text over" + at + ": the strip's opaque");

    // The glyphs: "+" as Explorer's, Segoe Fluent Icons' E710 at 16 (its
    // strokes 13 px across at 100%, where WinUI's 12 makes 10) in the
    // primary text colour; the current tab's close button WinUI's, E711 at
    // 12 in the secondary.
    const float add_size = 16.f * float(dpi) / 96.f;
    expect(std::fabs(f.add_size - add_size) < 0.01f,
           "\"+\" in " + narrow(icons) + " at " + str(std::lround(add_size)) + " px, Explorer's" + at + ": " +
               str(std::lround(f.add_size)));
    const double plus_ref = reference_ink(icons, add_size, 400, L""),
                 x_ref = reference_ink(icons, glyph, 400, L"");
    const ink_stats a = ink_in(p, add, pal.add, pal.clear ? -1 : (long long)pal.empty);
    expect(a.within && a.peak >= 0.6 && (!pal.clear || a.chroma == 0),
           "\"+\" in " + hex(pal.add.rgb) + " at alpha " + str(pal.add.a) + " (TextFillColorPrimary)" + at +
               ": peak " + str(std::lround(a.peak * 100)) + "%, chroma " + str(a.chroma) +
               (a.within ? "" : ", past at " + a.worst));
    expect(a.mass >= plus_ref * 0.8 && a.mass <= plus_ref * 1.5,
           "\"+\" as heavy as Segoe Fluent Icons' E710 at that size, about " + str(std::lround(plus_ref)) + at + ": " +
               str(std::lround(a.mass)));
    const ink_stats x =
        pal.current_opaque ? ink_in(p, close, pal.close, pal.current) : ink_on(p, close, pal.close, pal.current_px);
    expect(x.within && x.peak >= 0.6 && (pal.current_opaque || x.chroma == 0),
           "the close button's X in " + hex(pal.close.rgb) + " at alpha " + str(pal.close.a) + " on the tab" +
               (pal.current_opaque ? "" : ", grey") + at + ": peak " + str(std::lround(x.peak * 100)) +
               "%, chroma " + str(x.chroma) + (x.within ? "" : ", past at " + x.worst));
    // Over a translucent fill Direct2D blends grayscale text with what's
    // there, its gamma and contrast and all, where over the clear strip it
    // can't: light ink on the dark layer comes out heavier (measured about
    // 1.6 times the reference there, 1.2 times on the light one).
    const double x_most = pal.current_opaque ? 1.5 : 1.7;
    expect(x.mass >= x_ref * 0.8 && x.mass <= x_ref * x_most,
           "and as light as E711, about " + str(std::lround(x_ref)) + at + ": " + str(std::lround(x.mass)));
}

// The ClearType pass's first copy (from the strip's target into a run's own
// bitmap) made to fail, through the strip's test hook (query 8), for the
// first run, the current tab's title: nothing of that bitmap is copied
// back, its background never filled, and the title is drawn grayscale
// instead, on the tab's fill as ever.
void check_copy_failure(HWND strip, const palette& pal)
{
    strip_fonts f{};
    SendMessageW(strip, query(), MAKEWPARAM(q_fonts, 0), reinterpret_cast<LPARAM>(&f));
    if (!f.cleartype)
    {
        note("skipped the ClearType copy's failure: the runner's font smoothing isn't ClearType, so no copies");
        return;
    }
    const UINT dpi = GetDpiForWindow(strip);
    const printed normal = print(strip);
    const int before = ask(strip, q_fail_copy, 1);
    const printed p = print(strip);
    const int after = ask(strip, q_fail_copy, 0);
    expect(after == before + 1, "the hook made one ClearType copy fail (" + str(before) + " then " + str(after) + ")");
    const int current = ask(strip, q_current), line = std::max(1, px(1, dpi));
    const RECT tab = part(strip, q_tab, current), close = part(strip, q_close, current);
    // The title's rectangle on the tab, past its icon, before its close
    // button: what the run's bitmap covered.
    const RECT title{tab.left + px(36, dpi), tab.top + px(4, dpi), close.left - px(2, dpi), p.h - line};
    long opaque = 0, pixels = 0;
    for (int y = title.top; y < title.bottom; ++y)
        for (int x = title.left; x < title.right; ++x, ++pixels)
            if ((p.at(x, y) >> 24) == 255) ++opaque;
    expect(pixels > 0 && opaque == pixels, "the failed run's rectangle still opaque, the tab's fill (" +
                                               str(opaque) + " of " + str(pixels) + ")");
    const ink_stats t = ink_in(p, title, pal.title, pal.current);
    expect(t.within && t.mass > 10 && t.peak >= 0.6,
           "nothing copied back from the unfilled bitmap: only the tab's fill and the title's ink, " +
               hex(over(pal.title, pal.current)) + ", there: ink " + str(std::lround(t.mass)) + ", peak " +
               str(std::lround(t.peak * 100)) + "%" + (t.within ? "" : ", past at " + t.worst));
    expect(t.chroma <= 2, "the title drawn grayscale instead (chroma " + str(t.chroma) + ")");
    // Past the title, the rest of the strip as ever.
    long differ = 0;
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x)
        {
            const bool in = x >= title.left - px(2, dpi) && x < title.right && y >= tab.top && y < p.h - line;
            if (!in && p.at(x, y) != normal.at(x, y)) ++differ;
        }
    expect(differ == 0, "and everything else as printed without the failure (" + str(differ) + " pixels differ)");
    const printed again = print(strip);
    const ink_stats back = ink_in(again, title, pal.title, pal.current);
    expect(back.within && again.px == normal.px, "the next print, unhooked, ClearType as before");
}

// The strip printed into a DIB that's all `sentinel`, with `clip` (device
// coordinates; null: none) selected and the viewport's origin at `origin`:
// it draws only inside the clip, where the mapping puts it, the pixels
// everywhere else kept to the last bit (alpha too, which the ClearType
// pass mends straight in the bitmap).
void check_clipped_print(HWND strip, HRGN clip, POINT origin, const std::string& what)
{
    constexpr DWORD sentinel = 0x11223344;
    const printed ref = print(strip);
    const int w = ref.w, h = ref.h;
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    std::fill_n(static_cast<DWORD*>(bits), size_t(w) * h, sentinel);
    if (clip) SelectClipRgn(mem, clip);
    SetViewportOrgEx(mem, origin.x, origin.y, nullptr);
    SendMessageW(strip, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT);
    GdiFlush();
    const auto* px = static_cast<const DWORD*>(bits);
    long outside = 0, inside = 0, wrong_outside = 0, wrong_inside = 0;
    std::string first_out, first_in;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const DWORD v = px[size_t(y) * w + x];
            const bool drawn = (!clip || PtInRegion(clip, x, y)) && x >= origin.x && y >= origin.y;
            if (!drawn)
            {
                ++outside;
                if (v != sentinel && wrong_outside++ == 0)
                    first_out = str(x) + "," + str(y) + " " + hex(v);
            }
            else
            {
                ++inside;
                const DWORD want = ref.at(x - origin.x, y - origin.y);
                if (v != want && wrong_inside++ == 0)
                    first_in = str(x) + "," + str(y) + " " + hex(v) + " for " + hex(want);
            }
        }
    expect(wrong_outside == 0, "printed with " + what + ", every pixel outside it kept, RGBA (" + str(outside) +
                                   " pixels" + (wrong_outside ? ", " + str(wrong_outside) + " changed, first " +
                                                                    first_out
                                                              : std::string()) +
                                   ")");
    expect(inside > 0 && wrong_inside == 0, "and inside it as printed unclipped (" + str(inside) + " pixels" +
                                                (wrong_inside ? ", " + str(wrong_inside) + " differ, first " +
                                                                    first_in
                                                              : std::string()) +
                                                ")");
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void check_clipped_prints(HWND strip)
{
    RECT c;
    GetClientRect(strip, &c);
    const RECT tab = part(strip, q_tab, ask(strip, q_current));
    // Across the current tab's title (ClearType's rectangle) and its edge.
    HRGN partial = CreateRectRgn(tab.left + 30, 0, tab.left + 90, c.bottom);
    check_clipped_print(strip, partial, {0, 0}, "a rectangular clip, part of the current tab");
    // Two pieces apart, one through the title, one through the next tab's.
    HRGN two = CreateRectRgn(0, 0, tab.left + 50, c.bottom);
    HRGN second = CreateRectRgn(tab.left + 70, 10, tab.right + 60, c.bottom - 6);
    CombineRgn(two, two, second, RGN_OR);
    check_clipped_print(strip, two, {0, 0}, "a clip of two separate rectangles");
    // The viewport moved, and a clip (device coordinates) across the moved
    // title.
    HRGN moved = CreateRectRgn(tab.left + 60, 0, tab.left + 140, c.bottom);
    check_clipped_print(strip, moved, {40, 3}, "the viewport's origin at 40,3 and a clip");
    for (HRGN r : {partial, two, second, moved}) DeleteObject(r);
}

// A WM_SETTINGCHANGE (as the font smoothing's) has the strip read its
// rendering parameters again: sent to the window, no setting changed.
void check_rebuild(HWND top, HWND strip)
{
    print(strip);  // made, if they weren't
    const int before = ask(strip, q_params_made);
    SendMessageW(top, WM_SETTINGCHANGE, SPI_SETFONTSMOOTHING, 0);
    pump(2);
    print(strip);
    const int after = ask(strip, q_params_made);
    expect(before > 0 && after > before, "a WM_SETTINGCHANGE reads the rendering parameters again (made " +
                                             str(before) + " times, then " + str(after) + ")");
}

// The same where they're the window's (the band beside the strip) and on
// screen: shown only, and in front.
void check_screen(HWND top, HWND strip, const palette& pal)
{
    SetWindowPos(top, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
    RedrawWindow(top, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    pump();
    DwmFlush();
    const UINT dpi = GetDpiForWindow(top);
    const int h = px(39, dpi);
    const RECT tab = part(strip, q_tab, ask(strip, q_current));
    RECT c;
    GetClientRect(top, &c);
    const POINT under{tab.left + px(4, dpi), h - 1}, band{tab.left + px(4, dpi), h + px(2, dpi)},
        right{c.right - px(20, dpi), h - 1};
    // Each sample what it's said to be: the strip's (the current tab), and
    // the window's own (above the content's margin; under the caption
    // buttons), no control over either.
    expect(ChildWindowFromPointEx(top, under, CWP_SKIPINVISIBLE) == strip, "the tab's sample is the strip's");
    window_own(top, band, "the band's sample");
    window_own(top, right, "the line's sample");
    const DWORD u = on_screen(top, under), b = on_screen(top, band), r = on_screen(top, right);
    expect(b == pal.band, "on screen, the band under the strip is " + hex(pal.band) + ": " + hex(b));
    expect(u == pal.current, "the current tab's bottom row, one with it: " + hex(u));
    expect(r == pal.line, "the line goes on under the caption buttons, the window's: " + hex(r));
    SetWindowPos(top, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
}

void check_hits(HWND top, HWND strip, const std::string& when)
{
    const UINT dpi = GetDpiForWindow(top);
    const std::string at = " (" + when + ")";
    const RECT add = part(strip, q_add), tab1 = part(strip, q_tab, 1), close0 = part(strip, q_close, 0);
    const RECT tab0 = part(strip, q_tab, 0);
    const POINT empty{past_add(strip, when), px(20, dpi)}, between{tab1.right, px(20, dpi)};
    (void)add;
    if (empty.x >= 0)
        expect(hit(strip, strip, empty) == HTTRANSPARENT && hit(top, strip, empty) == HTCAPTION,
               "past \"+\": through the strip, the caption" + at);
    expect(ask(strip, q_current) != 1 && ask(strip, q_current) != 2, "(neither tab 1 nor 2 current)");
    expect(hit(strip, strip, between) == HTTRANSPARENT && hit(top, strip, between) == HTCAPTION,
           "between two tabs: the caption" + at);
    expect(hit(strip, strip, POINT{tab1.left + px(30, dpi), px(3, dpi)}) == HTTRANSPARENT,
           "above a tab: through the strip" + at);
    expect(hit(strip, strip, centre(tab1)) == HTCLIENT, "a tab is the strip's" + at);
    expect(hit(strip, strip, centre(close0)) == HTCLIENT && !IsRectEmpty(&close0), "its close button" + at);
    expect(hit(strip, strip, centre(add)) == HTCLIENT, "and \"+\"" + at);
    const POINT content{tab0.left, px(39, dpi) + px(20, dpi)};
    expect(hit(top, top, content) == HTCLIENT, "under the strip, the client area" + at);
    if (!IsZoomed(top))
    {
        expect(hit(top, top, POINT{px(200, dpi), 1}) == HTTOP, "the window's top edge resizes" + at);
        expect(hit(top, top, POINT{1, 1}) == HTTOPLEFT, "its top left corner" + at);
        expect(hit(strip, strip, POINT{centre(tab1).x, 1}) == HTTRANSPARENT, "the strip lets it through" + at);
    }
    else
        expect(hit(top, top, POINT{px(200, dpi), 1}) == HTCAPTION, "maximised, no edge: the caption" + at);
    // The caption buttons, the compositor's (snap layouts from maximise).
    RECT b{};
    if (g_shown && SUCCEEDED(DwmGetWindowAttribute(top, DWMWA_CAPTION_BUTTON_BOUNDS, &b, sizeof b)) &&
        b.right > b.left)
    {
        RECT w;
        GetWindowRect(top, &w);
        const int y = w.top + (b.top + b.bottom) / 2;
        const POINT close_at{w.left + b.right - px(10, dpi), y},
            max_at{w.left + b.right - (b.right - b.left) / 2, y};
        const LRESULT close = SendMessageW(top, WM_NCHITTEST, 0, MAKELPARAM(short(close_at.x), short(close_at.y)));
        const LRESULT max = SendMessageW(top, WM_NCHITTEST, 0, MAKELPARAM(short(max_at.x), short(max_at.y)));
        // What the coordinates were, and what DWM itself says there: whether
        // the samples or the window's handling is wrong.
        {
            POINT origin{0, 0};
            ClientToScreen(top, &origin);
            RECT c;
            GetClientRect(top, &c);
            HMONITOR mon = MonitorFromWindow(top, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(mon, &mi);
            auto rect = [](const RECT& v)
            {
                return str(long(v.left)) + "," + str(long(v.top)) + ".." + str(long(v.right)) + "," +
                       str(long(v.bottom));
            };
            note("caption buttons" + at + ": zoomed " + str(long(IsZoomed(top))) + ", dpi " + str(long(dpi)) +
                 ", window " + rect(w) + ", client origin " + str(long(origin.x)) + "," + str(long(origin.y)) +
                 " size " + str(long(c.right)) + "x" + str(long(c.bottom)) + ", monitor " + rect(mi.rcMonitor) +
                 " work " + rect(mi.rcWork) + ", DWMWA_CAPTION_BUTTON_BOUNDS " + rect(b));
            auto dwm = [&](POINT s)
            {
                LRESULT r = 0;
                const BOOL handled = DwmDefWindowProc(top, WM_NCHITTEST, 0, MAKELPARAM(short(s.x), short(s.y)), &r);
                return "DwmDefWindowProc " + str(long(handled)) + "/" + str(long(r));
            };
            note("  close sampled at " + str(long(close_at.x)) + "," + str(long(close_at.y)) + ": " +
                 str(long(close)) + ", " + dwm(close_at));
            note("  maximise sampled at " + str(long(max_at.x)) + "," + str(long(max_at.y)) + ": " + str(long(max)) +
                 ", " + dwm(max_at));
            // Where DWM answers for the buttons, down the close button's
            // column from above the window to below the strip.
            std::string scan;
            for (int sy = w.top - px(4, dpi); sy < w.top + px(60, dpi); sy += std::max(1, px(2, dpi)))
            {
                LRESULT r = 0;
                const BOOL handled =
                    DwmDefWindowProc(top, WM_NCHITTEST, 0, MAKELPARAM(short(close_at.x), short(sy)), &r);
                scan += " " + str(long(sy)) + ":" + (handled ? str(long(r)) : std::string("-"));
            }
            note("  DWM down the close column:" + scan);
        }
        expect(close == HTCLOSE, "the close button is the system's" + at + " (" + str(long(close)) + ")");
        expect(max == HTMAXBUTTON, "and maximise" + at + " (" + str(long(max)) + ")");
    }
}

// -- UI Automation --------------------------------------------------------

struct handlers
{
    std::mutex lock;
    std::vector<std::string> selected, closed;
    std::vector<std::pair<std::string, int>> reordered;
    int added = 0;
    std::atomic<int> events{0};
};
handlers H;

template <class T>
void release(T*& p)
{
    if (p) p->Release();
    p = nullptr;
}

std::wstring name_of(IUIAutomationElement* e)
{
    BSTR b = nullptr;
    std::wstring out;
    if (e && SUCCEEDED(e->get_CurrentName(&b)) && b) out = b;
    SysFreeString(b);
    return out;
}

IUIAutomationElement* find(IUIAutomation* uia, IUIAutomationElement* in, TreeScope scope, CONTROLTYPEID type,
                           const wchar_t* name)
{
    VARIANT t;
    VariantInit(&t);
    t.vt = VT_I4;
    t.lVal = type;
    IUIAutomationCondition* by_type = nullptr;
    IUIAutomationCondition* by_name = nullptr;
    IUIAutomationCondition* both = nullptr;
    IUIAutomationElement* found = nullptr;
    uia->CreatePropertyCondition(UIA_ControlTypePropertyId, t, &by_type);
    if (name)
    {
        VARIANT n;
        VariantInit(&n);
        n.vt = VT_BSTR;
        n.bstrVal = SysAllocString(name);
        uia->CreatePropertyCondition(UIA_NamePropertyId, n, &by_name);
        VariantClear(&n);
        if (by_type && by_name) uia->CreateAndCondition(by_type, by_name, &both);
    }
    IUIAutomationCondition* c = name ? both : by_type;
    if (in && c) in->FindFirst(scope, c, &found);
    release(both);
    release(by_name);
    release(by_type);
    return found;
}

template <class Pattern>
Pattern* pattern(IUIAutomationElement* e, PATTERNID id)
{
    Pattern* p = nullptr;
    if (e) e->GetCurrentPatternAs(id, __uuidof(Pattern), reinterpret_cast<void**>(&p));
    return p;
}

// Counts the ChildrenReordered UIA raises.
class structure_counter : public IUIAutomationStructureChangedEventHandler
{
public:
    std::atomic<int> reordered{0};
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }  // on the stack: never deleted
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IUIAutomationStructureChangedEventHandler))
        {
            *out = static_cast<IUIAutomationStructureChangedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(IUIAutomationElement*, StructureChangeType change,
                                                          SAFEARRAY*) override
    {
        if (change == StructureChangeType_ChildrenReordered) ++reordered;
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
};

// The names of `tabs`' TabItems, in UIA's order.
std::vector<std::wstring> item_names(IUIAutomation* uia, IUIAutomationElement* tabs)
{
    std::vector<std::wstring> out;
    IUIAutomationTreeWalker* walker = nullptr;
    uia->get_ControlViewWalker(&walker);
    IUIAutomationElement* child = nullptr;
    if (walker && tabs) walker->GetFirstChildElement(tabs, &child);
    while (child)
    {
        CONTROLTYPEID type = 0;
        child->get_CurrentControlType(&type);
        if (type == UIA_TabItemControlTypeId) out.push_back(name_of(child));
        IUIAutomationElement* next = nullptr;
        walker->GetNextSiblingElement(child, &next);
        release(child);
        child = next;
    }
    release(walker);
    return out;
}

// Waits up to 5 s for `done` to say so, the window's thread handling the
// calls meanwhile.
template <class F>
bool wait_until(F&& done)
{
    for (int i = 0; i < 500; ++i)
    {
        {
            std::lock_guard g(H.lock);
            if (done()) return true;
        }
        Sleep(10);
    }
    std::lock_guard g(H.lock);
    return done();
}

void uia_client(HWND top, std::atomic<bool>* finished)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                     reinterpret_cast<void**>(&uia));
    expect(uia != nullptr, "a UIA client");
    IUIAutomationElement* window = nullptr;
    if (uia) uia->ElementFromHandle(top, &window);
    expect(window != nullptr, "the window, to UIA");
    IUIAutomationElement* tabs = find(uia, window, TreeScope_Descendants, UIA_TabControlTypeId, nullptr);
    expect(tabs != nullptr, "a Tab in it");
    IUIAutomationSelectionPattern* selection = pattern<IUIAutomationSelectionPattern>(tabs, UIA_SelectionPatternId);
    BOOL multiple = TRUE, required = FALSE;
    if (selection)
    {
        selection->get_CurrentCanSelectMultiple(&multiple);
        selection->get_CurrentIsSelectionRequired(&required);
    }
    expect(selection && !multiple && required, "with Selection, single, one required");
    IUIAutomationElement* items[3] = {};
    const wchar_t* names[3] = {L"Alpha", L"Beta", L"Gamma"};
    const char* said[3] = {"Alpha", "Beta", "Gamma"};
    for (int i = 0; i < 3; ++i)
    {
        items[i] = find(uia, tabs, TreeScope_Children, UIA_TabItemControlTypeId, names[i]);
        expect(items[i] != nullptr, std::string("a TabItem named ") + said[i]);
    }
    IUIAutomationElement* add = find(uia, tabs, TreeScope_Children, UIA_ButtonControlTypeId, L"New tab");
    expect(add != nullptr, "a New tab Button");
    // The window's own controls, beside the strip's elements.
    IUIAutomationElement* edit = find(uia, window, TreeScope_Descendants, UIA_EditControlTypeId, nullptr);
    expect(edit != nullptr, "the window's native text field, still reached");
    {
        IUIAutomationTreeWalker* walker = nullptr;
        uia->get_RawViewWalker(&walker);
        IUIAutomationElement* parent = nullptr;
        if (walker && edit) walker->GetParentElement(edit, &parent);
        int depth = 0;
        bool to_window = false;
        while (parent && depth++ < 20)
        {
            BOOL same = FALSE;
            uia->CompareElements(parent, window, &same);
            if (same)
            {
                to_window = true;
                break;
            }
            IUIAutomationElement* up = nullptr;
            walker->GetParentElement(parent, &up);
            release(parent);
            parent = up;
        }
        release(parent);
        release(walker);
        expect(to_window, "navigable up to the window");
    }

    // Select: on_select once, for that tab.
    if (IUIAutomationSelectionItemPattern* item =
            pattern<IUIAutomationSelectionItemPattern>(items[2], UIA_SelectionItemPatternId))
    {
        expect(SUCCEEDED(item->Select()), "Gamma selected");
        expect(wait_until([] { return !H.selected.empty(); }), "on_select heard");
        BOOL is = FALSE;
        item->get_CurrentIsSelected(&is);
        expect(is, "and it's the selected one");
        item->Release();
    }
    else
        expect(false, "Gamma has SelectionItem");
    // Its close button invoked: on_close once, for that tab.
    IUIAutomationElement* close = find(uia, items[0], TreeScope_Children, UIA_ButtonControlTypeId, L"Close tab");
    expect(close != nullptr, "Alpha's Close tab button");
    if (IUIAutomationInvokePattern* invoke = pattern<IUIAutomationInvokePattern>(close, UIA_InvokePatternId))
    {
        expect(SUCCEEDED(invoke->Invoke()), "Close tab invoked");
        expect(wait_until([] { return !H.closed.empty(); }), "on_close heard");
        invoke->Release();
    }
    if (IUIAutomationInvokePattern* invoke = pattern<IUIAutomationInvokePattern>(add, UIA_InvokePatternId))
    {
        expect(SUCCEEDED(invoke->Invoke()), "New tab invoked");
        expect(wait_until([] { return H.added > 0; }), "on_new heard");
        invoke->Release();
    }
    Sleep(200);  // nothing more comes
    {
        std::lock_guard g(H.lock);
        expect(H.selected == std::vector<std::string>{"c"}, "on_select(c) once");
        expect(H.closed == std::vector<std::string>{"a"}, "on_close(a) once");
        expect(H.added == 1, "on_new once");
    }
    // Gamma (current, last) dragged to the front: the strip's sent the
    // mouse's messages from here, handled on the window's thread.
    {
        structure_counter counter;
        const bool listening =
            tabs && SUCCEEDED(uia->AddStructureChangedEventHandler(tabs, TreeScope_Element, nullptr, &counter));
        expect(listening, "listening for the Tab's structure changes");
        HWND strip = FindWindowExW(top, nullptr, L"rapportCaptionTabs", nullptr);
        const RECT from = part(strip, q_tab, 2), to = part(strip, q_tab, 0);
        const POINT a = centre(from), b = centre(to);
        SendMessageW(strip, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(a.x, a.y));
        SendMessageW(strip, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(a.x - 40, a.y));
        SendMessageW(strip, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(b.x, b.y));
        SendMessageW(strip, WM_LBUTTONUP, 0, MAKELPARAM(b.x, b.y));
        expect(wait_until([] { return !H.reordered.empty(); }), "on_reorder heard");
        bool raised = false;
        for (int i = 0; i < 300 && !raised; ++i)
        {
            raised = counter.reordered > 0;
            if (!raised) Sleep(10);
        }
        expect(raised, "ChildrenReordered raised on the Tab (" + str(long(counter.reordered.load())) + ")");
        const std::vector<std::wstring> names = item_names(uia, tabs);
        expect(names == std::vector<std::wstring>{L"Gamma", L"Alpha", L"Beta"}, "the TabItems in the new order");
        {
            std::lock_guard g(H.lock);
            expect(H.reordered == std::vector<std::pair<std::string, int>>{{"c", 0}}, "on_reorder(c, 0) once");
        }
        if (listening) uia->RemoveStructureChangedEventHandler(tabs, &counter);
    }
    release(close);
    release(edit);
    release(add);
    for (auto*& i : items) release(i);
    release(selection);
    release(tabs);
    release(window);
    release(uia);
    CoUninitialize();
    *finished = true;
}

// -- system settings (CI only) --------------------------------------------

// The primary display's scale, as Settings sets it: steps relative to the
// recommended one (DisplayConfig's undocumented DPI scale, as Settings uses).
struct dpi_scale_get
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    int32_t min_rel, cur_rel, max_rel;
};
struct dpi_scale_set
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    int32_t rel;
};
// The DisplayConfig source that is a GDI display device (\\.\DISPLAYn): the
// one display whose scale is read, changed and put back, the same as the
// one whose mode is.
struct display_source
{
    LUID adapter{};
    UINT32 id = 0;
    bool known = false;
};
display_source g_source;

bool source_of(const std::wstring& gdi_name, display_source& out)
{
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS || !paths) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr) != ERROR_SUCCESS)
        return false;
    for (UINT32 i = 0; i < paths; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof name;
        name.header.adapterId = p[i].sourceInfo.adapterId;
        name.header.id = p[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS && gdi_name == name.viewGdiDeviceName)
        {
            out = {p[i].sourceInfo.adapterId, p[i].sourceInfo.id, true};
            return true;
        }
    }
    return false;
}
bool get_scale(dpi_scale_get& g)
{
    g = {};
    if (!g_source.known) return false;
    g.header.type = DISPLAYCONFIG_DEVICE_INFO_TYPE(-3);
    g.header.size = sizeof g;
    g.header.adapterId = g_source.adapter;
    g.header.id = g_source.id;
    return DisplayConfigGetDeviceInfo(&g.header) == ERROR_SUCCESS;
}
bool set_scale(int32_t rel)
{
    if (!g_source.known) return false;
    dpi_scale_set s{};
    s.header.type = DISPLAYCONFIG_DEVICE_INFO_TYPE(-4);
    s.header.size = sizeof s;
    s.header.adapterId = g_source.adapter;
    s.header.id = g_source.id;
    s.rel = rel;
    return DisplayConfigSetDeviceInfo(&s.header) == ERROR_SUCCESS;
}

bool set_high_contrast(bool on)
{
    wchar_t scheme[] = L"High Contrast Black";
    HIGHCONTRASTW hc{sizeof hc};
    hc.dwFlags = on ? HCF_HIGHCONTRASTON : 0;
    hc.lpszDefaultScheme = on ? scheme : nullptr;
    return SystemParametersInfoW(SPI_SETHIGHCONTRAST, sizeof hc, &hc, SPIF_SENDCHANGE) != FALSE;
}

// What the transitions change, as it was before the first change, and put
// back to exactly that on every path (main undoes it after the
// transitions, the destructor if anything returns first). Each restore is
// checked by reading it back, bounded; one that fails is a failure, as the
// base surface's high-contrast harness has it, and stays marked to put
// back: the runner would be left changed for the tests after.
struct restore
{
    // The primary display, its mode and scale step, before any change.
    bool display_saved = false;
    std::wstring device;
    DEVMODEW mode{};
    int32_t rel = 0;
    // High contrast and the system colours it changes, before any change.
    bool contrast_saved = false;
    DWORD contrast_flags = 0;
    std::array<COLORREF, 5> colours{};
    static constexpr int colour_ids[5] = {COLOR_WINDOW, COLOR_WINDOWTEXT, COLOR_HIGHLIGHT, COLOR_HIGHLIGHTTEXT,
                                          COLOR_BTNFACE};
    // What's been changed and not yet put back.
    bool scale = false, mode_changed = false, contrast = false;

    ~restore() { undo(); }

    // Before the display's first change: its device, current mode and scale.
    bool save_display()
    {
        if (display_saved) return true;
        DISPLAY_DEVICEW dev{sizeof dev};
        bool found = false;
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i)
            if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
            {
                found = true;
                break;
            }
        mode = {};
        mode.dmSize = sizeof mode;
        // Its DisplayConfig source, by its GDI name: the scale read, set
        // and put back is this display's, never another active one's.
        dpi_scale_get g{};
        if (!found || !source_of(dev.DeviceName, g_source) ||
            !EnumDisplaySettingsW(dev.DeviceName, ENUM_CURRENT_SETTINGS, &mode) || !get_scale(g))
            return false;
        device = dev.DeviceName;
        rel = g.cur_rel;
        display_saved = true;
        return true;
    }
    bool save_contrast()
    {
        if (contrast_saved) return true;
        HIGHCONTRASTW hc{sizeof hc};
        if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0)) return false;
        contrast_flags = hc.dwFlags & HCF_HIGHCONTRASTON;
        for (size_t i = 0; i < colours.size(); ++i) colours[i] = GetSysColor(colour_ids[i]);
        contrast_saved = true;
        return true;
    }

    bool mode_is_saved() const
    {
        DEVMODEW now{};
        now.dmSize = sizeof now;
        return EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &now) &&
               now.dmPelsWidth == mode.dmPelsWidth && now.dmPelsHeight == mode.dmPelsHeight &&
               now.dmBitsPerPel == mode.dmBitsPerPel && now.dmDisplayFrequency == mode.dmDisplayFrequency;
    }
    bool scale_is_saved() const
    {
        dpi_scale_get now{};
        return get_scale(now) && now.cur_rel == rel;
    }
    bool contrast_is_saved() const
    {
        HIGHCONTRASTW hc{sizeof hc};
        if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) ||
            (hc.dwFlags & HCF_HIGHCONTRASTON) != contrast_flags)
            return false;
        for (size_t i = 0; i < colours.size(); ++i)
            if (GetSysColor(colour_ids[i]) != colours[i]) return false;
        return true;
    }
    // Read back until it is, for a few seconds: the system applies these
    // asynchronously.
    template <class F>
    static bool settles(F&& is)
    {
        for (int waited = 0; waited < 5000; waited += 50)
        {
            if (is()) return true;
            pump(5);  // 50 ms
        }
        return is();
    }

    void undo()
    {
        // The mode first: the scale saved is the saved mode's.
        if (mode_changed)
        {
            const LONG r = ChangeDisplaySettingsExW(device.c_str(), &mode, nullptr, 0, nullptr);
            const bool ok = r == DISP_CHANGE_SUCCESSFUL && settles([&] { return mode_is_saved(); });
            expect(ok, "the display's mode put back to " + str(long(mode.dmPelsWidth)) + "x" +
                           str(long(mode.dmPelsHeight)) + " (" + str(long(r)) + ")");
            if (ok) mode_changed = false;
        }
        if (scale)
        {
            const bool ok = set_scale(rel) && settles([&] { return scale_is_saved(); });
            expect(ok, "the display's scale put back to step " + str(rel));
            if (ok) scale = false;
        }
        if (contrast)
        {
            const bool ok = set_high_contrast(contrast_flags != 0) && settles([&] { return contrast_is_saved(); });
            expect(ok, "high contrast and the system colours put back");
            if (ok) contrast = false;
        }
    }
};
restore g_restore;

template <class F>
bool pump_until(F&& done, int ms = 10000)
{
    for (int waited = 0; waited < ms; waited += 50)
    {
        if (done()) return true;
        pump(5);
    }
    return done();
}

// The display's scale changed, for real, and back: the window's DPI with it.
int dpi_transition(HWND top, HWND strip)
{
    if (!env_is("GITHUB_ACTIONS", "true") || !env_is("RAPPORT_TEST_SET_DPI", "1"))
    {
        note("skipped the DPI change: it changes the display's scale, so only on a CI runner "
             "(GITHUB_ACTIONS=true, RAPPORT_TEST_SET_DPI=1)");
        return 77;
    }
    if (!g_restore.save_display())
    {
        expect(false, "the display's mode and scale read before changing them");
        return 1;
    }
    // Armed before anything changes: a mode change may move the scale too,
    // even if what follows returns early.
    g_restore.scale = true;
    dpi_scale_get g{};
    if (!get_scale(g))
    {
        expect(false, "the display's scale read");
        return 1;
    }
    note("display scale steps " + str(g.min_rel) + ".." + str(g.max_rel) + ", now " + str(g.cur_rel));
    // A small screen (a runner's 1024x768) allows only 100%: its largest
    // mode first, for the scales it allows.
    if (g.max_rel <= g.cur_rel && g.min_rel >= g.cur_rel)
    {
        const wchar_t* device = g_restore.device.c_str();
        DEVMODEW best{}, mode{};
        best.dmSize = mode.dmSize = sizeof(DEVMODEW);
        for (DWORD i = 0; EnumDisplaySettingsW(device, i, &mode); ++i)
            if (mode.dmPelsWidth * mode.dmPelsHeight > best.dmPelsWidth * best.dmPelsHeight) best = mode;
        // Marked before it's tried: a change half made is put back too.
        if (best.dmPelsWidth) g_restore.mode_changed = true;
        if (best.dmPelsWidth &&
            ChangeDisplaySettingsExW(device, &best, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL)
        {
            note("display mode " + str(long(best.dmPelsWidth)) + "x" + str(long(best.dmPelsHeight)));
            pump(50);
            get_scale(g);
            note("display scale steps " + str(g.min_rel) + ".." + str(g.max_rel) + ", now " + str(g.cur_rel));
        }
    }
    const int32_t to = g.max_rel > g.cur_rel ? g.cur_rel + 1 : g.min_rel < g.cur_rel ? g.cur_rel - 1 : g.cur_rel;
    expect(to != g.cur_rel, "the display allows another scale");
    if (to == g.cur_rel) return 1;
    const UINT before = GetDpiForWindow(top);
    expect(set_scale(to), "the display's scale set");
    const bool changed = pump_until([&] { return GetDpiForWindow(top) != before; });
    expect(changed, "the window's DPI changed with it, from " + str(long(before)) + " to " +
                        str(long(GetDpiForWindow(top))));
    pump();
    check_layout(top, strip, "after the scale changed");
    check_hits(top, strip, "after the scale changed");
    check_pixels(strip, expected(high_contrast()), "after the scale changed");
    g_restore.undo();
    expect(pump_until([&] { return GetDpiForWindow(top) == before; }), "and back to " + str(long(before)));
    pump();
    check_layout(top, strip, "the scale back");
    return 0;
}

// The window's backdrop (DWMWA_SYSTEMBACKDROP_TYPE, 38): 0 if it can't be read.
DWORD backdrop_of(HWND top)
{
    DWORD b = 0;
    return SUCCEEDED(DwmGetWindowAttribute(top, 38, &b, sizeof b)) ? b : 0;
}

// Mica Alt (DWMSBT_TABBEDWINDOW, 4) behind the tabs in the caption, as
// Explorer's window has; none (1) in high contrast; and Mica (2) again once
// the tabs leave the caption (hidden), then Mica Alt as they come back.
// 22621 on, where the attribute's named.
void check_backdrop(HWND top, rap::document_tabs* tabs)
{
    if (windows_build() < 22621)
    {
        note("no backdrop attribute before build 22621: not checked");
        return;
    }
    const bool hc = high_contrast();
    const DWORD with = hc ? 1 : 4;
    expect(backdrop_of(top) == with, std::string("the tabs in the caption: the backdrop ") +
                                         (hc ? "none (1), high contrast" : "Mica Alt (4), as Explorer's") + ": " +
                                         str(long(backdrop_of(top))));
    if (!tabs) return;
    tabs->set_visible(false);
    pump(2);
    expect(!strip_of(top) || !(GetWindowLongPtrW(strip_of(top), GWL_STYLE) & WS_VISIBLE), "the tabs hidden");
    expect(backdrop_of(top) == 2, "the tabs gone from the caption: Mica (2) again: " + str(long(backdrop_of(top))));
    tabs->set_visible(true);
    pump(2);
    expect(backdrop_of(top) == with, "and back in it: " + str(long(with)) + " again: " + str(long(backdrop_of(top))));
}

// High contrast on and off with the window open.
int contrast_transition(HWND top, HWND strip)
{
    if (!env_is("GITHUB_ACTIONS", "true") || !env_is("RAPPORT_TEST_SET_HIGH_CONTRAST", "1"))
    {
        note("skipped turning high contrast on and off: a system setting, so only on a CI runner "
             "(GITHUB_ACTIONS=true, RAPPORT_TEST_SET_HIGH_CONTRAST=1)");
        return 77;
    }
    if (high_contrast())
    {
        note("skipped turning high contrast on: it's on already");
        return 77;
    }
    const bool mica = windows_build() >= 22621;
    auto backdrop = [&]
    {
        DWORD b = 0;
        DwmGetWindowAttribute(top, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &b, sizeof b);
        return b;
    };
    if (!g_restore.save_contrast())
    {
        expect(false, "high contrast and the system colours read before changing them");
        return 1;
    }
    g_restore.contrast = true;
    expect(set_high_contrast(true), "high contrast on");
    const bool took = pump_until(
        [&]
        {
            const printed p = print(strip);
            const int x = past_add(strip, "waiting for high contrast", false);
            if (x < 0) return false;
            const DWORD v = p.at(x, p.h / 2);
            return high_contrast() && (v & 0xffffff) == expected(true).empty && (v >> 24) == 255;
        });
    expect(took, "the strip takes high contrast's colours");
    const palette on = expected(true);
    check_pixels(strip, on, "high contrast turned on");
    check_text(strip, on, "high contrast turned on");
    check_layout(top, strip, "high contrast turned on");
    if (mica) expect(backdrop() == 1, "no Mica in high contrast (" + str(long(backdrop())) + ")");
    g_restore.undo();
    const bool back = pump_until([&] { return !high_contrast() && (print(strip).at(2, 2) >> 24) == 0; });
    expect(back, "high contrast off: the strip clear again");
    check_pixels(strip, expected(false), "high contrast turned off");
    check_text(strip, expected(false), "high contrast turned off");
    if (mica) expect(backdrop() == 4, "Mica Alt again (" + str(long(backdrop())) + ")");
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    bool transitions = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--base") g_base = true;
        if (std::string_view(argv[i]) == "--transitions") transitions = true;
    }
    rap::load_options options{.backend = "win32"};
    options.look.color_scheme = rap::color_scheme::dark;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 2;
    const DWORD build = windows_build();
    const bool hc = high_contrast();
    g_dark = !hc && build >= 17763 && env("RAPPORT_THEME") != "light";
    g_base = g_base && build >= 22000;
    std::printf("build %lu, %s%s\n", build, hc ? "high contrast" : g_dark ? "dark" : "light",
                g_base ? ", base surface" : "");

    using namespace rap::layout;
    rap::document_tabs* tabs = nullptr;
    auto doc_tabs = [&]
    {
        return document_tabs{{{"a", "Alpha", rap::icon::named("folder"), "/home/me/alpha"},
                              {"b", "Beta", rap::icon::named("folder"), {}},
                              {"c", "Gamma", {}, {}}}}
            .current("a")
            .on_select(
                [](const std::string& id)
                {
                    std::lock_guard g(H.lock);
                    H.selected.push_back(id);
                })
            .on_close(
                [](const std::string& id)
                {
                    std::lock_guard g(H.lock);
                    H.closed.push_back(id);
                })
            .on_new(
                []
                {
                    std::lock_guard g(H.lock);
                    ++H.added;
                })
            .on_reorder(
                [](const std::string& id, int i)
                {
                    std::lock_guard g(H.lock);
                    H.reordered.emplace_back(id, i);
                });
    };
    const char* title = "Caption tabs";
    // A window whose first content item is a toolbar across it (or, with
    // `first` false, second, under a label).
    const char* band_title = "Caption tabs, toolbar band";
    auto band_window = [&](const char* name, bool first, bool margins = false)
    {
        auto bar = []
        {
            return toolbar{tool_button{rap::icon::named("go-previous"), "Back"},
                           tool_button{rap::icon::named("go-next"), "Forward"}, separator{}, entry{}.stretch()};
        };
        window w = first ? window{name, {bar(), label{"The current tab's content"}}}
                         : window{name, {label{"Above the toolbar"}, bar()}};
        w.size(900_px, 300_px).margins(margins).base_surface(g_base).tabs(doc_tabs());
        return w;
    };
    auto win = (*ui)->create(window{title, {entry{}, label{"The current tab's content"}}}
                                 .size(1000_px, 300_px)
                                 .base_surface(g_base)
                                 .tabs(doc_tabs().bind(tabs)));
    show(*win);
    pump();
    HWND top = top_of(title);
    expect(top != nullptr, "the window");
    if (!top) return 1;
    {
        wchar_t text[64]{};
        GetWindowTextW(top, text, 64);
        expect(std::wstring(text) == L"Caption tabs", "the window's title still set, for the taskbar and UIA");
    }
    HWND strip = strip_of(top);

    if (build < 22000)
    {
        // Windows 10 (Server 2022's 20348): stage 1, unchanged.
        expect(!strip, "before Windows 11, no strip in the caption");
        expect(tab_control_of(top) != nullptr, "but stage 1's tab control");
        expect(title_bar(top) >= GetSystemMetrics(SM_CYCAPTION),
               "under the title bar (" + str(title_bar(top)) + ")");
        // A toolbar first: no band, as before.
        auto banded = (*ui)->create(band_window(band_title, true));
        show(*banded);
        pump();
        if (HWND b = top_of(band_title))
            check_band_rows(b, 0, false, expected(hc), nullptr, "before Windows 11, a toolbar first");
        else
            expect(false, "the toolbar's window");
        std::printf(fails ? "%d failed\n" : "all passed\n", fails.load());
        if (transitions)
        {
            note("skipped the transitions: no tabs in the caption before Windows 11 (build 22000)");
            return fails ? 1 : 77;
        }
        return fails ? 1 : 0;
    }

    expect(strip != nullptr, "the strip in the caption");
    expect(!tab_control_of(top), "and no stage 1 tab control");
    if (!strip) return 1;
    expect(ask(strip, q_count) == 3 && ask(strip, q_current) == 0, "three tabs, the first current");

    if (transitions)
    {
        check_layout(top, strip, "before");
        const int dpi = dpi_transition(top, strip);
        const int contrast = contrast_transition(top, strip);
        g_restore.undo();
        std::printf(fails ? "%d failed\n" : "all passed\n", fails.load());
        if (fails) return 1;
        return dpi == 77 && contrast == 77 ? 77 : 0;
    }

    check_layout(top, strip, "shown");
    check_hits(top, strip, "shown");
    check_pixels(strip, expected(hc), hc ? "high contrast" : g_dark ? "dark" : "light");
    check_text(strip, expected(hc), hc ? "high contrast" : g_dark ? "dark" : "light");
    check_separators(strip, expected(hc), hc ? "high contrast" : g_dark ? "dark" : "light");
    check_rebuild(top, strip);
    check_clipped_prints(strip);
    check_copy_failure(strip, expected(hc));
    check_backdrop(top, tabs);
    // Its first item a text field, no toolbar: no band, no line, the current
    // tab the window's surface (check_pixels).
    check_band_rows(top, int(rect_in(top, strip).bottom), false, expected(hc), nullptr, "no toolbar");
    if (g_shown)
    {
        check_screen(top, strip, expected(hc));
        // Maximised: the client area still from the window's top, past the
        // monitor's; the strip at the work area's top (check_layout), no
        // edge to resize by, and the caption buttons still the system's.
        ShowWindow(top, SW_MAXIMIZE);
        pump();
        expect(IsZoomed(top), "maximised");
        check_layout(top, strip, "maximised");
        check_hits(top, strip, "maximised");
        ShowWindow(top, SW_RESTORE);
        pump();
        check_layout(top, strip, "restored");
        check_hits(top, strip, "restored");
    }

    // UI Automation, with this thread handling its calls meanwhile. Only
    // shown: UIA leaves hidden windows' content out of its tree.
    if (g_shown)
    {
        std::atomic<bool> finished{false};
        std::thread client(uia_client, top, &finished);
        const ULONGLONG deadline = GetTickCount64() + 60000;
        while (!finished && GetTickCount64() < deadline) pump(1);
        if (!finished)
        {
            expect(false, "the UIA client finished within a minute");
            std::fflush(stdout);
            ExitProcess(1);
        }
        client.join();
        pump();
        expect(ask(strip, q_count) == 3 && ask(strip, q_current) == 0 &&
                   tabs->ids() == std::vector<std::string>{"c", "a", "b"},
               "the strip shows Gamma current, first, as dragged; the app's tabs otherwise as they were");
    }

    // A toolbar first, across the content: the band, on a base surface.
    {
        auto banded = (*ui)->create(band_window(band_title, true));
        show(*banded);
        pump();
        HWND b = top_of(band_title);
        expect(b && strip_of(b), "a toolbar's window, its tabs in the caption");
        if (b && strip_of(b))
        {
            check_band(b, g_base, g_base ? "a toolbar first" : "a toolbar first, no base surface");
            if (g_shown && g_base)
            {
                HWND s = strip_of(b);
                SetWindowPos(b, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
                RedrawWindow(b, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
                pump();
                DwmFlush();
                const UINT dpi = GetDpiForWindow(b);
                const int h = int(rect_in(b, s).bottom);
                const RECT tab = part(s, q_tab, ask(s, q_current));
                RECT c;
                GetClientRect(b, &c);
                // A point of the band no child covers (a toolbar's
                // controls fill most of it), checked to be the window's own.
                const POINT at = band_point(b, h), tab_at{tab.left + px(4, dpi), h - 1};
                expect(at.x >= 0 && window_own(b, at, "on screen, the band's sample"),
                       "an uncovered point of the band to sample on screen");
                expect(ChildWindowFromPointEx(b, tab_at, CWP_SKIPINVISIBLE) == s, "the tab's sample is the strip's");
                // The band's layer over whatever the compositor has there
                // (Mica Alt, or its solid colour inactive): the strip's
                // empty part past "+" on screen, which is that alone.
                const int past = past_add(s, "on screen");
                const DWORD backdrop = past >= 0 ? on_screen(s, POINT{past, h / 2}) : 0xDEADBEEF;
                const DWORD fill = hc ? band_fill(true) : over(layer(), backdrop), u = on_screen(b, tab_at),
                            under = at.x >= 0 ? on_screen(b, at) : 0xDEADBEEF;
                note("on screen, the backdrop past \"+\": " + hex(backdrop));
                auto close_to = [](DWORD a, DWORD b)
                {
                    for (int shift : {16, 8, 0})
                        if (std::abs(channel(a, shift) - channel(b, shift)) > 3) return false;
                    return true;
                };
                expect(close_to(under, fill), "on screen, the band is its layer over the backdrop, " + hex(fill) +
                                                  " at " + str(at.x) + "," + str(at.y) + ": " + hex(under));
                expect(hc ? u == expected(true).current : close_to(u, under),
                       "and the current tab's bottom row one with it: " + hex(u));
                SetWindowPos(b, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
            }
        }
        // The window's margins on: the toolbar still spans the content.
        const char* margined_title = "Caption tabs, toolbar band, margins";
        auto margined = (*ui)->create(band_window(margined_title, true, true));
        show(*margined);
        pump();
        if (HWND m = top_of(margined_title); m && strip_of(m))
            check_band_rows(m, int(rect_in(m, strip_of(m)).bottom), g_base, expected(hc),
                            FindWindowExW(m, nullptr, TOOLBARCLASSNAMEW, nullptr), "a toolbar first, margins on");
        else
            expect(false, "the margined toolbar's window, its tabs in the caption");
        // Second, under a label: nothing qualifies, no band.
        const char* second_title = "Caption tabs, toolbar second";
        auto second = (*ui)->create(band_window(second_title, false));
        show(*second);
        pump();
        if (HWND t = top_of(second_title); t && strip_of(t))
        {
            check_band_rows(t, int(rect_in(t, strip_of(t)).bottom), false, expected(hc), nullptr,
                            "a toolbar second");
            check_pixels(strip_of(t), expected(hc), "a toolbar second");
        }
        else
            expect(false, "the second toolbar's window, its tabs in the caption");
    }

    // Every kind of control a toolbar holds, in the band, on glass (a base
    // surface's only: no band otherwise).
    if (g_base)
    {
        const char* glass_title = "Caption tabs, glass";
        auto glassy = (*ui)->create(
            window{glass_title,
                   {toolbar{tool_button{rap::icon::named("go-previous"), "Back"},
                            tool_button{rap::icon::named("go-next"), "Forward"}, separator{}, button{"Go"},
                            menu_button{"Menu"}, check{"Check"}, label{"Label"}, drop_down{{"One", "Two"}},
                            combo{{"One", "Two"}, "One"}, hbox{tool_button{rap::icon::named("go-up"), "Tool"}},
                            entry{"Text"}.clearable().stretch()},
                    label{"Content"}}}
                .size(1300_px, 300_px)
                .base_surface(true)
                .tabs(doc_tabs()));
        show(*glassy);
        pump();
        HWND g = top_of(glass_title);
        expect(g && strip_of(g), "the glass window, its tabs in the caption");
        if (g) check_glass(g);
    }

    // Under a menu bar, and in a dialog: stage 1's, under the title bar.
    {
        const char* menu_title = "Caption tabs, menu bar";
        // A toolbar first, on a base surface: no band in stage 1.
        auto menued = (*ui)->create(window{menu_title, {toolbar{tool_button{rap::icon::named("go-previous"), "Back"},
                                                                entry{}.stretch()},
                                                        label{"Content"}}}
                                        .size(600_px, 200_px)
                                        .margins(false)
                                        .base_surface(g_base)
                                        .menu_bar({{"_File", [] { return rap::menu{}; }}})
                                        .tabs(doc_tabs()));
        show(*menued);
        pump();
        HWND m = top_of(menu_title);
        expect(m && !strip_of(m) && tab_control_of(m), "under a menu bar: stage 1's strip");
        expect(m && title_bar(m) >= GetSystemMetrics(SM_CYCAPTION), "and the title bar kept");
        if (m) check_band_rows(m, 0, false, expected(hc), nullptr, "stage 1, a toolbar first");
        const char* dialog_title = "Caption tabs, dialog";
        auto dlg = (*ui)->create(dialog{dialog_title, {label{"Content"}}}.size(400_px, 200_px).tabs(doc_tabs()));
        show(*dlg);
        pump();
        HWND d = top_of(dialog_title);
        expect(d && !strip_of(d) && tab_control_of(d), "in a dialog: stage 1's strip");
        expect(d && title_bar(d) >= GetSystemMetrics(SM_CYCAPTION), "and the title bar kept");
    }

    std::printf(fails ? "%d failed\n" : "all passed\n", fails.load());
    return fails ? 1 : 0;
}
