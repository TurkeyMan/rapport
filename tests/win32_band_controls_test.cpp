// The toolbar band's controls, on Windows 11 (the tabs in the caption, a base
// surface), at both of its densities: every control in the band's row the
// field's height (WinUI's text box's: 32, compact 24), in a row 46 tall
// (compact 36), the row a toolbar or a row of controls (an hbox).
//
// Each control is printed (WM_PRINT, through the band's glass) into a 32-bit
// bitmap, never shown, light and dark (RAPPORT_THEME): a push button as
// WinUI's Button (ControlFillColorDefault opaque over the band's colour, the
// elevation border's ControlStrokeColorDefault along its top and, light,
// ...Secondary along its bottom; ControlFillColorTertiary pressed,
// ...Disabled disabled; WinUI's focus visual on its edge only while focus
// cues are shown), its corners the band's layer; a menu button as its
// DropDownButton (the same, with a chevron); a drop-down list as ComboBox's
// closed face (the same fill, a chevron at its right); an editable combo
// box as a band field (ControlFillColorDefault, at rest WinUI's
// TextControlElevationBorderBrush round it, its bottom ControlStrong-
// StrokeColorDefault; focused,
// ControlFillColorInputActive and the accent along its bottom); a tool
// button as an AppBarButton (the band's layer at rest, SubtleFillColor-
// Secondary over it hovered), a checked toggle in the accent; a separator a
// line the field's height in DividerStrokeColorDefault. Labels and check
// boxes stay native, centred in the row. Under high contrast
// (rapport-win32-high-contrast-run, CI only): none of it drawn, the native
// controls at the band's heights.
//
// A static build checks both densities through the density's test hook; a
// plugin build the one RAPPORT_DENSITY says (win32-band-controls-compact).
//   rapport-win32-band-controls-test
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which they need.
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
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
void set_text_scale_for_tests(
    float scale);  // win32_common.h (which wants the backend's include paths)
void set_band_density_for_tests(int compact);  // win32_band_field.h
void set_dpi_for_tests(UINT dpi);              // win32_common.h
}  // namespace rap::win32
#endif

namespace
{

int fails = 0;

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

std::string str(long v)
{
    return std::to_string(v);
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

int px(float logical, UINT dpi)
{
    return int(std::lround(logical * float(dpi) / 96.f));
}

int channel(DWORD v, int shift)
{
    return int((v >> shift) & 0xff);
}
int luma(DWORD v)
{
    return (channel(v, 16) * 299 + channel(v, 8) * 587 + channel(v, 0) * 114) / 1000;
}

// -- what's expected ------------------------------------------------------

bool g_dark = false;

// WinUI's colours (#AARRGGBB), as backends/win32/win32_band_face.cpp has
// them, each over what's under it.
struct argb
{
    DWORD rgb;  // 0xRRGGBB
    int a;
};
argb from(DWORD v)
{
    return {v & 0xffffff, int(v >> 24)};
}
DWORD over(argb top, DWORD under)
{
    DWORD out = 0;
    for (int shift : {16, 8, 0})
        out |=
            DWORD((channel(top.rgb, shift) * top.a + channel(under, shift) * (255 - top.a) + 127) /
                  255)
            << shift;
    return out;
}
// ControlFillColorDefault, ...Tertiary (pressed), ...Disabled; the
// elevation border's ControlStrokeColorDefault and ...Secondary;
// SubtleFillColorSecondary; ControlFillColorInputActive (a focused field).
struct face_colors
{
    argb fill, pressed, disabled, stroke, stroke_secondary, subtle_hover, input_active;
};
face_colors colours()
{
    if (g_dark)
        return {from(0x0FFFFFFF), from(0x08FFFFFF), from(0x0BFFFFFF), from(0x12FFFFFF),
                from(0x18FFFFFF), from(0x0FFFFFFF), from(0xB31E1E1E)};
    return {from(0xB3FFFFFF), from(0x4DF9F9F9), from(0x4DF9F9F9), from(0x0F000000),
            from(0x29000000), from(0x09000000), from(0xFFFFFFFF)};
}

// The band's layer as printed (premultiplied: LayerOnMicaBaseAltFillColor-
// Default at its own alpha), and the opaque colour of it a fill goes over
// (...Tertiary), as win32_band_fields_test has them.
DWORD band_layer_px()
{
    return g_dark ? 0x731A1A1Au : 0xB3B3B3B3u;
}
DWORD band_fill()
{
    return g_dark ? 0x2C2C2C : 0xF9F9F9;
}

// AccentFillColorDefault: the accent palette's Dark1 (Light2 dark), if the
// desktop has the palette (else unknown: 0xFFFFFFFF).
DWORD accent_fill()
{
    BYTE p[32] = {};
    DWORD size = sizeof p;
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, p, &size) != ERROR_SUCCESS ||
        size != sizeof p)
        return 0xFFFFFFFF;
    const BYTE* c = p + (g_dark ? 1 : 4) * 4;
    return (DWORD(c[0]) << 16) | (DWORD(c[1]) << 8) | c[2];
}

