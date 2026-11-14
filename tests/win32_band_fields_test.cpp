// The toolbar band's metrics and its text fields, on Windows 11 (the tabs in
// the caption, a base surface: Explorer's address row).
//
// The band's toolbar takes WinUI's command-bar metrics as Explorer's address
// row measures them at 100%: the band 46 px from the strip to its line, its
// fields 32 px tall with 7 px above and below, the navigation buttons 40 by
// 32 and 8 apart. Each field is still the native EDIT, its frame and fill
// drawn as WinUI's text box: printed (WM_PRINT, its client area through
// WM_PRINTCLIENT) into a 32-bit bitmap, never shown, each state, light and
// dark (RAPPORT_THEME): at rest (no border, ControlFillColorDefault over the
// band, its corners rounded to the band), hovered (a WM_MOUSEMOVE sent: its
// fill ControlFillColorSecondary and the ControlStrokeColorDefault border),
// focused (WM_SETFOCUS sent: ControlFillColorInputActive, the border, and
// the accent's AccentFillColorDefault along its bottom 2 px), disabled,
// read-only (as at rest: WinUI draws it as it does an editable box) and
// empty with its placeholder (TextFillColorSecondary); its text in
// TextFillColorPrimary, the capitals' ink centred in the field to a pixel.
// The search field (a leading search button, clearable) keeps its buttons:
// the glyph where Explorer's sits, the clear button there while there's
// text, both doing what they do. At 125% and 150% through the DPI test hook
// (rap::win32::set_dpi_for_tests and a WM_DPICHANGED, static builds), the
// same metrics scaled. At a larger text size (Settings > Accessibility >
// Text size: 150% and 225%, through rap::win32::set_text_scale_for_tests,
// the setting untouched), the fields' text enlarged by it from WinUI's 14
// and the fields and the row taller to hold it. A framed control on the band
// that isn't a band field (a spin's EDIT, its client edge) printed whole
// (WM_PRINT): its client area inside its frame, as it prints alone, opaque,
// and its frame our border. Under high contrast (rapport-win32-high-contrast-run,
// CI only): nothing drawn, the native bordered EDIT at the band's metrics,
// its text centred. Shown only (UI Automation leaves hidden windows out):
// the field still an Edit to UIA, with its name and value.
//
// Unchanged elsewhere: a window without a base surface keeps its plain
// fields and toolbar, and before Windows 11 there's no band.
//   rapport-win32-band-fields-test
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which they need.
#include <commctrl.h>
#include <ole2.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(                                                                                   \
    linker,                                                                                        \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
namespace rap::win32
{
void set_dpi_for_tests(UINT dpi);  // win32_common.h (which wants the backend's include paths)
void set_text_scale_for_tests(float scale);
}
#endif

namespace
{

std::atomic<int> fails{0};

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
}

void note(const std::string& what)
{
    std::printf("     %s\n", what.c_str());
    std::fflush(stdout);
}

std::string hex(DWORD v)
{
    char b[16];
    std::snprintf(b, sizeof b, "%06lx", v & 0xffffff);
    return b;
}
std::string argb_hex(DWORD v)
{
    char b[16];
    std::snprintf(b, sizeof b, "%08lx", v);
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

// 0xRRGGBB, as pixels read.
DWORD rgb(COLORREF c)
{
    return (DWORD(GetRValue(c)) << 16) | (DWORD(GetGValue(c)) << 8) | GetBValue(c);
}

int px(float logical, UINT dpi)
{
    return int(std::lround(logical * float(dpi) / 96.f));
}

int channel(DWORD v, int shift)
{
    return int((v >> shift) & 0xff);
}

// -- what's expected ------------------------------------------------------

bool g_dark = false;

// WinUI's colours (#AARRGGBB), as backends/win32/win32_band_field.cpp has
// them, each over what's under it.
struct argb
{
    DWORD rgb;  // 0xRRGGBB
    int a;
};
DWORD over(argb top, DWORD under)
{
    DWORD out = 0;
    for (int shift : {16, 8, 0})
        out |= DWORD((channel(top.rgb, shift) * top.a + channel(under, shift) * (255 - top.a) + 127) / 255) << shift;
    return out;
}
argb from(DWORD v)
{
    return {v & 0xffffff, int(v >> 24)};
}
struct field_colors
{
    argb rest, hover, focus, disabled, stroke, text, placeholder, placeholder_focused, text_disabled;
};
// ControlFillColorDefault, ...Secondary, ...InputActive, ...Disabled;
// ControlStrokeColorDefault; TextFillColorPrimary, ...Secondary, ...Tertiary,
// ...Disabled.
field_colors colours()
{
    if (g_dark)
        return {from(0x0FFFFFFF), from(0x15FFFFFF), from(0xB31E1E1E), from(0x0BFFFFFF), from(0x12FFFFFF),
                from(0xFFFFFFFF), from(0xC5FFFFFF), from(0x87FFFFFF), from(0x5DFFFFFF)};
    return {from(0xB3FFFFFF), from(0x80F9F9F9), from(0xFFFFFFFF), from(0x4DF9F9F9), from(0x0F000000),
            from(0xE4000000), from(0x9E000000), from(0x72000000), from(0x5C000000)};
}

// The band (win32-caption-tabs): WinUI's LayerOnMicaBaseAltFillColorDefault,
// #B3FFFFFF (dark #733A3A3A), at its own alpha over the extended frame,
// printed premultiplied: 0xB3B3B3B3, dark 0x731A1A1A; high contrast's window
// colour, opaque.
DWORD band_layer_px()
{
    if (high_contrast()) return 0xFF000000u | rgb(GetSysColor(COLOR_WINDOW));
    return g_dark ? 0x731A1A1Au : 0xB3B3B3B3u;
}
// The same family's opaque one, what a field's fill goes over (GDI's ground
// for the band: LayerOnMicaBaseAltFillColorTertiary), #F9F9F9, dark #2C2C2C;
// high contrast's window colour.
DWORD band_fill()
{
    if (high_contrast()) return rgb(GetSysColor(COLOR_WINDOW));
    return g_dark ? 0x2C2C2C : 0xF9F9F9;
}

// AccentFillColorDefault: the accent palette's Dark1 (Light2 dark), if the
// desktop has the palette (else unknown here: 0xFFFFFFFF).
DWORD accent_fill()
{
    BYTE p[32] = {};
    DWORD size = sizeof p;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, p, &size) != ERROR_SUCCESS ||
        size != sizeof p)
        return 0xFFFFFFFF;
    const BYTE* c = p + (g_dark ? 1 : 4) * 4;
    return (DWORD(c[0]) << 16) | (DWORD(c[1]) << 8) | c[2];
}

