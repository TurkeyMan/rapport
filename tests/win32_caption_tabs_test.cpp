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
// between strip and band elsewhere, under the caption buttons too. UI
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
#include <ole2.h>
#include <uiautomation.h>

#include <algorithm>
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

// The colours rapport's roles give, as win32-base-surface has them:
// Explorer's dark ones (backends/common/theme_palette.h), else the system
// colours; high contrast's own.
struct palette
{
    DWORD empty = 0;  // the strip's empty part, where it's opaque
    bool clear = true;
    DWORD band = 0, line = 0, current = 0;
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
        return p;
    }
    p.band = g_base ? (g_dark ? 0x191919 : rgb(GetSysColor(COLOR_WINDOW)))
                    : (g_dark ? 0x202020 : rgb(GetSysColor(COLOR_BTNFACE)));
    p.line = g_dark ? 0x333333 : rgb(GetSysColor(COLOR_3DLIGHT));
    p.current = p.band;
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
    q_add = 5
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
    expect(s.left == 0 && s.top == 0 && s.bottom - s.top == height,
           "the strip at the client area's top left, " + str(height) + " tall" + at + ": " + str(s.left) + "," +
               str(s.top) + " " + str(s.bottom - s.top) + " tall");
    if (!IsZoomed(top))
        expect(title_bar(top) == 0, "no title bar: the client area from the window's top" + at + " (" +
                                        str(title_bar(top)) + ")");
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
void check_pixels(HWND strip, const palette& pal, const std::string& when)
{
    const UINT dpi = GetDpiForWindow(strip);
    const printed p = print(strip);
    const int current = ask(strip, q_current);
    const RECT tab = part(strip, q_tab, current), add = part(strip, q_add);
    const int h = p.h, line = std::max(1, px(1, dpi)), mid = (tab.top + h - line) / 2;
    const std::string at = " (" + when + ")";
    auto colour = [](DWORD v) { return v & 0xffffff; };
    auto alpha = [](DWORD v) { return v >> 24; };
    auto is = [&](DWORD v, DWORD want) { return alpha(v) == 255 && colour(v) == want; };
    // The empty part: above the tabs, and past "+".
    for (const POINT e : {POINT{tab.left + px(24, dpi), px(3, dpi)}, POINT{add.right + px(12, dpi), h / 2}})
    {
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
    for (const int x : {int(add.right) + px(12, dpi), current == 0 ? int(part(strip, q_tab, 1).left) + px(20, dpi)
                                                                   : px(16, dpi)})
    {
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
    const POINT empty{add.right + px(12, dpi), px(20, dpi)}, between{tab1.right, px(20, dpi)};
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
        const LRESULT close = SendMessageW(top, WM_NCHITTEST, 0, MAKELPARAM(short(w.left + b.right - px(10, dpi)), short(y)));
        const LRESULT max = SendMessageW(top, WM_NCHITTEST, 0,
                                         MAKELPARAM(short(w.left + b.right - (b.right - b.left) / 2), short(y)));
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
bool primary_source(LUID& adapter, UINT32& id)
{
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS || !paths) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr) != ERROR_SUCCESS)
        return false;
    adapter = p[0].sourceInfo.adapterId;
    id = p[0].sourceInfo.id;
    return true;
}
bool get_scale(dpi_scale_get& g)
{
    g = {};
    g.header.type = DISPLAYCONFIG_DEVICE_INFO_TYPE(-3);
    g.header.size = sizeof g;
    return primary_source(g.header.adapterId, g.header.id) && DisplayConfigGetDeviceInfo(&g.header) == ERROR_SUCCESS;
}
bool set_scale(int32_t rel)
{
    dpi_scale_set s{};
    s.header.type = DISPLAYCONFIG_DEVICE_INFO_TYPE(-4);
    s.header.size = sizeof s;
    s.rel = rel;
    return primary_source(s.header.adapterId, s.header.id) && DisplayConfigSetDeviceInfo(&s.header) == ERROR_SUCCESS;
}

bool set_high_contrast(bool on)
{
    wchar_t scheme[] = L"High Contrast Black";
    HIGHCONTRASTW hc{sizeof hc};
    hc.dwFlags = on ? HCF_HIGHCONTRASTON : 0;
    hc.lpszDefaultScheme = on ? scheme : nullptr;
    return SystemParametersInfoW(SPI_SETHIGHCONTRAST, sizeof hc, &hc, SPIF_SENDCHANGE) != FALSE;
}

// Put back whatever happens.
struct restore
{
    bool scale = false, mode = false, contrast = false;
    int32_t rel = 0;
    std::wstring device;
    ~restore() { undo(); }
    void undo()
    {
        if (scale) set_scale(rel);
        if (mode) ChangeDisplaySettingsExW(device.c_str(), nullptr, nullptr, 0, nullptr);
        if (contrast) set_high_contrast(false);
        scale = mode = contrast = false;
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
        DISPLAY_DEVICEW dev{sizeof dev};
        DEVMODEW best{}, mode{};
        best.dmSize = mode.dmSize = sizeof(DEVMODEW);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i)
            if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) break;
        for (DWORD i = 0; EnumDisplaySettingsW(dev.DeviceName, i, &mode); ++i)
            if (mode.dmPelsWidth * mode.dmPelsHeight > best.dmPelsWidth * best.dmPelsHeight) best = mode;
        if (best.dmPelsWidth && ChangeDisplaySettingsExW(dev.DeviceName, &best, nullptr, CDS_FULLSCREEN, nullptr) ==
                                    DISP_CHANGE_SUCCESSFUL)
        {
            g_restore.mode = true;
            g_restore.device = dev.DeviceName;
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
    g_restore.rel = g.cur_rel;
    g_restore.scale = true;
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
    g_restore.contrast = true;
    expect(set_high_contrast(true), "high contrast on");
    const bool took = pump_until(
        [&]
        {
            const printed p = print(strip);
            const RECT add = part(strip, q_add);
            const DWORD v = p.at(add.right + 12, p.h / 2);
            return high_contrast() && (v & 0xffffff) == expected(true).empty && (v >> 24) == 255;
        });
    expect(took, "the strip takes high contrast's colours");
    const palette on = expected(true);
    check_pixels(strip, on, "high contrast turned on");
    check_layout(top, strip, "high contrast turned on");
    if (mica) expect(backdrop() == 1, "no Mica in high contrast (" + str(long(backdrop())) + ")");
    g_restore.undo();
    const bool back = pump_until([&] { return !high_contrast() && (print(strip).at(2, 2) >> 24) == 0; });
    expect(back, "high contrast off: the strip clear again");
    check_pixels(strip, expected(false), "high contrast turned off");
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
    if (g_shown)
    {
        check_screen(top, strip, expected(hc));
        // Maximised: the frame's thickness above the client area, the
        // strip at its top, no edge to resize by.
        ShowWindow(top, SW_MAXIMIZE);
        pump();
        const UINT dpi = GetDpiForWindow(top);
        const int frame = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        expect(IsZoomed(top), "maximised");
        expect(title_bar(top) == frame, "the client area below the frame's thickness, " + str(frame) + " (" +
                                            str(title_bar(top)) + ")");
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