// -- the windows ------------------------------------------------------------

HWND top_of(const char* title)
{
    return FindWindowW(nullptr, std::wstring(title, title + std::strlen(title)).c_str());
}

RECT rect_in(HWND top, HWND child)
{
    RECT r{};
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, top, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

// A child of `top` of class `cls`, with `text` (any, if null).
HWND child(HWND top, const wchar_t* cls, const wchar_t* text = nullptr)
{
    return FindWindowExW(top, nullptr, cls, text);
}
// The window's combo boxes: a drop-down list's style, or an editable one's.
HWND combo_of(HWND top, bool list)
{
    for (HWND c = FindWindowExW(top, nullptr, L"ComboBox", nullptr); c;
         c = FindWindowExW(top, c, L"ComboBox", nullptr))
        if (((GetWindowLongPtrW(c, GWL_STYLE) & 3) == CBS_DROPDOWNLIST) == list) return c;
    return nullptr;
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
    DWORD raw(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0xDEADBEEF;
        return px[size_t(y) * w + x];
    }
};
// A control printed whole (WM_PRINT: its client area through
// WM_PRINTCLIENT, and so through the band's glass), as a pane's copy is.
printed print(HWND h)
{
    RECT r{};
    GetWindowRect(h, &r);
    printed out;
    out.w = r.right - r.left;
    out.h = r.bottom - r.top;
    if (out.w <= 0 || out.h <= 0) return out;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), out.w, -out.h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    std::fill_n(static_cast<DWORD*>(bits), size_t(out.w) * out.h, 0x00ABCDEFu);
    SendMessageW(h, WM_PRINT, reinterpret_cast<WPARAM>(mem),
                 PRF_NONCLIENT | PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
    GdiFlush();
    out.px.assign(static_cast<DWORD*>(bits), static_cast<DWORD*>(bits) + size_t(out.w) * out.h);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return out;
}

// Keyboard cues shown, then the focus sent (a never-shown window can't take
// it: the control's own state follows WM_SETFOCUS).
void focus(HWND h, bool on)
{
    SendMessageW(h, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
    SendMessageW(h, on ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}

// -- the checks -------------------------------------------------------------

struct density
{
    const char* name;
    float row, field, nav;
};
constexpr density standard{"standard", 46, 32, 40};
constexpr density compact{"compact", 36, 24, 32};

// Every control in the row the field's height, (row - field) / 2 under the
// strip; labels and check boxes centred in it.
void check_heights(HWND top, const std::vector<HWND>& controls, const std::vector<HWND>& centred,
                   const density& d, const std::string& at)
{
    const UINT dpi = GetDpiForWindow(top);
    HWND strip = FindWindowExW(top, nullptr, L"rapportCaptionTabs", nullptr);
    const int from = strip ? int(rect_in(top, strip).bottom) : 0;
    const int field = px(d.field, dpi), above = (px(d.row, dpi) - field) / 2;
    for (HWND c : controls)
    {
        wchar_t cls[32]{}, text[64]{};
        GetClassNameW(c, cls, 32);
        GetWindowTextW(c, text, 64);
        char name[128];
        std::snprintf(name, sizeof name, "%ls \"%ls\"", cls, text);
        const RECT r = rect_in(top, c);
        expect(r.bottom - r.top == field && r.top - from == above,
               std::string(name) + " " + str(field) + " px tall, " + str(above) +
                   " under the strip" + at + ": " + str(r.bottom - r.top) + " at " +
                   str(r.top - from));
    }
    for (HWND c : centred)
    {
        const RECT r = rect_in(top, c);
        const int mid = r.top + r.bottom, want = 2 * (from + above) + field;
        expect(std::abs(mid - want) <= 1, "a label or check box centred in the row" + at +
                                              ": its centre " + str(mid / 2) + ", the row's " +
                                              str(want / 2));
    }
}

// The row's own height: the band from the strip's bottom to its line.
void check_row(HWND top, const density& d, const std::string& at)
{
    const UINT dpi = GetDpiForWindow(top);
    HWND strip = FindWindowExW(top, nullptr, L"rapportCaptionTabs", nullptr);
    const int from = strip ? int(rect_in(top, strip).bottom) : 0;
    RECT c{};
    GetClientRect(top, &c);
    printed bg;
    bg.w = c.right;
    bg.h = c.bottom;
    {
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bg.w, -bg.h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        SendMessageW(top, WM_ERASEBKGND, reinterpret_cast<WPARAM>(mem), 0);
        GdiFlush();
        bg.px.assign(static_cast<DWORD*>(bits), static_cast<DWORD*>(bits) + size_t(bg.w) * bg.h);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }
    if (high_contrast()) return;  // (its band is the window's colour: no line to find by it)
    int y = from;
    while (y < bg.h && bg.raw(bg.w - 2, y) == band_layer_px()) ++y;
    expect(y - from == px(d.row, dpi), "the band " + str(px(d.row, dpi)) +
                                           " px from the strip to its line" + at + ": " +
                                           str(y - from));
}

// A face printed: its corners the band's layer (rounded to it), a pixel
// inside it `fill`, opaque.
void check_face(const printed& p, DWORD fill, const std::string& what)
{
    const DWORD layer = band_layer_px();
    expect(p.raw(0, 0) == layer && p.raw(p.w - 1, p.h - 1) == layer,
           "its corners the band's layer, " + argb_hex(layer) + what + ": " +
               argb_hex(p.raw(0, 0)) + ", " + argb_hex(p.raw(p.w - 1, p.h - 1)));
    const DWORD in = p.raw(3, p.h / 2);
    expect(in == (0xFF000000u | fill),
           "its fill " + hex(fill) + ", opaque" + what + ": " + argb_hex(in));
}

// A push button (or a menu button, a drop-down list): WinUI's Button in each
// state.
void check_button(HWND b, bool list, const std::string& name, const std::string& at)
{
    const face_colors c = colours();
    const DWORD ground = band_fill();
    const DWORD rest = over(c.fill, ground);
    const std::string what = " (" + name + at + ")";
    printed p = print(b);
    check_face(p, rest, what);
    // The elevation border: ControlStrokeColorDefault along the top (light;
    // ...Secondary dark), ...Secondary along the bottom (light).
    const DWORD top = over(g_dark ? c.stroke_secondary : c.stroke, rest);
    const DWORD bottom = over(g_dark ? c.stroke : c.stroke_secondary, rest);
    const int mid = p.w / 2;
    expect(p.at(mid, 0) == top && p.at(mid, p.h - 1) == bottom,
           "its border " + hex(top) + " along its top, " + hex(bottom) + " along its bottom" +
               what + ": " + hex(p.at(mid, 0)) + ", " + hex(p.at(mid, p.h - 1)));
    // Pressed: ControlFillColorTertiary, its border flat.
    if (!list)
    {
        SendMessageW(b, BM_SETSTATE, TRUE, 0);
        p = print(b);
        SendMessageW(b, BM_SETSTATE, FALSE, 0);
        check_face(p, over(c.pressed, ground), " pressed" + what);
        expect(p.at(mid, p.h - 1) == over(c.stroke, over(c.pressed, ground)),
               "pressed, its border ControlStrokeColorDefault" + what + ": " +
                   hex(p.at(mid, p.h - 1)));
    }
    // Disabled: ControlFillColorDisabled.
    EnableWindow(b, FALSE);
    p = print(b);
    EnableWindow(b, TRUE);
    check_face(p, over(c.disabled, ground), " disabled" + what);
    // Focused, cues shown: the focus visual's outer ring along its edge
    // (FocusStrokeColorOuter: dark in the light theme, light in the dark).
    focus(b, true);
    p = print(b);
    focus(b, false);
    const int ring = luma(p.at(mid, 0)), face = luma(rest);
    expect(g_dark ? ring > face + 100 : ring < face - 100,
           "focused, the focus visual along its edge" + what + ": " + hex(p.at(mid, 0)));
    // Cues hidden (a mouse click's focus): no ring.
    SendMessageW(b, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    SendMessageW(b, WM_SETFOCUS, 0, 0);
    p = print(b);
    SendMessageW(b, WM_KILLFOCUS, 0, 0);
    expect(p.at(mid, 0) == top,
           "focused with cues hidden, no ring" + what + ": " + hex(p.at(mid, 0)));
}

// A chevron's ink at a control's right end, none at its middle-right where
// there's no text.
void check_chevron(HWND h, const std::string& what)
{
    const printed p = print(h);
    const DWORD rest = p.at(3, p.h / 2);
    int found = 0;
    for (int y = 0; y < p.h; ++y)
        for (int x = p.w - px(30, GetDpiForWindow(h)); x < p.w - 4; ++x)
            found += std::abs(luma(p.at(x, y)) - luma(rest)) > 60;
    expect(found > 4, "its chevron drawn at its right" + what + ": " + str(found) + " px of ink");
}

// A tool button (owner drawn, alone): the band's layer at rest, hovered
// SubtleFillColorSecondary over it; a checked toggle the accent.
void check_tool_button(HWND b, bool checked, const std::string& at)
{
    printed p = print(b);
    if (checked)
    {
        const DWORD accent = accent_fill();
        expect(accent == 0xFFFFFFFF || p.at(3, p.h / 2) == accent,
               "a checked tool button the accent, " + hex(accent) + at + ": " +
                   hex(p.at(3, p.h / 2)));
        return;
    }
    expect(p.raw(3, p.h / 2) == band_layer_px(),
           "a tool button at rest the band's layer" + at + ": " + argb_hex(p.raw(3, p.h / 2)));
    SendMessageW(b, WM_MOUSEMOVE, 0, MAKELPARAM(2, 2));
    p = print(b);
    SendMessageW(b, WM_MOUSELEAVE, 0, 0);
    expect(p.raw(3, p.h / 2) != band_layer_px() &&
               (p.raw(3, p.h / 2) >> 24) > (band_layer_px() >> 24),
           "hovered, SubtleFillColorSecondary over it" + at + ": " + argb_hex(p.raw(3, p.h / 2)));
}

// An editable combo box's edit in its face's colour: its fill right of its
// text, where there's no ink, the face's beside it (left of the edit).
void check_combo_fill(HWND combo, const std::string& what)
{
    const printed p = print(combo);
    COMBOBOXINFO info{};
    info.cbSize = sizeof info;
    GetComboBoxInfo(combo, &info);
    const RECT e = rect_in(combo, info.hwndItem);
    const DWORD face = p.at(3, p.h / 2), in = p.at(e.right - 3, (e.top + e.bottom) / 2);
    expect(in == face, "its edit's ground the face's, " + hex(face) + what + ": " + hex(in));
}

// An editable combo box: a band field (at rest ControlStrokeColorDefault
// along its top, ControlStrongStrokeColorDefault along its bottom), focused
// the accent along its bottom.
void check_combo(HWND combo, const std::string& at)
{
    const face_colors c = colours();
    const DWORD rest = over(c.fill, band_fill());
    printed p = print(combo);
    check_face(p, rest, " (an editable combo box" + at + ")");
    // TextControlElevationBorderBrush, as a band field's: ControlStroke-
    // ColorDefault along its top and sides, ControlStrongStrokeColorDefault
    // along its bottom (#72000000 light, #8BFFFFFF dark).
    const DWORD edge = over(c.stroke, band_fill());
    const DWORD strong = over(from(g_dark ? 0x8BFFFFFF : 0x72000000), band_fill());
    expect(p.at(p.w / 3, 0) == edge && p.at(0, p.h / 2) == edge && p.at(p.w / 3, p.h - 1) == strong,
           "at rest its border " + hex(edge) + " along its top and sides, " + hex(strong) +
               " along its bottom" + at + ": " + hex(p.at(p.w / 3, 0)) + ", " +
               hex(p.at(0, p.h / 2)) + ", " + hex(p.at(p.w / 3, p.h - 1)));
    COMBOBOXINFO info{};
    info.cbSize = sizeof info;
    GetComboBoxInfo(combo, &info);
    const RECT e = rect_in(combo, info.hwndItem);
    const UINT dpi = GetDpiForWindow(combo);
    expect(e.left == px(11, dpi), "its text 11 px in (border and TextControlThemePadding)" + at +
                                      ": " + str(e.left) + " (" + str(e.top) + ", " + str(e.right) +
                                      ", " + str(e.bottom) + " in " + str(p.w) + " by " + str(p.h) +
                                      ")");
    check_combo_fill(combo, " (an editable combo box" + at + ")");
    // Disabled: ControlFillColorDisabled, its edit's (WM_CTLCOLORSTATIC) too.
    EnableWindow(combo, FALSE);
    p = print(combo);
    check_face(p, over(c.disabled, band_fill()), " (an editable combo box, disabled" + at + ")");
    // Disabled: ControlStrokeColorDefault all round, no strong bottom.
    expect(p.at(p.w / 3, p.h - 1) == over(c.stroke, band_fill()),
           "disabled, its bottom ControlStrokeColorDefault" + at + ": " +
               hex(p.at(p.w / 3, p.h - 1)));
    check_combo_fill(combo, " (an editable combo box, disabled" + at + ")");
    EnableWindow(combo, TRUE);
    const DWORD accent = accent_fill();
    if (accent == 0xFFFFFFFF) return;
    // The edit's focus is the face's.
    SendMessageW(combo, WM_COMMAND, MAKEWPARAM(0, EN_SETFOCUS),
                 reinterpret_cast<LPARAM>(info.hwndItem));
    // (it reads the focus itself: a never-shown window has none to give, so
    // only its colours' fill is checked through its edit's WM_CTLCOLOREDIT)
    note("the focused look needs the keyboard focus, which a never-shown window can't have: not "
         "printed");
}

// The separator: a line the field's height.
void check_separator(HWND top, HWND sep, const density& d, const std::string& at)
{
    const RECT r = rect_in(top, sep);
    const UINT dpi = GetDpiForWindow(top);
    expect(r.bottom - r.top == px(d.field, dpi) && r.right - r.left <= px(2, dpi),
           "a separator a line the field's height" + at + ": " + str(r.right - r.left) + " by " +
               str(r.bottom - r.top));
}

// A font's height in px (its LOGFONT's, unsigned).
long font_height(HWND h)
{
    LOGFONTW lf{};
    GetObjectW(reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)), sizeof lf, &lf);
    return std::labs(lf.lfHeight);
}

// The row at `dpi` (after a DPI change): each control the field's height;
// the editable combo's edit in the band field's font (a new DPI gives every
// child the UI font, the edit after its combo box) and where a band field's
// text is; and each combo box still the field's height once moved again (a
// combo box finds its own height from its font as it's moved).
void check_at_dpi(HWND top, const std::vector<HWND>& controls, const std::vector<HWND>& combos,
                  HWND editable, HWND field, const density& d, UINT dpi, const std::string& here)
{
    for (HWND c : controls)
    {
        const RECT r = rect_in(top, c);
        wchar_t cls[32]{}, text[64]{};
        GetClassNameW(c, cls, 32);
        GetWindowTextW(c, text, 64);
        char name[128];
        std::snprintf(name, sizeof name, "%ls \"%ls\"", cls, text);
        expect(r.bottom - r.top == px(d.field, dpi), std::string(name) + " " +
                                                         str(px(d.field, dpi)) + " px tall" + here +
                                                         ": " + str(r.bottom - r.top));
    }
    COMBOBOXINFO info{};
    info.cbSize = sizeof info;
    GetComboBoxInfo(editable, &info);
    expect(info.hwndItem && font_height(info.hwndItem) == font_height(field),
           "the editable combo's text in the band field's font" + here + ": " +
               str(info.hwndItem ? font_height(info.hwndItem) : 0) + " px, the field's " +
               str(font_height(field)));
    if (info.hwndItem)
    {
        const RECT e = rect_in(editable, info.hwndItem);
        expect(e.left == px(11, dpi),
               "its text " + str(px(11, dpi)) + " px in" + here + ": " + str(e.left));
    }
    for (HWND c : combos)
    {
        // Moved a pixel and back, as a later layout moves it.
        const RECT r = rect_in(top, c);
        SetWindowPos(c, nullptr, r.left + 1, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(c, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        pump(2);
        const RECT now = rect_in(top, c);
        expect(now.bottom - now.top == px(d.field, dpi),
               "a combo box still " + str(px(d.field, dpi)) + " px tall moved again" + here + ": " +
                   str(now.bottom - now.top));
    }
}

#ifdef RAPPORT_TEST_BUILTIN_WIN32
// 125% and 150%, and back, through the DPI test hook (every window taken to
// be at that DPI, then WM_DPICHANGED as Windows sends it, which gives every
// control the UI font at the new DPI). On CI the display's own scale is
// changed instead (--transitions).
void check_dpi(HWND top, const std::vector<HWND>& controls, const std::vector<HWND>& combos,
               HWND editable, HWND field, const density& d, const std::string& at)
{
    const UINT dpi = GetDpiForWindow(top);
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
        check_at_dpi(top, controls, combos, editable, field, d, to,
                     at + ", " + str(long(to)) + " dpi");
    }
}
#endif

// -- the display's scale, for real (CI only) ----------------------------------
//
// As win32-band-fields-transitions (and win32-caption-tabs-transitions)
// change it: the primary display's scale through DisplayConfig's
// undocumented DPI scale (as Settings sets it, in steps relative to the
// recommended one), its mode made larger first where a small screen allows
// only 100%, and both put back exactly as they were, read back to be sure,
// on every path. (The same code as win32_band_fields_test.cpp's.)

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
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS ||
        !paths)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr) !=
        ERROR_SUCCESS)
        return false;
    for (UINT32 i = 0; i < paths; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof name;
        name.header.adapterId = p[i].sourceInfo.adapterId;
        name.header.id = p[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS &&
            gdi_name == name.viewGdiDeviceName)
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
               now.dmBitsPerPel == mode.dmBitsPerPel &&
               now.dmDisplayFrequency == mode.dmDisplayFrequency;
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

// Every scale the display allows, for real, `check` at each new DPI of
// `top` (125% and 150% where the runner reaches them), then the scale put
// back and `check` again. Which ran is noted.
int scale_transitions(HWND top, const std::function<void(UINT, const std::string&)>& check)
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
    note("display scale steps " + str(g.min_rel) + ".." + str(g.max_rel) + ", now " +
         str(g.cur_rel));
    // A small screen (a runner's 1024x768) allows only 100%: its largest
    // mode first, for the scales it allows.
    if (g.max_rel <= g.cur_rel + 1)
    {
        const wchar_t* device = g_restore.device.c_str();
        DEVMODEW best{}, mode{};
        best.dmSize = mode.dmSize = sizeof(DEVMODEW);
        for (DWORD i = 0; EnumDisplaySettingsW(device, i, &mode); ++i)
            if (mode.dmPelsWidth * mode.dmPelsHeight > best.dmPelsWidth * best.dmPelsHeight)
                best = mode;
        if (best.dmPelsWidth && best.dmPelsWidth * best.dmPelsHeight >
                                    g_restore.mode.dmPelsWidth * g_restore.mode.dmPelsHeight)
        {
            g_restore.mode_changed = true;  // marked before it's tried: half made is put back too
            if (ChangeDisplaySettingsExW(device, &best, nullptr, CDS_FULLSCREEN, nullptr) ==
                DISP_CHANGE_SUCCESSFUL)
            {
                note("display mode " + str(long(best.dmPelsWidth)) + "x" +
                     str(long(best.dmPelsHeight)));
                pump(50);
                get_scale(g);
                note("display scale steps " + str(g.min_rel) + ".." + str(g.max_rel) + ", now " +
                     str(g.cur_rel));
            }
        }
    }
    const UINT before = GetDpiForWindow(top);
    std::string ran;
    bool at_125 = false;
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
        check(now, ", the display's scale, " + str(long(now)) + " dpi");
    }
    note("scales that ran: " + (ran.empty() ? std::string("none") : ran));
    expect(!ran.empty(), "the display allows another scale");
    expect(at_125, "125% among them");
    g_restore.undo();
    expect(pump_until([&] { return GetDpiForWindow(top) == before; }),
           "and back to " + str(long(before)));
    pump();
    check(before, ", the scale back, " + str(long(before)) + " dpi");
    return 0;
}

