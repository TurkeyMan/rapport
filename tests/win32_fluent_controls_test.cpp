// rapport's controls in WinUI's look in any window (look_options::controls):
// automatic is fluent on Windows 11 (build 22000 and later) and native
// before it; native is native everywhere; under high contrast the faces
// aren't drawn but keep the density's heights.
//
// A plain window (no base surface, no tabs in the caption: no toolbar band)
// holds an entry, a push button, a menu button, a drop-down list, an
// editable combo box, a tool button and a toolbar, never shown. Fluent:
// each the density's height (look_options::density, or RAPPORT_DENSITY:
// 32, compact 24), each face printed (WM_PRINT) with its corner the window's
// own ground and its fill ControlFillColorDefault over it, a tool button
// transparent at rest. Native (--native, or before Windows 11): the native
// controls at their own heights, no face.
//   rapport-win32-fluent-controls-test [--native] [--compact]
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
#include <string>
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

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
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

// `top` (#AARRGGBB) over opaque `under` (0xRRGGBB).
DWORD over(DWORD top, DWORD under)
{
    const int a = int(top >> 24);
    DWORD out = 0;
    for (int shift : {16, 8, 0})
        out |= DWORD((channel(top, shift) * a + channel(under, shift) * (255 - a) + 127) / 255)
               << shift;
    return out;
}

HWND top_of(const char* title)
{
    return FindWindowW(nullptr, std::wstring(title, title + std::strlen(title)).c_str());
}

HWND child(HWND top, const wchar_t* cls, const wchar_t* text = nullptr)
{
    return FindWindowExW(top, nullptr, cls, text);
}
HWND combo_of(HWND top, bool list)
{
    for (HWND c = FindWindowExW(top, nullptr, L"ComboBox", nullptr); c;
         c = FindWindowExW(top, c, L"ComboBox", nullptr))
        if (((GetWindowLongPtrW(c, GWL_STYLE) & 3) == CBS_DROPDOWNLIST) == list) return c;
    return nullptr;
}

int height_of(HWND h)
{
    RECT r{};
    GetWindowRect(h, &r);
    return r.bottom - r.top;
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
};
// A control printed whole (WM_PRINT), as a pane's copy is.
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

// AccentFillColorDefault: the accent palette's Dark1 (Light2 dark), if the
// desktop has the palette (else unknown here: 0xFFFFFFFF).
DWORD accent_fill(bool dark)
{
    BYTE p[32] = {};
    DWORD size = sizeof p;
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, p, &size) != ERROR_SUCCESS ||
        size != sizeof p)
        return 0xFFFFFFFF;
    const BYTE* c = p + (dark ? 1 : 4) * 4;
    return (DWORD(c[0]) << 16) | (DWORD(c[1]) << 8) | c[2];
}

// The window's own ground, as its erase paints it, at `child`'s left.
DWORD ground_of(HWND top, HWND c)
{
    RECT cr{};
    GetWindowRect(c, &cr);
    MapWindowPoints(nullptr, top, reinterpret_cast<POINT*>(&cr), 2);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, 4, 4);
    HGDIOBJ old = SelectObject(mem, bmp);
    SetViewportOrgEx(mem, -cr.left, -cr.top, nullptr);
    SendMessageW(top, WM_ERASEBKGND, reinterpret_cast<WPARAM>(mem), 0);
    const COLORREF c0 = GetPixel(mem, cr.left, cr.top);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return (DWORD(GetRValue(c0)) << 16) | (DWORD(GetGValue(c0)) << 8) | GetBValue(c0);
}

}  // namespace

