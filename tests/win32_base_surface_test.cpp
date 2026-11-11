// A base surface window (window_spec::base_surface) is painted in the base
// colour wherever its background shows, on Windows 11: the window's and its
// containers' own, labels', check boxes', sliders' and the toolbar's, inside
// a group box, on a tab page, and on a scroll pane after layout passes have
// resized it (its content drawn off screen, paint_pane). Before Windows 11,
// the window colour as for any other window. Light and dark
// (RAPPORT_THEME=light), and high contrast where the desktop has it. A tab
// control printed (as a pane's copy prints it) gives the same, a partial
// print whose clip leaves out its body's centre too.
#include <rapport/rapport.h>
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{
int failures;
void expect(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "failed: %s\n", why); ++failures; }
}

void pump()
{
    for (int round = 0; round < 20; ++round)
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
    auto get = ntdll ? reinterpret_cast<fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    return get && get(&v) == 0 ? v.dwBuildNumber : 0;
}

bool high_contrast()
{
    HIGHCONTRASTW hc{sizeof hc};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

// 0xRRGGBB, as the screen's pixels read.
DWORD rgb(COLORREF c)
{
    return (DWORD(GetRValue(c)) << 16) | (DWORD(GetGValue(c)) << 8) | GetBValue(c);
}

// The screen's pixels over `area` (screen coordinates): the window is in
// front, topmost. Not PrintWindow, which has a GDI window print itself.
struct pixels
{
    RECT area{};
    std::vector<DWORD> p;
    DWORD at(int x, int y) const
    {
        return p[size_t(y - area.top) * (area.right - area.left) + (x - area.left)] & 0xffffff;
    }
};
pixels on_screen(RECT area)
{
    const int w = area.right - area.left, h = area.bottom - area.top;
    pixels out{area, std::vector<DWORD>(size_t(w) * h)};
    DwmFlush();
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    expect(BitBlt(mem, 0, 0, w, h, screen, area.left, area.top, SRCCOPY) != FALSE, "the screen could be read");
    GdiFlush();
    std::memcpy(out.p.data(), bits, out.p.size() * 4);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return out;
}

// The commonest colour in `r` (screen coordinates): what's behind a
// control's text or glyph, or a container's content.
DWORD commonest(const pixels& from, RECT r)
{
    IntersectRect(&r, &r, &from.area);
    std::map<DWORD, size_t> n;
    for (int y = r.top; y < r.bottom; ++y)
        for (int x = r.left; x < r.right; ++x) ++n[from.at(x, y)];
    DWORD best = 0xff000000;
    size_t most = 0;
    for (auto& [c, k] : n)
        if (k > most) most = k, best = c;
    return best;
}

void expect_colour(const pixels& from, RECT r, DWORD want, const char* what)
{
    const DWORD got = commonest(from, r);
    if (got == want) return;
    std::fprintf(stderr, "failed: %s is %06lx, not %06lx\n", what, got, want);
    ++failures;
}

// The shown window of class `cls` with text `text` (any, if null), after
// skipping `skip` of them.
HWND find(HWND root, const wchar_t* cls, const wchar_t* text, int skip = 0)
{
    struct search { const wchar_t* cls; const wchar_t* text; int skip; HWND found; } s{cls, text, skip, nullptr};
    EnumChildWindows(root, [](HWND h, LPARAM lp) -> BOOL {
        auto* s = reinterpret_cast<search*>(lp);
        wchar_t name[64]{}, text[64]{};
        GetClassNameW(h, name, 64);
        GetWindowTextW(h, text, 64);
        if (_wcsicmp(name, s->cls) || !IsWindowVisible(h) || (s->text && wcscmp(text, s->text))) return TRUE;
        if (s->skip-- > 0) return TRUE;
        s->found = h;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.found;
}

RECT rect_of(HWND h)
{
    RECT r{};
    GetWindowRect(h, &r);
    return r;
}

// A tab control's page body, in its client coordinates.
RECT body_of(HWND tab)
{
    RECT body;
    GetClientRect(tab, &body);
    TabCtrl_AdjustRect(tab, FALSE, &body);
    return body;
}

// The pixel at `at` (client coordinates) of tab control `tab` printed
// (WM_PRINTCLIENT) into a bitmap, its clip `clip` if given.
DWORD printed(HWND tab, POINT at, const RECT* clip)
{
    RECT c;
    GetClientRect(tab, &c);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), c.right, -c.bottom, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (clip) IntersectClipRect(mem, clip->left, clip->top, clip->right, clip->bottom);
    SendMessageW(tab, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT | PRF_ERASEBKGND);
    GdiFlush();
    const DWORD px = static_cast<DWORD*>(bits)[size_t(at.y) * c.right + at.x] & 0xffffff;
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return px;
}
}

int main(int argc, char** argv)
{
    rap::load_options options{.backend = "win32"};
    options.look.color_scheme = rap::color_scheme::dark;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 1;
    using namespace rap::layout;
    const char* theme = std::getenv("RAPPORT_THEME");
    const DWORD build = windows_build();
    const bool hc = high_contrast();
    const bool dark = !hc && build >= 17763 && (!theme || std::strcmp(theme, "light"));
    const bool base = build >= 22000;
    // Explorer's dark surfaces (backends/common/theme_palette.h), else the
    // system colours.
    const DWORD surface = base ? (dark ? 0x191919 : rgb(GetSysColor(COLOR_WINDOW)))
                               : (dark ? 0x202020 : rgb(GetSysColor(COLOR_BTNFACE)));
    std::printf("build %lu, %s%s: surface %06lx\n", build, hc ? "high contrast" : dark ? "dark" : "light",
                base ? ", base surface" : "", surface);

    auto win = (*ui)->create(window{"Rapport base surface", {vbox{
        toolbar{tool_button{rap::icon{"go-up"}, "Up"}, tool_button{rap::icon{"view-refresh"}, "Refresh"}},
        label{"A label"},
        check{"A check box"},
        slider{0, 10, 5},
        group{"A group", vbox{label{"In the group"}, check{"Grouped check"}}},
        tabs{{"One", vbox{label{"On the tab"}, check{"Tab check"}, slider{0, 10, 3}}}, {"Two", label{"Second"}}},
        scroll{vbox{
            label{"On the pane"}, check{"Pane check"}, slider{0, 10, 7},
            tabs{{"Pane one", vbox{label{"On the pane's tab"}}}, {"Pane two", label{"Pane second"}}},
        }}.stretch(),
    }}}.base_surface().size(480_px, 720_px));
    win->show();
    HWND root = FindWindowW(nullptr, L"Rapport base surface");
    if (!root) return 2;
    // On the desktop (a CI machine's is small, and pixels off it can't be
    // read), or at RAPPORT_TEST_AT=x,y (away from someone's work); in front.
    int x = 0, y = 0;
    if (const char* at = std::getenv("RAPPORT_TEST_AT")) std::sscanf(at, "%d,%d", &x, &y);
    SetWindowPos(root, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    pump();
    // Layout passes that resize the pane's content, each drawn off screen.
    for (int i = 0; i < 6; ++i)
    {
        SetWindowPos(root, nullptr, 0, 0, 480 + (i % 2 ? 40 : 0), 720, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump();
    }

    RECT client;
    GetClientRect(root, &client);
    MapWindowPoints(root, nullptr, reinterpret_cast<POINT*>(&client), 2);
    const pixels shot = on_screen(client);

    // The window's own background, most of what's around its content.
    expect_colour(shot, client, surface, "the window");
    const struct { const wchar_t* cls; const wchar_t* text; int skip; const char* what; } controls[] = {
        {TOOLBARCLASSNAMEW, nullptr, 0, "the toolbar"},
        {L"Static", L"A label", 0, "a label"},
        {L"Button", L"A check box", 0, "a check box"},
        {L"Static", L"In the group", 0, "a label in a group box"},
        {L"Button", L"Grouped check", 0, "a check box in a group box"},
        {L"Static", L"On the pane", 0, "a label on a scroll pane"},
        {L"Button", L"Pane check", 0, "a check box on a scroll pane"},
    };
    for (auto& c : controls)
    {
        HWND h = find(root, c.cls, c.text, c.skip);
        if (!h)
        {
            std::fprintf(stderr, "no %s\n", c.what);
            return 2;
        }
        expect_colour(shot, rect_of(h), surface, c.what);
    }

    // Inside the group box's frame, between it and its content.
    if (HWND in = find(root, L"Static", L"In the group"))
    {
        RECT r = rect_of(in);
        expect_colour(shot, RECT{r.left - 5, r.top, r.left - 1, r.bottom}, surface, "inside a group box");
    }

    // A tab page's body and what's on it: on a base surface, the surface;
    // otherwise, in light mode, the style's body, which its controls match.
    // The tab controls: on the window, and on the scroll pane.
    HWND pane_panel = GetParent(find(root, L"Static", L"On the pane"));
    HWND tab = nullptr, pane_tab = nullptr;
    for (int i = 0; HWND h = find(root, WC_TABCONTROLW, nullptr, i); ++i)
        (GetParent(h) == pane_panel ? pane_tab : tab) = h;
    HWND on_tab = find(root, L"Static", L"On the tab");
    HWND tab_check = find(root, L"Button", L"Tab check");
    if (!tab || !on_tab || !tab_check)
    {
        std::fprintf(stderr, "no tab page\n");
        return 2;
    }
    RECT body;
    GetClientRect(tab, &body);
    TabCtrl_AdjustRect(tab, FALSE, &body);
    MapWindowPoints(tab, nullptr, reinterpret_cast<POINT*>(&body), 2);
    // The sliders: on the window, on the tab page, and on the pane.
    HWND slider = nullptr, tab_slider = nullptr, pane_slider = nullptr;
    for (int i = 0; HWND h = find(root, TRACKBAR_CLASSW, nullptr, i); ++i)
    {
        RECT r = rect_of(h), meet;
        if (GetParent(h) == GetParent(find(root, L"Static", L"On the pane")))
            pane_slider = h;
        else if (IntersectRect(&meet, &r, &body))
            tab_slider = h;
        else
            slider = h;
    }
    if (!slider || !tab_slider || !pane_slider)
    {
        std::fprintf(stderr, "no slider\n");
        return 2;
    }
    expect_colour(shot, rect_of(slider), surface, "a slider");
    expect_colour(shot, rect_of(pane_slider), surface, "a slider on a scroll pane");
    const DWORD page = base || dark ? surface : commonest(shot, body);
    expect_colour(shot, body, page, "a tab page");
    expect_colour(shot, rect_of(on_tab), page, "a label on a tab page");
    expect_colour(shot, rect_of(tab_check), page, "a check box on a tab page");
    expect_colour(shot, rect_of(tab_slider), page, "a slider on a tab page");

    // A tab page on the scroll pane, drawn with the pane's off-screen copy.
    HWND on_pane_tab = find(root, L"Static", L"On the pane's tab");
    if (!pane_tab || !on_pane_tab)
    {
        std::fprintf(stderr, "no tab page on the pane\n");
        return 2;
    }
    RECT pane_body = body_of(pane_tab);
    MapWindowPoints(pane_tab, nullptr, reinterpret_cast<POINT*>(&pane_body), 2);
    {
        RECT viewport, meet;
        GetClientRect(GetParent(pane_panel), &viewport);
        MapWindowPoints(GetParent(pane_panel), nullptr, reinterpret_cast<POINT*>(&viewport), 2);
        const RECT whole = rect_of(pane_tab);
        if (!IntersectRect(&meet, &whole, &viewport) || !EqualRect(&meet, &whole))
        {
            std::fprintf(stderr, "the pane's tabs aren't wholly in view\n");
            return 2;
        }
    }
    const DWORD pane_page = base || dark ? surface : commonest(shot, pane_body);
    expect_colour(shot, pane_body, pane_page, "a tab page on a scroll pane");
    expect_colour(shot, rect_of(on_pane_tab), pane_page, "a label on a tab page on a scroll pane");

    // Each printed: whole, and through a clip of a strip down the body's
    // left that leaves out its centre (where its colour is read).
    for (HWND t : {tab, pane_tab})
    {
        const char* where = t == tab ? "the window's" : "the pane's";
        const RECT b = body_of(t);
        const RECT strip{b.left, b.top, b.left + 12, b.bottom};
        const POINT at{b.left + 5, (b.top + b.bottom) / 2};
        const DWORD whole = printed(t, at, nullptr), part = printed(t, at, &strip);
        char why[160];
        if (base || dark)
        {
            if (whole != surface)
            {
                std::snprintf(why, sizeof why, "%s tab page printed is %06lx, not %06lx", where, whole, surface);
                expect(false, why);
            }
        }
        if (part != whole)
        {
            std::snprintf(why, sizeof why, "%s tab page printed through a clip is %06lx, not %06lx", where, part, whole);
            expect(false, why);
        }
    }

    // The scroll pane's viewport, after the passes that resized it.
    if (HWND pane = GetParent(find(root, L"Static", L"On the pane")))
    {
        RECT r;
        GetClientRect(GetParent(pane), &r);
        MapWindowPoints(GetParent(pane), nullptr, reinterpret_cast<POINT*>(&r), 2);
        expect_colour(shot, r, surface, "a scroll pane after resizing");
    }
    return failures ? 1 : 0;
}
