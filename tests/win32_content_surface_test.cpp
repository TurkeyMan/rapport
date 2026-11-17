// What a window's content sits on (look_options::content_surface, #96), and
// the ground of fluent fields and faces on a tab page (#94). Never shown:
// each part printed (WM_ERASEBKGND, WM_PRINT, WM_PRINTCLIENT) into a 32-bit
// bitmap.
//
// A base surface window on Windows 11 (window_spec::base_surface):
//  - automatic, under fluent controls (Windows 11's default), and layer:
//    its content on WinUI's layer, LayerFillColorDefault over
//    SolidBackgroundFillColorBase (#F9F9F9, dark #282828): the window's
//    erase, a tab page's body (the tab control printed), an enabled
//    entry's fill over(ControlFillColorDefault, layer) with its corners the
//    layer, a disabled entry's over(ControlFillColorDisabled, layer) and not
//    the enabled one's, a push button's corners the layer. The same entry
//    on the tab page, over the page's body.
//  - plain, native controls with automatic, before Windows 11, high
//    contrast: the base colour (COLOR_WINDOW, dark #191919), or the window
//    colour where there's no base surface, as before #96.
//  - layer with native controls: the layer, the app's choice honoured.
// A window without a base surface, fluent controls: an entry, a disabled
// entry and a push button on a tab page sit on the page's body as the tab
// control prints it (lighter than the window colour in light mode), not on
// the window colour (#94); in dark mode the page's body is the window
// colour.
//
//   rapport-win32-content-surface-test automatic|plain|layer|native|native-layer light|dark
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
extern "C" int rapport_win32_tab_body_cached(HWND);
extern "C" int rapport_win32_tab_body_known(HWND);
extern "C" int rapport_win32_tab_body_lookup(HWND);
extern "C" void rapport_win32_set_tab_dying_hook(void (*)(HWND, void*), void*);
namespace rap::win32
{
unsigned layout_requests_for_tests();  // win32_common.h
}  // namespace rap::win32
#endif