// -- the windows ----------------------------------------------------------

bool g_shown = true;  // shown: UI Automation's part (it leaves hidden windows out)

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

RECT rect_in(HWND top, HWND child)
{
    RECT r{};
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, top, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

// The window's EDITs, left to right.
std::vector<HWND> edits_of(HWND top)
{
    std::vector<HWND> out;
    for (HWND c = GetWindow(top, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
    {
        wchar_t cls[16]{};
        GetClassNameW(c, cls, 16);
        if (!_wcsicmp(cls, L"Edit")) out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [&](HWND a, HWND b) { return rect_in(top, a).left < rect_in(top, b).left; });
    return out;
}

// The field's own account (win32_band_field.h): 0 not a band field, else
// 1 | 2 drawn | 4 hovered | 8 focused.
int field_state(HWND edit)
{
    static const UINT m = RegisterWindowMessageW(L"rapport-band-field");
    return int(SendMessageW(edit, m, 0, 0));
}

struct printed
{
    std::vector<DWORD> px;
    int w = 0, h = 0;
    DWORD at(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0xDEADBEEF;
        return px[size_t(y) * w + x] & 0xffffff;
    }
    // The pixel as printed, alpha and all (premultiplied ARGB).
    DWORD raw(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0xDEADBEEF;
        return px[size_t(y) * w + x];
    }
};
printed into_bitmap(int w, int h, const std::function<void(HDC)>& paint)
{
    printed out;
    out.w = w;
    out.h = h;
    if (w <= 0 || h <= 0) return out;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    std::fill_n(static_cast<DWORD*>(bits), size_t(w) * h, 0x00ABCDEFu);
    paint(mem);
    GdiFlush();
    out.px.assign(static_cast<DWORD*>(bits), static_cast<DWORD*>(bits) + size_t(w) * h);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return out;
}
// A field printed whole, frame, text and buttons: WM_PRINT (its client area
// through WM_PRINTCLIENT), as a pane's copy prints it.
printed print_field(HWND edit)
{
    RECT r{};
    GetWindowRect(edit, &r);
    return into_bitmap(r.right - r.left, r.bottom - r.top,
                       [&](HDC dc)
                       {
                           SendMessageW(edit, WM_PRINT, reinterpret_cast<WPARAM>(dc),
                                        PRF_NONCLIENT | PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
                       });
}
// The window's background as its erase paints it: the band and its line.
printed print_erase(HWND top)
{
    RECT c{};
    GetClientRect(top, &c);
    return into_bitmap(c.right, c.bottom,
                       [&](HDC dc) { SendMessageW(top, WM_ERASEBKGND, reinterpret_cast<WPARAM>(dc), 0); });
}

int luma(DWORD v)
{
    return (channel(v, 16) * 299 + channel(v, 8) * 587 + channel(v, 0) * 114) / 1000;
}

// The ink in `area` of a printed field: pixels unlike `face`, their rows
// and columns, and the one furthest from the face (the ink's own colour,
// where a stem covers a whole pixel).
struct ink_found
{
    int top = -1, bottom = -1, left = -1, right = -1;
    DWORD strongest = 0;
    bool any() const { return top >= 0; }
};
ink_found ink(const printed& p, RECT area, DWORD face)
{
    ink_found f;
    int best = -1;
    for (int y = std::max(0L, area.top); y < std::min<long>(p.h, area.bottom); ++y)
        for (int x = std::max(0L, area.left); x < std::min<long>(p.w, area.right); ++x)
        {
            const DWORD v = p.at(x, y);
            const int d = std::abs(luma(v) - luma(face));
            if (d < 24) continue;
            if (f.top < 0 || y < f.top) f.top = y;
            if (y > f.bottom) f.bottom = y;
            if (f.left < 0 || x < f.left) f.left = x;
            if (x > f.right) f.right = x;
            if (d > best) best = d, f.strongest = v;
        }
    return f;
}

// -- the checks -------------------------------------------------------------

// The row: the band from the strip's bottom down band_metrics::row, then its
// line; the toolbar's buttons and each field band_metrics' size, the
// fields (row - field) / 2 below the strip.
void check_metrics(HWND top, const std::vector<HWND>& fields, UINT dpi, const std::string& when)
{
    const std::string at = " (" + when + ", " + str(long(dpi)) + " dpi)";
    HWND strip = strip_of(top);
    const int from = strip ? int(rect_in(top, strip).bottom) : 0;
    const int row = px(46, dpi), field = px(32, dpi), above = (row - field) / 2;
    const printed bg = print_erase(top);
    const DWORD fill = band_layer_px();
    int y = from;
    while (y < bg.h && bg.raw(bg.w - 2, y) == fill) ++y;
    expect(y - from == row, "the band " + str(row) + " px from the strip's bottom to its line" + at + ": " +
                                str(y - from) + " px, from " + str(from));
    for (HWND f : fields)
    {
        const RECT r = rect_in(top, f);
        expect(r.bottom - r.top == field && r.top - from == above && from + row - r.bottom == row - field - above,
               "a field " + str(field) + " px tall, " + str(above) + " px under the strip and " +
                   str(row - field - above) + " above the line" + at + ": " + str(r.bottom - r.top) + " tall at " +
                   str(r.top - from) + ", " + str(from + row - r.bottom) + " above the line");
    }
    // The navigation buttons, 40 by 32, 8 apart.
    HWND toolbar = FindWindowExW(top, nullptr, TOOLBARCLASSNAMEW, nullptr);
    expect(toolbar != nullptr, "the toolbar's buttons" + at);
    if (!toolbar) return;
    RECT b0{}, b1{};
    SendMessageW(toolbar, TB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&b0));
    SendMessageW(toolbar, TB_GETITEMRECT, 1, reinterpret_cast<LPARAM>(&b1));
    const RECT tb = rect_in(top, toolbar);
    expect(b0.right - b0.left == px(40, dpi) && b0.bottom - b0.top == px(32, dpi) &&
               b1.left - b0.right == px(8, dpi),
           "the navigation buttons " + str(px(40, dpi)) + " by " + str(px(32, dpi)) + ", " + str(px(8, dpi)) +
               " apart" + at + ": " + str(b0.right - b0.left) + " by " + str(b0.bottom - b0.top) + ", " +
               str(b1.left - b0.right) + " apart");
    expect(tb.top + b0.top - from == above && tb.left + b0.left == px(7, dpi),
           "the first one " + str(above) + " under the strip and " + str(px(7, dpi)) + " in" + at + ": " +
               str(tb.top + b0.top - from) + ", " + str(tb.left + b0.left));
    RECT c{};
    GetClientRect(top, &c);
    if (!fields.empty())
    {
        const RECT last = rect_in(top, fields.back());
        expect(c.right - last.right == px(11, dpi), "the last field ends " + str(px(11, dpi)) + " from the edge" + at +
                                                        ": " + str(c.right - last.right));
    }
    if (fields.size() > 1)
    {
        const RECT a = rect_in(top, fields[0]), b = rect_in(top, fields[1]);
        expect(b.left - a.right == px(8, dpi),
               "the fields " + str(px(8, dpi)) + " apart" + at + ": " + str(b.left - a.right));
    }
}

// Where the field's text goes (its formatting rectangle: clear of its
// buttons and its frame), in the field's window coordinates.
RECT text_area(HWND edit)
{
    RECT f{};
    SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&f));
    POINT o{0, 0};
    ClientToScreen(edit, &o);
    RECT w{};
    GetWindowRect(edit, &w);
    OffsetRect(&f, o.x - w.left, o.y - w.top);
    return f;
}

// The text's ink: its capitals' rows centred in the field to a pixel.
void check_centred(HWND edit, DWORD face, const std::string& what)
{
    const printed p = print_field(edit);
    const ink_found f = ink(p, text_area(edit), face);
    expect(f.any(), "the text's ink found" + what);
    if (!f.any()) return;
    const float mid = float(f.top + f.bottom) / 2.f, want = float(p.h - 1) / 2.f;
    expect(std::fabs(mid - want) <= 1.f, "its ink centred in the " + str(p.h) + " px field" + what + ": rows " +
                                             str(f.top) + ".." + str(f.bottom) + ", centre " +
                                             std::to_string(mid) + " for " + std::to_string(want));
}

// A field printed on the band: outside its rounded shape (each corner's
// outermost pixel) the band's layer exactly as the band prints it,
// premultiplied at its own alpha (not made opaque, which on the extended
// frame would be square corners of the opaque ground over Mica Alt); along
// its rounded top-left edge, antialiased between the layer and the field,
// never less opaque than the layer and each channel no more than its alpha
// (premultiplied); inside the shape, round the text (the padding above it),
// the field's own fill, opaque.
void check_corners(const printed& p, DWORD face, const std::string& what)
{
    const DWORD layer = band_layer_px();
    const POINT corners[] = {{0, 0}, {p.w - 1, 0}, {0, p.h - 1}, {p.w - 1, p.h - 1}};
    bool all = true;
    std::string seen;
    for (const POINT c : corners)
    {
        all = all && p.raw(c.x, c.y) == layer;
        seen += " " + argb_hex(p.raw(c.x, c.y));
    }
    expect(all, "its corners the band's layer, " + argb_hex(layer) + " (premultiplied)" + what + ":" + seen);
    const int la = int(layer >> 24);
    bool premultiplied = true, between = false;
    for (int y = 0; y < 4 && y < p.h; ++y)
        for (int x = 0; x < 4 && x < p.w; ++x)
        {
            const DWORD v = p.raw(x, y);
            const int a = int(v >> 24);
            premultiplied = premultiplied && a >= la && channel(v, 16) <= a && channel(v, 8) <= a &&
                            channel(v, 0) <= a;
            between = between || (a > la && a < 255);
        }
    expect(premultiplied && (between || la == 255),
           "its rounded edge antialiased over the layer, premultiplied" + what + ": " + argb_hex(p.raw(1, 0)) +
               ", " + argb_hex(p.raw(0, 1)) + ", " + argb_hex(p.raw(1, 1)));
    const DWORD inside = p.raw(p.w * 3 / 4, 2), middle = p.raw(p.w * 3 / 4, p.h / 2);
    expect(inside == (0xFF000000u | face) && middle == (0xFF000000u | face),
           "inside its shape its own fill, opaque, round the text and by it" + what + ": " + argb_hex(inside) +
               ", " + argb_hex(middle));
}

// A field's printed look: its corners the band's (rounded to it), its edge
// the border or the fill, its fill, its bottom the accent when focused; the
// text's ink as expected.
struct look
{
    DWORD face, edge, bottom;
    DWORD ink;  // the strongest ink expected
    bool accent = false;
};
void check_look(HWND edit, const look& want, const std::string& what)
{
    const printed p = print_field(edit);
    const int mid = p.w * 3 / 4;  // past the text, before a button
    check_corners(p, want.face, what);
    expect(p.at(mid, 0) == want.edge, "its top edge " + hex(want.edge) + what + ": " + hex(p.at(mid, 0)));
    expect(p.at(mid, 2) == want.face && p.at(mid, p.h / 2) == want.face,
           "its fill " + hex(want.face) + ", opaque" + what + ": " + hex(p.at(mid, 2)) + ", " +
               hex(p.at(mid, p.h / 2)));
    if (want.accent)
    {
        const DWORD a = p.at(mid, p.h - 1), b = p.at(mid, p.h - 2), c = p.at(mid, p.h - 3);
        const bool known = want.bottom != 0xFFFFFFFF;
        expect((known ? a == want.bottom && b == want.bottom : a == b && a != want.face && a != want.edge) &&
                   c == want.face,
               "its bottom 2 px the accent" + (known ? ", " + hex(want.bottom) : std::string()) + what + ": " +
                   hex(a) + ", " + hex(b) + ", then " + hex(c));
    }
    else
        expect(p.at(mid, p.h - 1) == want.bottom,
               "its bottom edge " + hex(want.bottom) + what + ": " + hex(p.at(mid, p.h - 1)));
    // The text, from its strongest pixel: within a few levels of WinUI's
    // colour over the fill (ClearType may tint a stem's edge, not its middle).
    const ink_found f = ink(p, text_area(edit), want.face);
    expect(f.any() && std::abs(luma(f.strongest) - luma(want.ink)) <= 16,
           "its text " + hex(want.ink) + what + ": strongest " + hex(f.strongest));
}

void check_states(HWND top, rap::entry* address, HWND edit, HWND search)
{
    const field_colors k = colours();
    const DWORD behind = band_fill();
    const DWORD rest = over(k.rest, behind), hover = over(k.hover, behind), focus = over(k.focus, behind),
                disabled = over(k.disabled, behind);
    const DWORD stroke = over(k.stroke, behind);
    const std::string theme = g_dark ? "dark" : "light";
    expect(field_state(edit) == 3 && field_state(search) == 3, "both fields drawn band fields (" + theme + ")");
    expect(!(GetWindowLongPtrW(edit, GWL_EXSTYLE) & WS_EX_CLIENTEDGE), "with no native edge (" + theme + ")");

    check_look(edit, {rest, rest, rest, over(k.text, rest)}, " (at rest, " + theme + ")");
    check_centred(edit, rest, " (at rest, " + theme + ")");

    // Hovered: a WM_MOUSEMOVE sent, printed before the pointer's found
    // elsewhere (the leave that follows is posted).
    SendMessageW(edit, WM_MOUSEMOVE, 0, MAKELPARAM(4, 4));
    expect((field_state(edit) & 4) != 0, "hovered (" + theme + ")");
    check_look(edit, {hover, stroke, stroke, over(k.text, hover)}, " (hovered, " + theme + ")");
    pump(2);
    SendMessageW(edit, WM_MOUSELEAVE, 0, 0);
    expect((field_state(edit) & 4) == 0, "and not, the pointer elsewhere (" + theme + ")");

    // Focused: WM_SETFOCUS sent, then WM_KILLFOCUS.
    SendMessageW(edit, WM_SETFOCUS, 0, 0);
    expect((field_state(edit) & 8) != 0, "focused (" + theme + ")");
    look f{focus, stroke, accent_fill(), over(k.text, focus), true};
    check_look(edit, f, " (focused, " + theme + ")");
    check_centred(edit, focus, " (focused, " + theme + ")");
    SendMessageW(edit, WM_KILLFOCUS, 0, 0);
    expect((field_state(edit) & 8) == 0, "and not (" + theme + ")");

    // Read-only: as at rest.
    address->set_read_only(true);
    check_look(edit, {rest, rest, rest, over(k.text, rest)}, " (read-only, " + theme + ")");
    address->set_read_only(false);

    // Disabled: its fill, the border, the text TextFillColorDisabled.
    address->set_enabled(false);
    check_look(edit, {disabled, stroke, stroke, over(k.text_disabled, disabled)}, " (disabled, " + theme + ")");
    check_centred(edit, disabled, " (disabled, " + theme + ")");
    address->set_enabled(true);

    // Empty, its placeholder in TextFillColorSecondary (Tertiary focused).
    check_look(search, {rest, rest, rest, over(k.placeholder, rest)}, " (placeholder, " + theme + ")");
    check_centred(search, rest, " (placeholder, " + theme + ")");
    SendMessageW(search, WM_SETFOCUS, 0, 0);
    check_look(search, {focus, stroke, accent_fill(), over(k.placeholder_focused, focus), true},
               " (placeholder focused, " + theme + ")");
    SendMessageW(search, WM_KILLFOCUS, 0, 0);
    (void)top;
}

// The search field: its leading button where Explorer's glyph sits, drawn on
// its fill; the clear button there while there's text; both working.
void check_search(HWND search, rap::entry* entry, const int& searched, std::string& changed)
{
    const UINT dpi = GetDpiForWindow(search);
    const DWORD rest = over(colours().rest, band_fill());
    // Its children, the buttons, by their text (their tooltips).
    HWND clear = nullptr, glyph = nullptr;
    for (HWND c = GetWindow(search, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
    {
        wchar_t text[32]{};
        GetWindowTextW(c, text, 32);
        if (!wcscmp(text, L"Clear")) clear = c;
        if (!wcscmp(text, L"Search")) glyph = c;
    }
    expect(glyph && clear, "the search field's search and clear buttons");
    if (!glyph || !clear) return;
    const printed p = print_field(search);
    // The search button's columns, in the field's window coordinates.
    RECT w{}, g{};
    GetWindowRect(search, &w);
    GetWindowRect(glyph, &g);
    const RECT gr{g.left - w.left, 0, g.right - w.left, p.h};
    const ink_found f = ink(p, gr, rest);
    const float centre = f.any() ? float(f.left + f.right) / 2.f : -1.f;
    expect(f.any() && std::fabs(centre - float(px(18, dpi))) <= 2.f,
           "the search glyph drawn, centred " + str(px(18, dpi)) + " in, as Explorer's: " + std::to_string(centre));
    expect(p.at(gr.left + 1, p.h / 2) == rest, "on the field's fill: " + hex(p.at(gr.left + 1, p.h / 2)));
    // Its click: what the button sends the field.
    const int before = searched;
    SendMessageW(search, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(glyph), BN_CLICKED), reinterpret_cast<LPARAM>(glyph));
    expect(searched == before + 1, "the search button still does its action");
    // The clear button: hidden while empty, there with text, clearing it.
    expect(!(GetWindowLongPtrW(clear, GWL_STYLE) & WS_VISIBLE), "no clear button while it's empty");
    entry->set_text("Hi");
    expect((GetWindowLongPtrW(clear, GWL_STYLE) & WS_VISIBLE) != 0, "the clear button, with text");
    RECT cr{}, sc{};
    GetWindowRect(clear, &cr);
    GetClientRect(search, &sc);
    POINT so{0, 0};
    ClientToScreen(search, &so);
    expect(cr.right == so.x + sc.right && cr.right - cr.left == px(28, dpi),
           "at the text's end, " + str(px(28, dpi)) + " wide: " + str(cr.right - cr.left));
    changed = "?";
    SendMessageW(search, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(clear), BN_CLICKED), reinterpret_cast<LPARAM>(clear));
    expect(entry->text().empty() && changed.empty(), "the clear button empties it, on_changed told");
    expect(!(GetWindowLongPtrW(clear, GWL_STYLE) & WS_VISIBLE), "and goes");
}

// A framed control on the band that isn't a band field (a spin's EDIT, its
// native client edge), printed whole (WM_PRINT with PRF_NONCLIENT, as a
// pane's copy prints it): its client area inside its frame, at the client
// area's offset in the window, exactly as it prints on its own
// (WM_PRINTCLIENT) and opaque; its frame our border, as it prints alone
// (WM_PRINT, PRF_NONCLIENT only), nothing of the bitmap left unpainted.
// (Without the frame, DefWindowProc would put the client area at the DC's
// origin, under the frame, its last columns and rows never painted.)
void check_framed_print(HWND edit)
{
    expect(field_state(edit) == 0, "the spin's EDIT not a band field: " + str(field_state(edit)));
    expect((GetWindowLongPtrW(edit, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) != 0, "with its client edge");
    RECT w{}, c{};
    GetWindowRect(edit, &w);
    GetClientRect(edit, &c);
    POINT o{0, 0};
    ClientToScreen(edit, &o);
    const int cx = o.x - w.left, cy = o.y - w.top;
    expect(cx > 0 && cy > 0 && c.right > 0 && c.bottom > 0,
           "its client area inside its frame, at " + str(cx) + "," + str(cy));
    if (cx <= 0 || cy <= 0 || c.right <= 0 || c.bottom <= 0) return;
    const printed client = into_bitmap(c.right, c.bottom,
                                       [&](HDC dc)
                                       {
                                           SendMessageW(edit, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc),
                                                        PRF_CLIENT | PRF_ERASEBKGND);
                                       });
    const printed whole = print_field(edit);
    const printed frame = into_bitmap(whole.w, whole.h,
                                      [&](HDC dc)
                                      {
                                          SendMessageW(edit, WM_PRINT, reinterpret_cast<WPARAM>(dc),
                                                       PRF_NONCLIENT);
                                      });
    // Its text (right-aligned, so where a shift shows) is there to compare.
    const ink_found f = ink(client, RECT{0, 0, c.right, c.bottom}, client.at(1, 1));
    expect(f.any(), "its value printed in its client area");
    int moved = 0, clear = 0, unpainted = 0, border = 0;
    POINT first{-1, -1};
    for (int y = 0; y < whole.h; ++y)
        for (int x = 0; x < whole.w; ++x)
        {
            const DWORD v = whole.raw(x, y);
            if (v == 0x00ABCDEFu) ++unpainted;
            const bool inside = x >= cx && x < cx + c.right && y >= cy && y < cy + c.bottom;
            if (inside)
            {
                if (v != client.raw(x - cx, y - cy))
                {
                    if (!moved++) first = {x, y};
                }
                if ((v >> 24) != 255) ++clear;
            }
            else if (v != frame.raw(x, y))
                ++border;
        }
    expect(moved == 0, "its client area printed at " + str(cx) + "," + str(cy) + ", as it prints alone: " +
                           str(moved) + " pixels differ" +
                           (moved ? ", the first at " + str(first.x) + "," + str(first.y) + ": " +
                                        argb_hex(whole.raw(first.x, first.y)) + ", not " +
                                        argb_hex(client.raw(first.x - cx, first.y - cy))
                                  : std::string()));
    expect(clear == 0, "its client area opaque: " + str(clear) + " pixels not");
    expect(border == 0, "its frame our border, as printed alone: " + str(border) + " pixels differ");
    expect(unpainted == 0, "nothing left unpainted: " + str(unpainted) + " pixels");
}

// Under high contrast: nothing drawn; the native bordered EDIT, the band's
// height, its text centred on the system's window colour.
void check_high_contrast(HWND edit, UINT dpi)
{
    expect(field_state(edit) == 1, "a band field, not drawn: " + str(field_state(edit)));
    expect((GetWindowLongPtrW(edit, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) != 0, "with the native edge");
    RECT r{};
    GetWindowRect(edit, &r);
    expect(r.bottom - r.top == px(32, dpi), "the band's height, " + str(px(32, dpi)) + ": " + str(r.bottom - r.top));
    const DWORD window = rgb(GetSysColor(COLOR_WINDOW));
    const printed p = print_field(edit);
    expect(p.at(p.w * 3 / 4, p.h / 2) == window, "on the window colour, " + hex(window) + ": " +
                                                      hex(p.at(p.w * 3 / 4, p.h / 2)));
    // Its padding (inside the edge, round the client area) too.
    expect(p.at(p.w * 3 / 4, px(4, dpi)) == window, "its padding too: " + hex(p.at(p.w * 3 / 4, px(4, dpi))));
    check_centred(edit, window, " (high contrast)");
}

// -- UI Automation ----------------------------------------------------------

template <class T>
void release(T*& p)
{
    if (p) p->Release();
    p = nullptr;
}

// An EDIT as UIA has it: its control type, name and value.
struct uia_edit
{
    bool found = false;
    CONTROLTYPEID type = 0;
    std::wstring name, value;
};
uia_edit uia_of(IUIAutomation* uia, HWND edit)
{
    uia_edit out;
    IUIAutomationElement* e = nullptr;
    if (uia) uia->ElementFromHandle(edit, &e);
    if (!e) return out;
    out.found = true;
    e->get_CurrentControlType(&out.type);
    BSTR name = nullptr;
    if (SUCCEEDED(e->get_CurrentName(&name)) && name) out.name = name;
    SysFreeString(name);
    IUIAutomationValuePattern* value = nullptr;
    e->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern), reinterpret_cast<void**>(&value));
    BSTR v = nullptr;
    if (value && SUCCEEDED(value->get_CurrentValue(&v)) && v) out.value = v;
    SysFreeString(v);
    release(value);
    release(e);
    return out;
}

