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
// box as a band field (ControlFillColorDefault, no border at rest; focused,
// ControlFillColorInputActive and the accent along its bottom); a tool
// button as an AppBarButton (the band's layer at rest, SubtleFillColor-
// Secondary over it hovered), a checked toggle in the accent; a separator a
// line the field's height in DividerStrokeColorDefault. Labels and check
// boxes stay native, centred in the row. Under high contrast
// (rapport-win32-high-contrast-run, CI only): none of it drawn, the native
// controls at the band's heights.
//
// Compact is through the density's test hook (static builds: a plugin build
// checks the standard density only).
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

// An editable combo box: a band field (no border at rest), focused the
// accent along its bottom.
void check_combo(HWND combo, const std::string& at)
{
    const face_colors c = colours();
    const DWORD rest = over(c.fill, band_fill());
    printed p = print(combo);
    check_face(p, rest, " (an editable combo box" + at + ")");
    expect(p.at(p.w / 3, 0) == rest,
           "at rest no border, as a band field's" + at + ": " + hex(p.at(p.w / 3, 0)));
    COMBOBOXINFO info{};
    info.cbSize = sizeof info;
    GetComboBoxInfo(combo, &info);
    const RECT e = rect_in(combo, info.hwndItem);
    const UINT dpi = GetDpiForWindow(combo);
    expect(e.left == px(11, dpi), "its text 11 px in (border and TextControlThemePadding)" + at +
                                      ": " + str(e.left) + " (" + str(e.top) + ", " + str(e.right) +
                                      ", " + str(e.bottom) + " in " + str(p.w) + " by " + str(p.h) +
                                      ")");
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

// One density: a row of controls (an hbox) and a toolbar, each its window's
// band.
void run(rap::ui& ui, const density& d, bool hc)
{
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
                      .tabs(tabs()));
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
    }
}

}  // namespace

int main(int argc, char** argv)
{
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
    run(**ui, standard, hc);
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    rap::win32::set_band_density_for_tests(1);
    run(**ui, compact, hc);
    rap::win32::set_band_density_for_tests(-1);
#else
    note("compact: through its test hook, static builds only");
#endif
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