namespace
{
int failures = 0;
void expect(bool ok, const std::string& what)
{
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

std::string hex(DWORD v)
{
    char b[16];
    std::snprintf(b, sizeof b, "%06lx", static_cast<unsigned long>(v & 0xffffff));
    return b;
}

void pump(int rounds = 20)
{
    for (int i = 0; i < rounds; ++i)
    {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(5);
    }
}

DWORD windows_build()
{
    using fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto get = reinterpret_cast<fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    return get && get(&v) == 0 ? v.dwBuildNumber : 0;
}

bool high_contrast()
{
    HIGHCONTRASTW hc{sizeof hc};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

DWORD rgb(COLORREF c)
{
    return (DWORD(GetRValue(c)) << 16) | (DWORD(GetGValue(c)) << 8) | GetBValue(c);
}

int channel(DWORD v, int shift)
{
    return int((v >> shift) & 0xff);
}

// #AARRGGBB over opaque 0xRRGGBB.
DWORD over(DWORD top, DWORD under)
{
    const int a = int(top >> 24);
    DWORD out = 0;
    for (int shift : {16, 8, 0})
        out |= DWORD((channel(top, shift) * a + channel(under, shift) * (255 - a) + 127) / 255) << shift;
    return out;
}

bool g_dark = false;
// WinUI's ControlFillColorDefault and ControlFillColorDisabled
// (win32_band_field.cpp).
DWORD rest_fill()
{
    return g_dark ? 0x0FFFFFFF : 0xB3FFFFFF;
}
DWORD disabled_fill()
{
    return g_dark ? 0x0BFFFFFF : 0x4DF9F9F9;
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
printed print_whole(HWND h)
{
    RECT r{};
    GetWindowRect(h, &r);
    return into_bitmap(r.right - r.left, r.bottom - r.top,
                       [&](HDC dc)
                       {
                           SendMessageW(h, WM_PRINT, reinterpret_cast<WPARAM>(dc),
                                        PRF_NONCLIENT | PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
                       });
}
printed print_erase(HWND top)
{
    RECT c{};
    GetClientRect(top, &c);
    return into_bitmap(c.right, c.bottom,
                       [&](HDC dc) { SendMessageW(top, WM_ERASEBKGND, reinterpret_cast<WPARAM>(dc), 0); });
}
// The tab control's page body, as it prints itself (#94: the real ground,
// not the window's); with `beside`, its tab row right of the last tab.
DWORD tab_body(HWND tab, bool beside = false)
{
    RECT c{};
    GetClientRect(tab, &c);
    RECT body = c;
    TabCtrl_AdjustRect(tab, FALSE, &body);
    const printed p = into_bitmap(c.right, c.bottom,
                                  [&](HDC dc)
                                  {
                                      SendMessageW(tab, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc),
                                                   PRF_CLIENT | PRF_ERASEBKGND);
                                  });
    if (beside) return p.at(c.right - 3, std::max(1, int(body.top) / 2));
    return p.at((body.left + body.right) / 2, (body.top + body.bottom) / 2);
}

// This thread's top-level window titled `title` (never another process's:
// the variants run side by side under ctest -j, their windows titled alike).
HWND top_of(const wchar_t* title)
{
    struct find
    {
        const wchar_t* title;
        HWND found = nullptr;
    } f{title};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM p) -> BOOL
        {
            auto* f = reinterpret_cast<find*>(p);
            wchar_t text[128]{};
            GetWindowTextW(h, text, 128);
            if (std::wcscmp(text, f->title)) return TRUE;
            f->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}
std::vector<HWND> children_of(HWND top, const wchar_t* cls)
{
    std::vector<HWND> out;
    for (HWND c = GetWindow(top, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
    {
        wchar_t name[32]{};
        GetClassNameW(c, name, 32);
        if (!_wcsicmp(name, cls)) out.push_back(c);
    }
    auto top_y = [](HWND h)
    {
        RECT r{};
        GetWindowRect(h, &r);
        return r.top;
    };
    std::sort(out.begin(), out.end(), [&](HWND a, HWND b) { return top_y(a) < top_y(b); });
    return out;
}
HWND button_named(HWND top, const wchar_t* text)
{
    for (HWND b : children_of(top, L"Button"))
    {
        wchar_t t[64]{};
        GetWindowTextW(b, t, 64);
        if (!std::wcscmp(t, text)) return b;
    }
    return nullptr;
}

// An entry's fill (well right of its text, at mid height) and its top-left
// corner, printed whole.
struct field_look
{
    DWORD fill, corner;
};
field_look look_of(HWND edit)
{
    const printed p = print_whole(edit);
    return {p.at(p.w * 3 / 4, p.h / 2), p.at(0, 0)};
}

// A fluent entry and face on `ground`: the enabled entry's fill over its
// ground with its corner the ground, the disabled one's its own and not the
// enabled one's, the button's corner the ground.
void check_controls(HWND enabled, HWND disabled, HWND button, DWORD ground, const std::string& where)
{
    if (!enabled || !disabled || !button)
    {
        expect(false, where + ": the entries and the button found");
        return;
    }
    const field_look on = look_of(enabled), off = look_of(disabled);
    const DWORD rest = over(rest_fill(), ground), gone = over(disabled_fill(), ground);
    expect(on.fill == rest, where + ": an enabled entry's fill is over(rest, " + hex(ground) + ") " + hex(rest) +
                                ": " + hex(on.fill));
    expect(on.corner == ground, where + ": its corner is the ground " + hex(ground) + ": " + hex(on.corner));
    expect(off.fill == gone, where + ": a disabled entry's fill is over(disabled, " + hex(ground) + ") " +
                                 hex(gone) + ": " + hex(off.fill));
    expect(off.fill != on.fill, where + ": the disabled fill isn't the enabled one: " + hex(off.fill));
    const printed b = print_whole(button);
    expect(b.at(0, 0) == ground, where + ": a push button's corner is the ground " + hex(ground) + ": " + hex(b.at(0, 0)));
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "automatic";
    g_dark = argc > 2 && !std::strcmp(argv[2], "dark");
    rap::load_options options{.backend = "win32"};
    options.look.color_scheme = g_dark ? rap::color_scheme::dark : rap::color_scheme::light;
    if (mode == "plain") options.look.content_surface = rap::content_surface::plain;
    if (mode == "layer" || mode == "native-layer") options.look.content_surface = rap::content_surface::layer;
    if (mode == "native" || mode == "native-layer") options.look.controls = rap::control_style::native;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 1;

    const DWORD build = windows_build();
    const bool eleven = build >= 22000, hc = high_contrast();
    if (g_dark && (hc || build < 17763)) g_dark = false;  // no dark mode there
    const bool fluent = eleven && !hc && mode != "native" && mode != "native-layer";
    const bool layer = eleven && !hc && (mode == "layer" || mode == "native-layer" || (mode == "automatic" && fluent));
    const DWORD layer_colour = g_dark ? 0x282828 : 0xF9F9F9;
    const DWORD base_colour = hc ? rgb(GetSysColor(COLOR_WINDOW)) : g_dark ? 0x191919 : rgb(GetSysColor(COLOR_WINDOW));
    const DWORD window_colour = hc ? rgb(GetSysColor(COLOR_BTNFACE)) : g_dark ? 0x202020 : rgb(GetSysColor(COLOR_BTNFACE));
    const DWORD content = !eleven ? window_colour : layer ? layer_colour : base_colour;
    std::printf("build %lu, %s, %s%s: content %s\n", static_cast<unsigned long>(build), mode.c_str(),
                hc ? "high contrast" : g_dark ? "dark" : "light", fluent ? ", fluent" : "", hex(content).c_str());

    using namespace rap::layout;
    auto base = (*ui)->create(window{"Content surface", {vbox{
        entry{}.placeholder("Filter paths"),
        entry{"Disabled"}.enabled(false),
        button{"Push"},
        separator{},
        tabs{{"One", vbox{entry{}.placeholder("On the page"), entry{"Page disabled"}.enabled(false), button{"Page push"}}},
             {"Two", label{"Second"}}},
    }}}.base_surface().size(480_px, 760_px));
    auto plain = (*ui)->create(window{"Content surface, no base", {vbox{
        tabs{{"One", vbox{entry{}.placeholder("On a page"), entry{"Off"}.enabled(false), button{"A push"}}},
             {"Two", label{"Second"}}},
    }}}.size(420_px, 300_px));
    pump();

    HWND top = top_of(L"Content surface");
    HWND top2 = top_of(L"Content surface, no base");
    if (!top || !top2)
    {
        std::printf("FAIL the windows found\n");
        return 2;
    }

    // The window's erase, well below its controls and right of the list's
    // edge: the content's colour.
    {
        const printed e = print_erase(top);
        const DWORD got = e.at(e.w - 2, e.h - 2);
        expect(got == content, "the content's ground is " + hex(content) + ": " + hex(got));
        if (eleven && !hc && !layer)
            expect(got != layer_colour, "not the layer: " + hex(got));
    }

    const std::vector<HWND> edits = children_of(top, L"Edit");
    const std::vector<HWND> tabs_found = children_of(top, WC_TABCONTROLW);
    if (edits.size() < 4 || tabs_found.empty())
    {
        std::printf("FAIL the entries (%zu) and the tab control found\n", edits.size());
        return 2;
    }
    // The page's body: the content's colour on a base surface (rapport draws
    // its tabs), whichever it is.
    const DWORD page = tab_body(tabs_found[0]);
    if (eleven)
    {
        expect(page == content, "a tab page's body is the content's " + hex(content) + ": " + hex(page));
        const DWORD row = tab_body(tabs_found[0], true);
        expect(row == content, "the tab row beside the tabs is the content's " + hex(content) + ": " + hex(row));
    }

    // A separator: dark mode draws it (its etched edge is white there), its
    // area the content's colour round its one divider pixel.
    if (g_dark)
    {
        HWND line = nullptr;
        for (HWND s : children_of(top, L"Static"))
            if ((GetWindowLongPtrW(s, GWL_STYLE) & SS_TYPEMASK) == SS_ETCHEDHORZ) line = s;
        if (!line)
            expect(false, "a separator found");
        else
        {
            // Its etched edge takes its client area: the whole window, printed.
            RECT r{};
            GetWindowRect(line, &r);
            OffsetRect(&r, -r.left, -r.top);
            const printed p = into_bitmap(r.right, r.bottom,
                                          [&](HDC dc)
                                          {
                                              SendMessageW(line, WM_PRINT, reinterpret_cast<WPARAM>(dc),
                                                           PRF_CLIENT | PRF_NONCLIENT);
                                          });
            const int mid = (r.bottom - 1) / 2;
            expect(r.bottom >= 2 && r.right > 0, "a separator " + std::to_string(r.right) + " by " + std::to_string(r.bottom) + ", drawn round a pixel's line");
            for (int y = 0; y < r.bottom; ++y)
                if (y != mid)
                    expect(p.at(r.right / 2, y) == content, "a separator's row " + std::to_string(y) + " of " +
                                                                std::to_string(r.bottom) + " is the content's " +
                                                                hex(content) + ": " + hex(p.at(r.right / 2, y)));
        }
    }

    if (fluent)
    {
        check_controls(edits[0], edits[1], button_named(top, L"Push"), content, "below the band");
        check_controls(edits[2], edits[3], button_named(top, L"Page push"), page, "on a tab page");
    }

    // A window with no base surface: fields and faces on a tab page, on its
    // body as the tab control prints it (#94).
    if (fluent)
    {
        const std::vector<HWND> e2 = children_of(top2, L"Edit");
        const std::vector<HWND> t2 = children_of(top2, WC_TABCONTROLW);
        if (e2.size() < 2 || t2.empty())
        {
            std::printf("FAIL no base: the entries (%zu) and the tab control found\n", e2.size());
            return 2;
        }
        const DWORD body = tab_body(t2[0]);
        std::printf("     no base: the page's body %s, the window %s\n", hex(body).c_str(), hex(window_colour).c_str());
        if (g_dark) expect(body == window_colour, "no base, dark: the page's body is the window colour: " + hex(body));
        check_controls(e2[0], e2[1], button_named(top2, L"A push"), body, "no base, on a tab page");

        // Windows made, their tab page's entry printed (its ground looked up)
        // and destroyed, again and again: no GDI object kept for one gone,
        // and the lookup for the window still here right after each.
        const DWORD before_e2 = look_of(e2[0]).fill;
        auto gdi = [] { return long(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)); };
        long baseline = 0;
        bool lookups_ok = true;
        int printed_ok = 0;
        const int cycles = 150;
        for (int i = 0; i < cycles; ++i)
        {
            {
                auto w = (*ui)->create(window{"Content surface, cycled", {vbox{
                    tabs{{"One", vbox{entry{}.placeholder("Cycled"), button{"Cycled push"}}}, {"Two", label{"Second"}}},
                }}}.size(360_px, 240_px));
                HWND t = top_of(L"Content surface, cycled");
                const std::vector<HWND> e = t ? children_of(t, L"Edit") : std::vector<HWND>{};
                HWND b = t ? button_named(t, L"Cycled push") : nullptr;
                const std::vector<HWND> tc = t ? children_of(t, WC_TABCONTROLW) : std::vector<HWND>{};
                // Each found and printed on the page's body (light: not the
                // window colour), or the cycle doesn't count.
                // A cycle that fails says why, on a line of its own.
                if (!t || e.empty() || !b || tc.empty())
                    std::printf("     cycle %d: not found:%s%s%s%s\n", i, t ? "" : " window", tc.empty() ? " tab" : "",
                                e.empty() ? " entry" : "", b ? "" : " button");
                else
                {
                    const field_look f = look_of(e[0]);
                    const printed pb = print_whole(b);
                    const DWORD page_body = tab_body(tc.front());
                    if (f.corner != 0xDEADBEEF && f.corner == page_body && pb.w > 0 && pb.at(0, 0) == f.corner)
                        ++printed_ok;
                    else
                        std::printf("     cycle %d: entry corner %s, page body %s, button %dx%d corner %s\n", i,
                                    hex(f.corner).c_str(), hex(page_body).c_str(), pb.w, pb.h,
                                    hex(pb.at(0, 0)).c_str());
                }
            }
            pump(2);
            lookups_ok = lookups_ok && look_of(e2[0]).fill == before_e2;
            if (i == 9) baseline = gdi();
        }
        const long final_count = gdi();
        const long grew = final_count - baseline;
        expect(final_count > 0, "GDI objects counted at the end (" + std::to_string(final_count) + ")");
        expect(printed_ok == cycles, "every cycled window's entry and button found and printed on its page: " +
                                         std::to_string(printed_ok) + " of " + std::to_string(cycles));
        expect(baseline > 0, "GDI objects counted (" + std::to_string(baseline) + ")");
        expect(baseline > 0 && grew <= 4, "140 windows made, printed and destroyed: GDI objects grew by " +
                                              std::to_string(grew) + " (" + std::to_string(baseline) +
                                              " after the first 10)");
        expect(lookups_ok, "the page's entry printed after each, as before: " + hex(look_of(e2[0]).fill));

        // A control's cached answer, a miss in a window with no tabs, goes
        // with the control (so a control made at its handle asks again).
        // Light: dark mode's grounds never ask.
#ifdef RAPPORT_TEST_BUILTIN_WIN32
        if (!g_dark)
        {
            int asked = 0, cached = 0, gone = 0;
            for (int i = 0; i < 20; ++i)
            {
                HWND edit = nullptr;
                {
                    auto w = (*ui)->create(window{"Content surface, no tabs", {vbox{entry{}.placeholder("Alone")}}}
                                               .size(300_px, 120_px));
                    HWND t = top_of(L"Content surface, no tabs");
                    const std::vector<HWND> e = t ? children_of(t, L"Edit") : std::vector<HWND>{};
                    if (!e.empty())
                    {
                        edit = e[0];
                        ++asked;
                        look_of(edit);
                        cached += rapport_win32_tab_body_cached(edit);
                    }
                }
                // At once: WM_NCDESTROY has run, and no later lookup (a new
                // pass clearing the map) has had the chance to.
                if (edit && !rapport_win32_tab_body_cached(edit)) ++gone;
                pump(2);
            }
            expect(asked == 20 && cached == 20, "a lone entry's lookup cached, a miss (" + std::to_string(cached) +
                                                    " of " + std::to_string(asked) + ")");
            expect(gone == 20, "and gone with its window (" + std::to_string(gone) + " of 20)");
        }
#endif

        // A tab control destroyed under its page's controls, with no layout
        // since: the entry it held asks again, and is on the window now.
        if (!g_dark)
        {
            const DWORD on_page = look_of(e2[0]).corner;
            expect(body != window_colour && on_page == body, "before: the page's body " + hex(body) +
                                                                 " isn't the window's " + hex(window_colour) +
                                                                 ", and the entry is on it: " + hex(on_page));
#ifdef RAPPORT_TEST_BUILTIN_WIN32
            const unsigned layouts_before = rap::win32::layout_requests_for_tests();
#endif
            DestroyWindow(t2[0]);
            expect(!IsWindow(t2[0]) && IsWindow(e2[0]), "the tab control destroyed, the entry still there");
            const field_look after = look_of(e2[0]);
#ifdef RAPPORT_TEST_BUILTIN_WIN32
            expect(rap::win32::layout_requests_for_tests() == layouts_before,
                   "no relayout asked for between the teardown and the print");
#endif
            expect(on_page == body && after.corner == window_colour,
                   "its tab control destroyed, the entry is on the window " + hex(window_colour) + ", not the page " +
                       hex(on_page) + ": " + hex(after.corner));
        }

#ifdef RAPPORT_TEST_BUILTIN_WIN32
        // A tab body's scan prints the tab control, and what that runs may
        // destroy the control asking, destroy the tab, or forget the tab
        // bodies (a theme change): nothing is kept from such a scan. Each in
        // a window of its own, its tab not yet measured; the tab's
        // WM_PRINTCLIENT does it (once), through a subclass of the test's.
        if (!g_dark)
        {
            enum class does { destroy_control, destroy_tab, retheme };
            struct hook_data
            {
                does what;
                HWND control, tab, top;
                bool done = false;
            };
            auto hook = [](HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) -> LRESULT
            {
                auto* d = reinterpret_cast<hook_data*>(ref);
                if (msg == WM_PRINTCLIENT && !d->done)
                {
                    d->done = true;
                    if (d->what == does::destroy_control) DestroyWindow(d->control);
                    if (d->what == does::destroy_tab)
                    {
                        DestroyWindow(h);
                        return 0;
                    }
                    if (d->what == does::retheme) SendMessageW(d->top, WM_THEMECHANGED, 0, 0);
                }
                return DefSubclassProc(h, msg, wp, lp);
            };
            int n = 0;
            for (does what : {does::destroy_control, does::destroy_tab, does::retheme})
            {
                const std::wstring title = L"Content surface, reentrant " + std::to_wstring(++n);
                const std::string name = what == does::destroy_control ? "the control asking destroyed"
                                         : what == does::destroy_tab   ? "the tab destroyed"
                                                                       : "the tab bodies forgotten";
                auto w = (*ui)->create(window{std::string(title.begin(), title.end()), {vbox{
                    tabs{{"One", vbox{entry{}.placeholder("Asking")}}, {"Two", label{"Second"}}},
                }}}.size(360_px, 200_px));
                HWND t = top_of(title.c_str());
                const std::vector<HWND> e = t ? children_of(t, L"Edit") : std::vector<HWND>{};
                const std::vector<HWND> tc = t ? children_of(t, WC_TABCONTROLW) : std::vector<HWND>{};
                if (e.empty() || tc.empty())
                {
                    expect(false, name + ": the entry and the tab control found");
                    continue;
                }
                expect(!rapport_win32_tab_body_known(tc[0]), name + ": its tab not measured yet");
                hook_data d{what, e[0], tc[0], t};
                SetWindowSubclass(tc[0], hook, 0x5eed, reinterpret_cast<DWORD_PTR>(&d));
                // One lookup, as a ground asks (a print asks several times,
                // the later ones after the tab's print is over).
                const int found = rapport_win32_tab_body_lookup(e[0]);
                if (IsWindow(tc[0])) RemoveWindowSubclass(tc[0], hook, 0x5eed);
                expect(d.done, name + ": the tab was printed for the lookup");
                expect(!found, name + ": the lookup found no body (" + std::to_string(found) + ")");
                expect(!rapport_win32_tab_body_cached(e[0]), name + ": no answer kept for the control");
                // (The control's going changes nothing of the tab's: its body,
                // measured whole, may be kept.)
                if (what != does::destroy_control)
                    expect(!rapport_win32_tab_body_known(tc[0]), name + ": no body kept for the tab");
                if (what == does::destroy_control)
                    expect(!IsWindow(e[0]), name + ": it's gone");
                else
                {
                    // Asked again, with nothing in the way: what's there now.
                    const DWORD want = what == does::destroy_tab ? window_colour : tab_body(tc[0]);
                    const DWORD got = look_of(e[0]).corner;
                    expect(got == want, name + ": printed again, the entry is on " + hex(want) + ": " + hex(got));
                }
            }

            // A ground looked up from inside the tab control's own
            // WM_NCDESTROY, after rapport's forgetting and before the rest of
            // its teardown (a subclass below rapport's): the dying tab isn't
            // measured again, and nothing kept refers to it.
            {
                auto w = (*ui)->create(window{"Content surface, dying tab", {vbox{
                    tabs{{"One", vbox{entry{}.placeholder("Asking")}}, {"Two", label{"Second"}}},
                }}}.size(360_px, 200_px));
                HWND t = top_of(L"Content surface, dying tab");
                const std::vector<HWND> e = t ? children_of(t, L"Edit") : std::vector<HWND>{};
                const std::vector<HWND> tc = t ? children_of(t, WC_TABCONTROLW) : std::vector<HWND>{};
                if (e.empty() || tc.empty())
                    expect(false, "a dying tab: the entry and the tab control found");
                else
                {
                    const DWORD before = look_of(e[0]).corner;
                    struct dying
                    {
                        HWND entry;
                        DWORD seen = 0xDEADBEEF;
                    } d{e[0]};
                    rapport_win32_set_tab_dying_hook(
                        [](HWND, void* p)
                        {
                            auto* d = static_cast<dying*>(p);
                            d->seen = DWORD(rapport_win32_tab_body_lookup(d->entry));
                        },
                        &d);
                    DestroyWindow(tc[0]);
                    rapport_win32_set_tab_dying_hook(nullptr, nullptr);
                    expect(before == body && d.seen == 0,
                           "a dying tab: the entry was on the page " + hex(before) +
                               ", and looked up as it went found no body (" + std::to_string(d.seen) + ")");
                    expect(!rapport_win32_tab_body_known(tc[0]), "a dying tab: no body kept for it");
                    expect(!rapport_win32_tab_body_cached(e[0]), "a dying tab: no answer kept for the entry");
                    const DWORD after = look_of(e[0]).corner;
                    expect(after == window_colour, "a dying tab: the entry's now on the window's colour " +
                                                       hex(window_colour) + ": " + hex(after));
                }
            }
        }
#endif
    }

    // An empty document-tab strip on a base surface (under a menu bar, as
    // the strip is on Windows 11 with one): its row the content's colour,
    // as the row beside a strip's tabs is.
    if (eleven)
    {
        std::vector<rap::menu_bar_item> menus{{"_File", [] { return rap::menu{}; }}};
        auto empty = (*ui)->create(window{"Content surface, empty strip", {label{"Content"}}}
                                       .base_surface()
                                       .menu_bar(menus)
                                       .tabs(document_tabs{})
                                       .size(420_px, 200_px));
        pump();
        HWND t = top_of(L"Content surface, empty strip");
        HWND strip = nullptr;
        for (HWND c : t ? children_of(t, WC_TABCONTROLW) : std::vector<HWND>{})
            if (GetWindowLongPtrW(c, GWL_STYLE) & TCS_FIXEDWIDTH) strip = c;
        if (!strip)
            expect(false, "an empty strip found");
        else
        {
            RECT c{};
            GetClientRect(strip, &c);
            const printed p = into_bitmap(c.right, c.bottom,
                                          [&](HDC dc)
                                          {
                                              SendMessageW(strip, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc),
                                                           PRF_CLIENT | PRF_ERASEBKGND);
                                          });
            // Its top row (above the page frame the style draws round
            // nothing), and inside that frame.
            const DWORD row = p.at(c.right / 2, 0);
            expect(row == content, "an empty document-tab strip's row is the content's " + hex(content) + ": " + hex(row));
            const DWORD got = p.at(c.right / 2, std::max(1L, c.bottom / 2));
            expect(c.bottom > 0 && got == content, "an empty document-tab strip (" + std::to_string(c.right) + " by " +
                                                       std::to_string(c.bottom) + ") is the content's " + hex(content) +
                                                       ": " + hex(got));
        }
    }
    return failures ? 1 : 0;
}
