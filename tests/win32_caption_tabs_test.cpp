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
// with its edge the divider's, one with the band under the strip; the line
// between strip and band elsewhere, under the caption buttons too. Its
// text: the titles in the UI font (Segoe UI Variable Text, else Segoe UI),
// as heavy as that face is, the current one in WinUI's TextFillColorPrimary,
// the others TextFillColorSecondary; ClearType on the current tab when the
// runner's own font smoothing is ClearType (read, never changed), grey over
// the backdrop, a hovered tab too (its fill WinUI's translucent
// LayerOnMicaBaseAltFillColorSecondary; the hover a WM_MOUSEMOVE sent to
// the strip, not input); "+" and the close button
// Segoe Fluent Icons' glyphs in TextFillColorSecondary; the rendering
// parameters read again on a WM_SETTINGCHANGE. UI
// Automation, from a client on a thread of its own: the Tab, its TabItems by
// name, Select, each Close tab and New tab invoked, each handler once; the
// window's native controls still reached. Light and dark (RAPPORT_THEME),
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
    // The current tab's title; "+" at rest; the current tab's close button;
    // another tab's title; a tab not current hovered (none in high contrast).
    argb title{}, glyph{}, close{}, other{}, hover{};
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
        p.glyph = {rgb(GetSysColor(COLOR_WINDOWTEXT)), 255};
        p.close = p.title;
        p.other = {rgb(GetSysColor(COLOR_WINDOWTEXT)), 255};
        return p;
    }
    p.band = g_base ? (g_dark ? 0x191919 : rgb(GetSysColor(COLOR_WINDOW)))
                    : (g_dark ? 0x202020 : rgb(GetSysColor(COLOR_BTNFACE)));
    p.line = over(g_dark ? dark_stroke : light_stroke, p.band);
    p.current = p.band;
    p.title = g_dark ? dark_primary : light_primary;
    p.glyph = p.close = p.other = g_dark ? dark_secondary : light_secondary;
    p.hover = g_dark ? dark_hover : light_hover;
    return p;
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
    q_fonts = 7
};
// What q_fonts fills (caption_tabs_fonts in win32_caption_tabs.h).
struct strip_fonts
{
    wchar_t text[64];
    wchar_t glyphs[64];
    float text_size, glyph_size;
    int text_weight;
    int cleartype;
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
    // it, no line), its sides the divider's.
    const DWORD inside = p.at(tab.left + px(4, dpi), mid), bottom = p.at(tab.left + px(4, dpi), h - 1);
    expect(is(inside, pal.current), "the current tab is " + hex(pal.current) + at + ": " + hex(colour(inside)) +
                                        " alpha " + str(long(alpha(inside))));
    expect(is(bottom, pal.current), "and so is its bottom row, where the line isn't" + at + ": " +
                                        hex(colour(bottom)));
    if (pal.current == pal.band)
    {
        const DWORD side = p.at(tab.left, mid);
        expect(is(side, pal.line), "its sides are the divider's, " + hex(pal.line) + at + ": " + hex(colour(side)));
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
            if (colour(p.at(x, y)) != pal.current) ++ink;
    expect(ink > 10, "the current tab's title is drawn" + at + " (" + str(ink) + " pixels)");
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
    const ink_stats t = ink_in(p, title, pal.title, pal.current);
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
           "the title in " + hex(over(pal.title, pal.current)) + " (TextFillColorPrimary on the tab), never past it" +
               at + ": peak " + str(std::lround(t.peak * 100)) + "%" + (t.within ? "" : ", past at " + t.worst));
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
    if (cleartype && geometry == DWRITE_PIXEL_GEOMETRY_FLAT)
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
    const RECT other_box{other.left + px(12, dpi), other.top + px(4, dpi), other.left + px(120, dpi), p.h - line};
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

    // The glyphs: "+" and the current tab's close button, Segoe Fluent
    // Icons' in the secondary text colour.
    const double plus_ref = reference_ink(icons, glyph, 400, L""),
                 x_ref = reference_ink(icons, glyph, 400, L"");
    const ink_stats a = ink_in(p, add, pal.glyph, pal.clear ? -1 : (long long)pal.empty);
    expect(a.within && a.peak >= 0.6 && (!pal.clear || a.chroma == 0),
           "\"+\" in " + hex(pal.glyph.rgb) + " at alpha " + str(pal.glyph.a) + at + ": peak "+ str(std::lround(a.peak * 100)) + "%, chroma " + str(a.chroma) +
               (a.within ? "" : ", past at " + a.worst));
    expect(a.mass >= plus_ref * 0.8 && a.mass <= plus_ref * 1.5,
           "\"+\" as light as Segoe Fluent Icons' E710, about " + str(std::lround(plus_ref)) + at + ": " +
               str(std::lround(a.mass)));
    const ink_stats x = ink_in(p, close, pal.close, pal.current);
    expect(x.within && x.peak >= 0.6,
           "the close button's X in " + hex(over(pal.close, pal.current)) + " on the tab" + at + ": peak " +
               str(std::lround(x.peak * 100)) + "%" + (x.within ? "" : ", past at " + x.worst));
    expect(x.mass >= x_ref * 0.8 && x.mass <= x_ref * 1.5,
           "and as light as E711, about " + str(std::lround(x_ref)) + at + ": " + str(std::lround(x.mass)));
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
    if (mica) expect(backdrop() == 2, "Mica again (" + str(long(backdrop())) + ")");
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
                              {"b", "Beta", {}, {}},
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
                });
    };
    const char* title = "Caption tabs";
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
    check_rebuild(top, strip);
    check_clipped_prints(strip);
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
        expect(ask(strip, q_count) == 3 && ask(strip, q_current) == 2,
               "the strip shows Gamma current, and the app's tabs as they were");
    }

    // Under a menu bar, and in a dialog: stage 1's, under the title bar.
    {
        const char* menu_title = "Caption tabs, menu bar";
        auto menued = (*ui)->create(window{menu_title, {label{"Content"}}}
                                        .size(600_px, 200_px)
                                        .menu_bar({{"_File", [] { return rap::menu{}; }}})
                                        .tabs(doc_tabs()));
        show(*menued);
        pump();
        HWND m = top_of(menu_title);
        expect(m && !strip_of(m) && tab_control_of(m), "under a menu bar: stage 1's strip");
        expect(m && title_bar(m) >= GetSystemMetrics(SM_CYCAPTION), "and the title bar kept");
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