// --transitions: the row (an hbox) shown, its window taking each scale the
// display allows, at the density RAPPORT_DENSITY names (as a plugin build
// runs it); the checks at each DPI as check_at_dpi has them.
int transitions(rap::ui& ui, const density& d)
{
    using namespace rap::layout;
    const std::string title = std::string("Band controls transitions ") + d.name;
    auto win = ui.create(
        window{title,
               {vbox{hbox{button{"Fetch"}, entry{}.placeholder("Filter").stretch(),
                          drop_down{{"Author Email", "Subject"}, 0}, combo{{"one", "two"}, "one"}},
                     label{"content"}.stretch()}}}
            .size(900_px, 300_px)
            .margins(false)
            .base_surface()
            .tabs(document_tabs{{{"a", "Alpha", rap::icon::named("folder"), {}}}}));
    win->show();
    pump();
    HWND top = top_of(title.c_str());
    expect(top != nullptr, "the window, shown");
    if (!top) return 1;
    HWND fetch = child(top, L"Button", L"Fetch"), field = child(top, L"Edit");
    HWND list = combo_of(top, true), editable = combo_of(top, false);
    expect(fetch && field && list && editable, "its controls");
    if (!(fetch && field && list && editable)) return 1;
    const std::string at = std::string(", ") + d.name;
    check_at_dpi(top, {fetch, field, list, editable}, {list, editable}, editable, field, d,
                 GetDpiForWindow(top), at + ", before");
    return scale_transitions(top,
                             [&](UINT dpi, const std::string& here)
                             {
                                 check_at_dpi(top, {fetch, field, list, editable}, {list, editable},
                                              editable, field, d, dpi, at + here);
                             });
}