int main(int argc, char** argv)
{
    bool native = false, compact = env("RAPPORT_DENSITY") == "compact";
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--native") native = true;
        if (std::string(argv[i]) == "--compact") compact = true;
    }
    rap::load_options options{.backend = "win32"};
    options.look.color_scheme = rap::color_scheme::dark;
    if (native) options.look.controls = rap::control_style::native;
    if (compact) options.look.density = rap::density::compact;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 2;
    const DWORD build = windows_build();
    const bool hc = high_contrast();
    const bool dark = !hc && env("RAPPORT_THEME") != "light";
    const bool fluent = !native && build >= 22000;
    std::printf("build %lu, %s, %s, %s\n", build,
                hc     ? "high contrast"
                : dark ? "dark"
                       : "light",
                fluent ? "fluent" : "native", compact ? "compact" : "standard");

    using namespace rap::layout;
    const char* title = "Fluent controls";
    auto win = (*ui)->create(window{
        title,
        {vbox{toolbar{tool_button{rap::icon::named("go-previous"), "Back"},
                      tool_button{rap::icon::named("go-next"), "Forward"}},
              hbox{entry{}.placeholder("Find").stretch(), button{"Fetch"}, button{"Other"},
                   menu_button{"master"}.on_menu([] { return rap::menu{}; }),
                   drop_down{{"Author Email", "Subject"}, 0}, combo{{"one", "two"}, "one"},
                   tool_button{rap::icon::named("go-up"), "Pull"}, label{"Label"}, check{"Check"}},
              label{"content"}.stretch()}}}
                                 .size(1000_px, 300_px));
    pump();
    HWND top = top_of(title);
    expect(top != nullptr, "the window");
    if (!top) return 1;
    HWND entry_h = child(top, L"Edit"), fetch = child(top, L"Button", L"Fetch");
    HWND menu_b = child(top, L"Button", L"master  ▾"), pull = child(top, L"Button", L"Pull");
    HWND list = combo_of(top, true), editable = combo_of(top, false);
    HWND strip = child(top, TOOLBARCLASSNAMEW);
    expect(entry_h && fetch && menu_b && pull && list && editable && strip, "its controls");
    if (!(entry_h && fetch && menu_b && pull && list && editable && strip)) return 1;
    const UINT dpi = GetDpiForWindow(top);
    const int field = px(compact ? 24.f : 32.f, dpi);

    struct control
    {
        const char* what;
        HWND h;
    };
    const control controls[] = {{"an entry", entry_h},
                                {"a push button", fetch},
                                {"a menu button", menu_b},
                                {"a drop-down list", list},
                                {"an editable combo box", editable},
                                {"a tool button", pull}};
    if (fluent)
    {
        // The density's height, drawn or (high contrast) not.
        for (const control& c : controls)
            expect(height_of(c.h) == field, std::string(c.what) + " the density's height, " +
                                                str(field) + ": " + str(height_of(c.h)));
        RECT b0{};
        SendMessageW(strip, TB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&b0));
        expect(b0.bottom - b0.top == field, "a toolbar's buttons the density's height, " +
                                                str(field) + ": " + str(b0.bottom - b0.top));
        if (!hc)
        {
            // Faces: round the rounded shape the window's own ground; inside
            // ControlFillColorDefault over it (a tool button transparent).
            const DWORD fill = dark ? 0x0FFFFFFFu : 0xB3FFFFFFu;
            for (const control& c : {controls[1], controls[2], controls[3], controls[4]})
            {
                const printed p = print(c.h);
                const DWORD ground = ground_of(top, c.h);
                const DWORD want = over(fill, ground);
                expect(p.at(0, 0) == ground && p.at(3, p.h / 2) == want,
                       std::string(c.what) + "'s face: its corner the window's ground, " +
                           hex(ground) + ", its fill " + hex(want) + ": " + hex(p.at(0, 0)) + ", " +
                           hex(p.at(3, p.h / 2)));
            }
            const printed t = print(pull);
            const DWORD ground = ground_of(top, pull);
            expect(t.at(3, t.h / 2) == ground, "a tool button at rest the window's ground, " +
                                                   hex(ground) + ": " + hex(t.at(3, t.h / 2)));
            // A dialog's default push button (BS_DEFPUSHBUTTON): WinUI's
            // AccentButtonStyle, its fill AccentFillColorDefault, beside an
            // ordinary one in ControlFillColorDefault; the default moved to
            // the other (as the dialog manager moves it to a focused push
            // button) and back, each face following it.
            HWND other = child(top, L"Button", L"Other");
            expect(other != nullptr, "a second push button");
            const DWORD accent = accent_fill(dark);
            if (other && accent != 0xFFFFFFFF)
            {
                auto fill_of = [](HWND b)
                {
                    const printed p = print(b);
                    return p.at(3, p.h / 2);
                };
                auto set_default = [](HWND b, bool on)
                { SendMessageW(b, BM_SETSTYLE, on ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE); };
                const DWORD plain = over(fill, ground_of(top, fetch));
                set_default(fetch, true);
                expect(fill_of(fetch) == accent && fill_of(other) == plain,
                       "the default button the accent, " + hex(accent) + ", the other " +
                           hex(plain) + ": " + hex(fill_of(fetch)) + ", " + hex(fill_of(other)));
                set_default(fetch, false);
                set_default(other, true);
                expect(fill_of(other) == accent && fill_of(fetch) == plain,
                       "the default moved to the other, its face with it: " + hex(fill_of(other)) +
                           ", " + hex(fill_of(fetch)));
                set_default(other, false);
                set_default(fetch, true);
                expect(fill_of(fetch) == accent && fill_of(other) == plain,
                       "and back: " + hex(fill_of(fetch)) + ", " + hex(fill_of(other)));
                // Its text TextOnAccentFillColorPrimary (white light, black dark):
                // the ink furthest from the accent.
                const printed p = print(fetch);
                int best = -1;
                DWORD ink = 0;
                for (int y = 0; y < p.h; ++y)
                    for (int x = p.w / 4; x < p.w * 3 / 4; ++x)
                    {
                        const DWORD v = p.at(x, y);
                        const int d = std::abs(channel(v, 16) - channel(accent, 16)) +
                                      std::abs(channel(v, 8) - channel(accent, 8)) +
                                      std::abs(channel(v, 0) - channel(accent, 0));
                        if (d > best) best = d, ink = v;
                    }
                const bool on_accent = dark ? channel(ink, 8) < 0x40 : channel(ink, 8) > 0xC0;
                expect(on_accent, std::string("its text TextOnAccentFillColorPrimary, ") +
                                      (dark ? "black" : "white") + ": " + hex(ink));
                set_default(fetch, false);
            }
            else
                std::printf("     the accent palette unknown here: the default button's fill not "
                            "checked\n");
            // An entry's frame: the band field's, drawn (the test tools' message).
            static const UINT query = RegisterWindowMessageW(L"rapport-band-field");
            expect((SendMessageW(entry_h, query, 0, 0) & 3) == 3,
                   "an entry a WinUI text box, drawn");
        }
    }
    else
    {
        // Native: their own heights, the entry's native edge, no face.
        static const UINT query = RegisterWindowMessageW(L"rapport-band-field");
        expect(SendMessageW(entry_h, query, 0, 0) == 0 &&
                   (GetWindowLongPtrW(entry_h, GWL_EXSTYLE) & WS_EX_CLIENTEDGE),
               "an entry a plain EDIT with its native edge");
        expect(height_of(fetch) < px(32, dpi),
               "a push button its own height: " + str(height_of(fetch)));
        expect(height_of(pull) == px(28, dpi),
               "a tool button its own size: " + str(height_of(pull)));
        static const UINT face = RegisterWindowMessageW(L"rapport-band-face");
        expect(SendMessageW(list, face, 0, 0) == 0, "a drop-down list not drawn here");
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