// The band's address field to UIA: an Edit, its name what the same field
// has without the band (its label's, by the native control's own rules),
// its value its text.
void uia_client(HWND edit, HWND plain, std::atomic<bool>* finished)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                     reinterpret_cast<void**>(&uia));
    expect(uia != nullptr, "a UIA client");
    const uia_edit band = uia_of(uia, edit), native = uia_of(uia, plain);
    expect(band.found && band.type == UIA_EditControlTypeId, "the band field is an Edit to UIA");
    auto utf8 = [](const std::wstring& w)
    {
        std::string s(w.size() * 3 + 1, '\0');
        s.resize(size_t(std::max(0, WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(),
                                                        int(s.size()), nullptr, nullptr))));
        return s;
    };
    expect(!band.name.empty() && band.name == native.name,
           "named as the plain field is (\"" + utf8(band.name) + "\", \"" + utf8(native.name) + "\")");
    expect(band.name == L"Address", "by its label, Address");
    expect(band.value == L"C:\\Band", "its value its text, C:\\Band");
    release(uia);
    CoUninitialize();
    *finished = true;
}

// At a DPI the window has (through the hook or for real): the row's metrics,
// the fields' text centred, their corners and fill.
void check_at_dpi(HWND top, const std::vector<HWND>& fields, UINT dpi, const std::string& how)
{
    const bool hc = high_contrast();
    const std::string at = " (" + str(long(dpi)) + " dpi, " + how + ")";
    check_metrics(top, fields, dpi, (hc ? "high contrast, " : g_dark ? "dark, " : "light, ") + how);
    if (hc)
    {
        check_centred(fields[0], rgb(GetSysColor(COLOR_WINDOW)), at);
        return;
    }
    const DWORD rest = over(colours().rest, band_fill());
    check_centred(fields[0], rest, at);
    check_centred(fields[1], rest, " (placeholder," + at.substr(1));
    const printed p = print_field(fields[0]);
    expect(p.at(p.w * 3 / 4, p.h / 2) == rest, "its fill" + at + ": " + hex(p.at(p.w * 3 / 4, p.h / 2)));
    check_corners(p, rest, at);
}