// One density: a row of controls (an hbox) and a toolbar, each its window's
// band.
void run(rap::ui& ui, const density& d, bool hc)
{
    rap::document_tabs* row_tabs = nullptr;
    using namespace rap::layout;
    const std::string at = std::string(", ") + d.name + ", " +
                           (hc       ? "high contrast"
                            : g_dark ? "dark"
                                     : "light");
    auto tabs = [] {
        return document_tabs{
            {{"a", "Alpha", rap::icon::named("folder"), {}}, {"b", "Beta", {}, {}}}};
    };
    auto menu = [] { return rap::menu{}; };
    const std::string row_title = std::string("Band controls row ") + d.name;
    auto row =
        ui.create(window{row_title,
                         {vbox{hbox{tool_button{rap::icon::named("go-down"), "Pull"},
                                    menu_button{"master"}.on_menu(menu),
                                    button{"Fetch"}.icon(rap::icon::named("go-down")), separator{},
                                    entry{}.placeholder("Filter").stretch(),
                                    drop_down{{"Author Email", "Subject"}, 0},
                                    combo{{"one", "two"}, "one"}, label{"Find"}, check{"Wrap"},
                                    tool_button{rap::icon::named("go-up"), "Toggle"}.toggle(true)},
                               label{"content"}.stretch()}}}
                      .size(1100_px, 300_px)
                      .margins(false)
                      .base_surface()
                      .tabs(tabs().bind(row_tabs)));
    const std::string bar_title = std::string("Band controls toolbar ") + d.name;
    auto bar = ui.create(
        window{bar_title,
               {vbox{toolbar{tool_button{rap::icon::named("go-previous"), "Back"}, separator{},
                             tool_button{rap::icon::named("go-next"), "Forward"}, button{"Fetch"},
                             drop_down{{"Author Email"}, 0}, entry{}.stretch()},
                     label{"content"}.stretch()}}}
            .size(1000_px, 300_px)
            .margins(false)
            .base_surface()
            .tabs(tabs()));
    pump();
    HWND top = top_of(row_title.c_str()), top2 = top_of(bar_title.c_str());
    expect(top && top2, "the windows" + at);
    if (!top || !top2) return;

    HWND pull = child(top, L"Button", L"Pull"), toggle = child(top, L"Button", L"Toggle");
    HWND menu_b = child(top, L"Button", L"master  ▾"), fetch = child(top, L"Button", L"Fetch");
    HWND wrap = child(top, L"Button", L"Wrap"), find = child(top, L"Static", L"Find");
    HWND list = combo_of(top, true), editable = combo_of(top, false), entry_h = child(top, L"Edit");
    expect(pull && toggle && menu_b && fetch && wrap && find && list && editable && entry_h,
           "the row's controls" + at);
    if (!(pull && toggle && menu_b && fetch && wrap && find && list && editable && entry_h)) return;
    HWND sep = nullptr;
    for (HWND s = FindWindowExW(top, nullptr, L"Static", nullptr); s;
         s = FindWindowExW(top, s, L"Static", nullptr))
        if (GetWindowTextLengthW(s) == 0 && rect_in(top, s).right - rect_in(top, s).left <= 4 &&
            rect_in(top, s).top < rect_in(top, fetch).bottom)
            sep = s;
    check_heights(top, {pull, menu_b, fetch, entry_h, list, editable, toggle}, {wrap, find}, d,
                  at + ", a row");
    check_row(top, d, at + ", a row");
    if (sep)
        check_separator(top, sep, d, at);
    else
        expect(false, "the row's separator" + at);

    HWND strip = child(top2, TOOLBARCLASSNAMEW);
    HWND fetch2 = child(top2, L"Button", L"Fetch"), list2 = combo_of(top2, true),
         entry2 = child(top2, L"Edit");
    expect(strip && fetch2 && list2 && entry2, "the toolbar's controls" + at);
    if (strip && fetch2 && list2 && entry2)
    {
        check_heights(top2, {fetch2, list2, entry2}, {}, d, at + ", a toolbar");
        check_row(top2, d, at + ", a toolbar");
        const UINT dpi = GetDpiForWindow(top2);
        RECT b0{};
        SendMessageW(strip, TB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&b0));
        expect(b0.right - b0.left == px(d.nav, dpi) && b0.bottom - b0.top == px(d.field, dpi),
               "its tool buttons " + str(px(d.nav, dpi)) + " by " + str(px(d.field, dpi)) + at +
                   ": " + str(b0.right - b0.left) + " by " + str(b0.bottom - b0.top));
    }

    if (!hc)
    {
        check_button(fetch, false, "a button", at);
        check_button(menu_b, false, "a menu button", at);
        check_chevron(menu_b, " (a menu button" + at + ")");
        check_button(list, true, "a drop-down list", at);
        check_chevron(list, " (a drop-down list" + at + ")");
        check_tool_button(pull, false, at);
        check_tool_button(toggle, true, at);
        check_combo(editable, at);
        // An entry in the band at rest: TextControlElevationBorderBrush, as
        // the combo box's (the window never shown, so never active).
        {
            const face_colors c = colours();
            const printed p = print(entry_h);
            const DWORD edge = over(c.stroke, band_fill());
            const DWORD strong = over(from(g_dark ? 0x8BFFFFFF : 0x72000000), band_fill());
            const int x = p.w * 3 / 4;
            expect(p.at(x, 0) == edge && p.at(0, p.h / 2) == edge && p.at(x, p.h - 1) == strong,
                   "an entry at rest its border " + hex(edge) + " along its top and sides, " +
                       hex(strong) + " along its bottom" + at + ": " + hex(p.at(x, 0)) + ", " +
                       hex(p.at(0, p.h / 2)) + ", " + hex(p.at(x, p.h - 1)));
        }
        // Out of the band and back (its tabs hidden: no band; shown again):
        // each face printed through the band's glass still, its corners the
        // band's layer, not opaque.
        if (row_tabs)
        {
            row_tabs->set_visible(false);
            pump();
            row_tabs->set_visible(true);
            pump();
            for (HWND c : {fetch, list, editable})
            {
                const printed p = print(c);
                expect(p.raw(0, 0) == band_layer_px(),
                       "out of the band and back, its corner the band's layer, " +
                           argb_hex(band_layer_px()) + at + ": " + argb_hex(p.raw(0, 0)));
            }
            check_button(fetch, false, "a button, out of the band and back", at);
            check_combo_fill(editable,
                             " (an editable combo box, out of the band and back" + at + ")");
        }
        else
            expect(false, "the row's tabs bound" + at);
#ifdef RAPPORT_TEST_BUILTIN_WIN32
        check_dpi(top, {pull, menu_b, fetch, entry_h, list, editable, toggle}, {list, editable},
                  editable, entry_h, d, at);
#endif
    }
}

}  // namespace

