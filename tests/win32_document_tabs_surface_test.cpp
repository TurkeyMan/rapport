// The document tabs' strip (window_spec::tabs) beside its tabs: the row's
// empty part takes the window's surface, as its content does. On a base
// surface (window_spec::base_surface) on Windows 11 that's the base colour,
// where the style's own drawing left it the dialog colour; before Windows 11,
// and on a window without one, the window colour. On screen and printed (as
// a pane's copy prints it); light and dark (RAPPORT_THEME=light). On Windows
// 11 the windows have a menu bar, under which the tabs stay this strip (a
// window without one has them in its caption: win32-caption-tabs).
#include <rapport/rapport.h>
#include <windows.h>
// After windows.h, which it needs.
#include <commctrl.h>
#include <dwmapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

int failures = 0;

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

// 0xRRGGBB, as pixels read.
DWORD rgb(COLORREF c)
{
    return (DWORD(GetRValue(c)) << 16) | (DWORD(GetGValue(c)) << 8) | GetBValue(c);
}

// The commonest colour of 32-bit pixels `p` (`stride` wide) over `r`.
DWORD commonest(const DWORD* p, int stride, RECT r)
{
    std::map<DWORD, size_t> n;
    for (int y = r.top; y < r.bottom; ++y)
        for (int x = r.left; x < r.right; ++x) ++n[p[size_t(y) * stride + x] & 0xffffff];
    DWORD best = 0xff000000;
    size_t most = 0;
    for (auto& [c, k] : n)
        if (k > most) most = k, best = c;
    return best;
}

// `area` of the strip (its client coordinates): read from the screen, or
// printed (WM_PRINTCLIENT) into a bitmap of the strip's size.
DWORD shown(HWND strip, RECT area, bool print)
{
    RECT c;
    GetClientRect(strip, &c);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), c.right, -c.bottom, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (print)
        SendMessageW(strip, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem),
                     PRF_CLIENT | PRF_ERASEBKGND);
    else
    {
        DwmFlush();
        POINT at{0, 0};
        ClientToScreen(strip, &at);
        BitBlt(mem, 0, 0, c.right, c.bottom, screen, at.x, at.y, SRCCOPY);
    }
    GdiFlush();
    const DWORD got = commonest(static_cast<DWORD*>(bits), c.right, area);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return got;
}

// The window's document tabs' control: the tab control with fixed widths.
HWND strip_of(HWND root)
{
    HWND found = nullptr;
    EnumChildWindows(
        root,
        [](HWND h, LPARAM p) -> BOOL
        {
            wchar_t name[32]{};
            GetClassNameW(h, name, 32);
            if (_wcsicmp(name, WC_TABCONTROLW) ||
                !(GetWindowLongPtrW(h, GWL_STYLE) & TCS_FIXEDWIDTH))
                return TRUE;
            *reinterpret_cast<HWND*>(p) = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

void check(rap::ui& ui, bool base_surface, DWORD want, int x, int y)
{
    using namespace rap::layout;
    const char* title = base_surface ? "Document tabs, base surface" : "Document tabs";
    std::vector<rap::menu_bar_item> menus;
    if (windows_build() >= 22000) menus.push_back({"_File", [] { return rap::menu{}; }});
    auto win = ui.create(
        window{title, {label{"Content"}}}
            .size(480_px, 160_px)
            .base_surface(base_surface)
            .menu_bar(menus)
            .tabs(document_tabs{{{"a", "Alpha", {}, {}}, {"b", "Beta", {}, {}}}}.current("a")));
    win->show();
    HWND root = FindWindowW(nullptr, std::wstring(title, title + std::strlen(title)).c_str());
    HWND strip = root ? strip_of(root) : nullptr;
    if (!strip)
    {
        std::fprintf(stderr, "failed: no strip in \"%s\"\n", title);
        ++failures;
        return;
    }
    // On the desktop, in front (pixels off it or behind can't be read).
    SetWindowPos(root, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    pump();
    // The row beside the tabs: from past the last one (and the current
    // one's raised edge) to the strip's end, the row's height, less a
    // margin each way.
    RECT last{}, c;
    TabCtrl_GetItemRect(strip, 1, &last);
    GetClientRect(strip, &c);
    const RECT beside{last.right + 6, 2, c.right - 6, last.bottom - 2};
    if (beside.right - beside.left < 20 || beside.bottom <= beside.top)
    {
        std::fprintf(stderr, "failed: \"%s\" has no row beside its tabs (%ld..%ld)\n", title,
                     beside.left, beside.right);
        ++failures;
        return;
    }
    for (bool print : {false, true})
    {
        const DWORD got = shown(strip, beside, print);
        std::printf("%s, %s: beside the tabs %06lx, want %06lx\n", title,
                    print ? "printed" : "on screen", got, want);
        if (got != want) ++failures;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options options{.backend = "win32"};
    options.look.color_scheme = rap::color_scheme::dark;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 1;
    const char* theme = std::getenv("RAPPORT_THEME");
    const DWORD build = windows_build();
    const bool hc = high_contrast();
    const bool dark = !hc && build >= 17763 && (!theme || std::strcmp(theme, "light"));
    // The surfaces as win32-base-surface has them: Explorer's dark ones
    // (backends/common/theme_palette.h), else the system colours. A base
    // surface only from Windows 11 (build 22000).
    const DWORD window_colour = dark ? 0x202020 : rgb(GetSysColor(COLOR_BTNFACE));
    const DWORD base_colour =
        build >= 22000 ? (dark ? 0x191919 : rgb(GetSysColor(COLOR_WINDOW))) : window_colour;
    std::printf("build %lu, %s\n", build, hc ? "high contrast" : dark ? "dark" : "light");
    int x = 0, y = 0;
    if (const char* at = std::getenv("RAPPORT_TEST_AT")) std::sscanf(at, "%d,%d", &x, &y);
    check(**ui, false, window_colour, x, y);
    check(**ui, true, base_colour, x, y);
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