// At a text size (Settings > Accessibility > Text size) of `scale`: the
// fields' text WinUI's 14 enlarged by it, as the message font is (21 at
// 150%), not put back to 14; each field tall enough for its line (32 at
// 100%, WinUI's padding and border around the text beyond), the text's ink
// centred in it, and the row the field's height and 7 above and below.
void check_text_size(HWND top, const std::vector<HWND>& fields, UINT dpi, float scale)
{
    const std::string at = " (text size " + str(std::lround(scale * 100)) + "%, " + str(long(dpi)) + " dpi)";
    HWND strip = strip_of(top);
    const int from = strip ? int(rect_in(top, strip).bottom) : 0;
    const int em = int(std::lround(14.f * scale * float(dpi) / 96.f));
    int tallest = 0;
    for (HWND f : fields)
    {
        LOGFONTW lf{};
        GetObjectW(reinterpret_cast<HFONT>(SendMessageW(f, WM_GETFONT, 0, 0)), sizeof lf, &lf);
        expect(std::abs(std::abs(lf.lfHeight) - em) <= 1,
               "its text " + str(em) + " px" + at + ": " + str(std::abs(lf.lfHeight)));
        HDC dc = GetDC(f);
        HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(f, WM_GETFONT, 0, 0)));
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        ReleaseDC(f, dc);
        const RECT r = rect_in(top, f);
        const int want = std::max(px(32, dpi), int(tm.tmHeight) + px(5 + 6 + 2, dpi));
        expect(r.bottom - r.top == want, "the field " + str(want) + " px tall for its " + str(tm.tmHeight) +
                                             " px line" + at + ": " + str(r.bottom - r.top));
        const RECT t = text_area(f);
        expect(t.bottom - t.top >= tm.tmHeight,
               "its line not clipped" + at + ": " + str(t.bottom - t.top) + " px for " + str(tm.tmHeight));
        tallest = std::max(tallest, int(r.bottom - r.top));
        const RECT n = rect_in(top, fields[0]);
        expect(r.top - from == (px(46, dpi) - px(32, dpi)) / 2 && r.top == n.top,
               "under the strip by " + str((px(46, dpi) - px(32, dpi)) / 2) + at + ": " + str(r.top - from));
    }
    if (scale > 1.f)
        expect(tallest > px(32, dpi),
               "the fields taller than " + str(px(32, dpi)) + " px" + at + ": " + str(tallest));
    const int row = tallest + px(46, dpi) - px(32, dpi);
    const printed bg = print_erase(top);
    const DWORD fill = band_layer_px();
    int y = from;
    while (y < bg.h && bg.raw(bg.w - 2, y) == fill) ++y;
    expect(y - from == row, "the band " + str(row) + " px from the strip to its line" + at + ": " + str(y - from));
    if (!high_contrast())
    {
        const DWORD rest = over(colours().rest, band_fill());
        check_centred(fields[0], rest, at);
    }
}