int main(int argc, char** argv)
{
    bool transitions_run = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--transitions") transitions_run = true;
    // --transitions changes the display's scale: on a CI runner only, and
    // checked before any window is made.
    if (transitions_run && (env("GITHUB_ACTIONS") != "true" || env("RAPPORT_TEST_SET_DPI") != "1"))
    {
        note("skipped the scale changes: they change the display's scale, so only on a CI runner "
             "(GITHUB_ACTIONS=true, RAPPORT_TEST_SET_DPI=1)");
        return 77;
    }
    const DWORD build = windows_build();
    if (build < 22000)
    {
        note("skipped: no band before Windows 11 (build 22000)");
        return 77;
    }
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    rap::win32::set_text_scale_for_tests(1.f);
#endif
    rap::load_options options{.backend = "win32"};
    options.look.color_scheme = rap::color_scheme::dark;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 2;
    const bool hc = high_contrast();
    g_dark = !hc && env("RAPPORT_THEME") != "light";
    std::printf("build %lu, %s\n", build, hc ? "high contrast" : g_dark ? "dark" : "light");
    if (transitions_run)
    {
        const int r = transitions(**ui, env("RAPPORT_DENSITY") == "compact" ? compact : standard);
        g_restore.undo();
        std::printf(fails ? "%d failed\n" : "all passed\n", fails);
        return fails ? 1 : r;
    }
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    // Both, through the density's test hook.
    rap::win32::set_band_density_for_tests(0);
    run(**ui, standard, hc);
    rap::win32::set_band_density_for_tests(1);
    run(**ui, compact, hc);
    rap::win32::set_band_density_for_tests(-1);
#else
    // The environment's (RAPPORT_DENSITY: win32-band-controls-compact).
    run(**ui, env("RAPPORT_DENSITY") == "compact" ? compact : standard, hc);
#endif
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