// -- the display's scale, for real (CI only) ----------------------------------
//
// As win32-caption-tabs-transitions changes it: the primary display's scale
// through DisplayConfig's undocumented DPI scale (as Settings sets it, in
// steps relative to the recommended one), its mode made larger first where a
// small screen allows only 100%, and both put back exactly as they were,
// read back to be sure, on every path.

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

// The display as it was before the first change, put back to exactly that
// (main undoes it, the destructor if anything returns first); each restore
// read back, bounded, and one that fails a failure, left marked.
struct restore
{
    bool saved = false;
    std::wstring device;
    DEVMODEW mode{};
    int32_t rel = 0;
    bool scale = false, mode_changed = false;  // changed and not yet put back

    ~restore() { undo(); }

    bool save()
    {
        if (saved) return true;
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
        dpi_scale_get g{};
        if (!found || !source_of(dev.DeviceName, g_source) ||
            !EnumDisplaySettingsW(dev.DeviceName, ENUM_CURRENT_SETTINGS, &mode) || !get_scale(g))
            return false;
        device = dev.DeviceName;
        rel = g.cur_rel;
        saved = true;
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
    template <class F>
    static bool settles(F&& is)
    {
        for (int waited = 0; waited < 5000; waited += 50)
        {
            if (is()) return true;
            pump(5);
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
    }
};
restore g_restore;

// Every scale the display allows, for real, the window's DPI with each: the
// fields checked at each (125% and 150% where the runner reaches them), then
// the scale put back. Which ran is noted.
int scale_transitions(HWND top, const std::vector<HWND>& fields)
{
    if (!g_restore.save())
    {
        expect(false, "the display's mode and scale read before changing them");
        return 1;
    }
    g_restore.scale = true;  // armed before anything changes
    dpi_scale_get g{};
    if (!get_scale(g))
    {
        expect(false, "the display's scale read");
        return 1;
    }
    note("display scale steps " + str(g.min_rel) + ".." + str(g.max_rel) + ", now " + str(g.cur_rel));
    // A small screen (a runner's 1024x768) allows only 100%: its largest
    // mode first, for the scales it allows.
    if (g.max_rel <= g.cur_rel + 1)
    {
        const wchar_t* device = g_restore.device.c_str();
        DEVMODEW best{}, mode{};
        best.dmSize = mode.dmSize = sizeof(DEVMODEW);
        for (DWORD i = 0; EnumDisplaySettingsW(device, i, &mode); ++i)
            if (mode.dmPelsWidth * mode.dmPelsHeight > best.dmPelsWidth * best.dmPelsHeight) best = mode;
        if (best.dmPelsWidth && best.dmPelsWidth * best.dmPelsHeight >
                                    g_restore.mode.dmPelsWidth * g_restore.mode.dmPelsHeight)
        {
            g_restore.mode_changed = true;  // marked before it's tried: half made is put back too
            if (ChangeDisplaySettingsExW(device, &best, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL)
            {
                note("display mode " + str(long(best.dmPelsWidth)) + "x" + str(long(best.dmPelsHeight)));
                pump(50);
                get_scale(g);
                note("display scale steps " + str(g.min_rel) + ".." + str(g.max_rel) + ", now " + str(g.cur_rel));
            }
        }
    }
    const UINT before = GetDpiForWindow(top);
    std::string ran;
    bool at_125 = false, at_150 = false;
    UINT now = before;
    for (int32_t rel = g.min_rel; rel <= g.max_rel; ++rel)
    {
        if (rel == g.cur_rel) continue;
        if (!set_scale(rel))
        {
            expect(false, "the display's scale set to step " + str(rel));
            continue;
        }
        const UINT was = now;
        pump_until([&] { return GetDpiForWindow(top) != was; });
        pump();
        now = GetDpiForWindow(top);
        if (now == was) continue;
        ran += (ran.empty() ? "" : ", ") + str(long(now * 100 / 96)) + "%";
        at_125 = at_125 || now == 120;
        at_150 = at_150 || now == 144;
        check_at_dpi(top, fields, now, "the display's scale");
    }
    note("scales that ran: " + (ran.empty() ? std::string("none") : ran));
    expect(!ran.empty(), "the display allows another scale");
    expect(at_125, "125% among them");
    if (!at_150) note("150% not reached on this display");
    g_restore.undo();
    expect(pump_until([&] { return GetDpiForWindow(top) == before; }), "and back to " + str(long(before)));
    pump();
    check_at_dpi(top, fields, before, "the scale back");
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    bool transitions = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--transitions") transitions = true;
    const DWORD build = windows_build();
    // --transitions changes the display's scale: on a CI runner only, and
    // checked before any window is made.
    if (transitions && (env("GITHUB_ACTIONS") != "true" || env("RAPPORT_TEST_SET_DPI") != "1"))
    {
        note("skipped the scale changes: they change the display's scale, so only on a CI runner "
             "(GITHUB_ACTIONS=true, RAPPORT_TEST_SET_DPI=1)");
        return 77;
    }
    if (transitions && build < 22000)
    {
        note("skipped the scale changes: no band before Windows 11 (build 22000)");
        return 77;
    }
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    // The text size taken to be 100% (through its test hook: the setting
    // itself untouched), whatever the desktop's: the metrics are 100%'s.
    rap::win32::set_text_scale_for_tests(1.f);
#endif
    rap::load_options options{.backend = "win32"};
    options.look.color_scheme = rap::color_scheme::dark;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 2;
    const bool hc = high_contrast();
    g_dark = !hc && build >= 17763 && env("RAPPORT_THEME") != "light";
    std::printf("build %lu, %s\n", build, hc ? "high contrast" : g_dark ? "dark" : "light");

    using namespace rap::layout;
    rap::entry* address = nullptr;
    rap::entry* search = nullptr;
    int searched = 0;
    std::string changed;
    auto bar = [&](bool bound)
    {
        auto a = entry{"C:\\Band"}.stretch();
        auto s = entry{}
                     .placeholder("FIND")  // capitals: their ink, flat, says where the text sits
                     .leading_action(rap::icon::named("system-search"), "Search", [&searched] { ++searched; })
                     .clearable()
                     .on_changed([&changed](const std::string& t) { changed = t; })
                     .width(300_px);
        if (bound)
        {
            a.bind(address);
            s.bind(search);
        }
        return toolbar{tool_button{rap::icon::named("go-previous"), "Back"},
                       tool_button{rap::icon::named("go-next"), "Forward"},
                       separator{},
                       label{"Address"},
                       a,
                       s};
    };
    auto tabs = [] { return document_tabs{{{"a", "Alpha", rap::icon::named("folder"), {}}, {"b", "Beta", {}, {}}}}; };
    const char* title = "Band fields";
    auto win = (*ui)->create(window{title, {bar(true), label{"The current tab's content"}}}
                                 .size(1000_px, 300_px)
                                 .margins(false)
                                 .base_surface()
                                 .tabs(tabs()));
    if (g_shown) show(*win);
    pump();
    HWND top = top_of(title);
    expect(top != nullptr, "the window");
    if (!top) return 1;
    std::vector<HWND> fields = edits_of(top);
    expect(fields.size() == 2 && address && search, "its two fields");
    if (fields.size() != 2 || !address || !search) return 1;

    // A window without a base surface: no band, its fields and toolbar plain.
    const char* plain_title = "Band fields, no base surface";
    auto plain = (*ui)->create(window{plain_title, {bar(false), label{"No band"}}}
                                   .size(1000_px, 300_px)
                                   .margins(false)
                                   .tabs(tabs()));
    if (g_shown) show(*plain);
    pump();
    if (HWND p = top_of(plain_title))
    {
        bool none = true;
        for (HWND f : edits_of(p))
            none = none && field_state(f) == 0 && (GetWindowLongPtrW(f, GWL_EXSTYLE) & WS_EX_CLIENTEDGE);
        expect(none, "without a base surface, its fields plain EDITs with their native edge");
        RECT r = rect_in(p, edits_of(p).front());
        const UINT dpi = GetDpiForWindow(p);
        expect(r.bottom - r.top < px(32, dpi), "at their own height: " + str(r.bottom - r.top));
    }
    else
        expect(false, "the window without a base surface");

    if (build < 22000)
    {
        // Before Windows 11: no band, the fields as ever.
        bool none = true;
        for (HWND f : fields) none = none && field_state(f) == 0;
        expect(none, "before Windows 11, no band fields");
        std::printf(fails ? "%d failed\n" : "all passed\n", fails.load());
        return fails ? 1 : 0;
    }

    const UINT dpi = GetDpiForWindow(top);
    if (transitions)
    {
        const int r = scale_transitions(top, fields);
        g_restore.undo();
        std::printf(fails ? "%d failed\n" : "all passed\n", fails.load());
        return fails ? 1 : r;
    }
    check_metrics(top, fields, dpi, hc ? "high contrast" : g_dark ? "dark" : "light");
    if (hc)
        check_high_contrast(fields[0], dpi);
    else
    {
        check_states(top, address, fields[0], fields[1]);
        check_search(fields[1], search, searched, changed);
        // A framed control on the band that isn't a band field: a spin.
        const char* framed_title = "Band fields, a framed control";
        auto framed = (*ui)->create(window{framed_title, {toolbar{label{"Size"}, spin{0, 99999, 88888}},
                                                          label{"A spin on the band"}}}
                                        .size(600_px, 200_px)
                                        .margins(false)
                                        .base_surface()
                                        .tabs(tabs()));
        if (g_shown) show(*framed);
        pump();
        HWND ft = top_of(framed_title);
        const std::vector<HWND> spins = ft ? edits_of(ft) : std::vector<HWND>{};
        expect(spins.size() == 1, "the window with a spin on its band");
        if (spins.size() == 1) check_framed_print(spins.front());
    }

#ifdef RAPPORT_TEST_BUILTIN_WIN32
    // 125% and 150%, through the DPI test hook: every window taken to be at
    // that DPI, and sent WM_DPICHANGED as Windows sends it. A font change
    // with it: the fields measured again in WinUI's size at that DPI.
    UINT was = dpi;
    for (const UINT to : {120u, 144u, dpi})
    {
        rap::win32::set_dpi_for_tests(to == dpi ? 0 : to);
        RECT wr{};
        GetWindowRect(top, &wr);
        RECT suggested{wr.left, wr.top, wr.left + MulDiv(wr.right - wr.left, int(to), int(was)),
                       wr.top + MulDiv(wr.bottom - wr.top, int(to), int(was))};
        was = to;
        SendMessageW(top, WM_DPICHANGED, MAKEWPARAM(to, to), reinterpret_cast<LPARAM>(&suggested));
        pump();
        check_at_dpi(top, fields, to, "the DPI hook");
    }

    // A larger text size, through its test hook (the setting untouched):
    // the fields' font made again, larger, and the fields and row taller
    // for it. Each step at a DPI other than the last's (the window's own
    // and 125%, or 150% where that's its own), so WM_DPICHANGED sets the
    // font and resizes the window, which lays it out: as a text size the
    // window takes as it's made, or with a move to another display.
    const UINT other = dpi == 120 ? 144u : 120u;
    for (const float scale : {1.5f, 2.25f, 1.f})
        for (const UINT to : {other, dpi})
        {
            rap::win32::set_text_scale_for_tests(scale);
            rap::win32::set_dpi_for_tests(to == dpi ? 0 : to);
            RECT wr{};
            GetWindowRect(top, &wr);
            RECT suggested{wr.left, wr.top, wr.left + MulDiv(wr.right - wr.left, int(to), int(was)),
                           wr.top + MulDiv(wr.bottom - wr.top, int(to), int(was))};
            was = to;
            SendMessageW(top, WM_DPICHANGED, MAKEWPARAM(to, to), reinterpret_cast<LPARAM>(&suggested));
            pump();
            check_text_size(top, fields, to, scale);
        }
    check_metrics(top, fields, dpi, "text size 100% again");
#else
    note("skipped 125% and 150%, and larger text sizes: the DPI and text size test hooks are the static backend's");
#endif

    // UI Automation: only shown (it leaves hidden windows' content out).
    HWND plain_top = top_of(plain_title);
    if (g_shown && plain_top)
    {
        std::atomic<bool> finished{false};
        std::thread client(uia_client, fields[0], edits_of(plain_top).front(), &finished);
        const ULONGLONG deadline = GetTickCount64() + 60000;
        while (!finished && GetTickCount64() < deadline) pump(1);
        if (!finished)
        {
            expect(false, "the UIA client finished within a minute");
            std::fflush(stdout);
            ExitProcess(1);
        }
        client.join();
    }
    else
        note("skipped UI Automation: the window isn't shown");

    std::printf(fails ? "%d failed\n" : "all passed\n", fails.load());
    return fails ? 1 : 0;
}
