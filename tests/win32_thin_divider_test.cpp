// A thin splitter (splitter::thin) draws its bar as Explorer's divider: the
// base colour with a line of the divider colour px(2) in from the pane
// before it, px(1) wide, while the bar stays px(5) to drag. A splitter not
// asked for it keeps the window-coloured bar. Horizontal and vertical, as
// the bar prints (WM_PRINT: what it erases on screen, and in a pane's copy).
// Light (RAPPORT_THEME=light) checks the colours, the system's window and
// 3D light; any theme checks the shape.
#include <rapport/rapport.h>
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
extern "C" void* rapport_win32_trace_splitter_bar(rap::splitter*, unsigned);
#endif

namespace
{
int failures;
void expect(bool ok, const char* why)
{
    if (!ok)
    {
        std::fprintf(stderr, "failed: %s\n", why);
        ++failures;
    }
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

struct pixels
{
    int w = 0, h = 0;
    std::vector<DWORD> p;
    DWORD at(int x, int y) const { return p[size_t(y) * w + x] & 0xffffff; }
};

// The bar as it prints itself, its erase included.
pixels printed(HWND h)
{
    RECT c;
    GetClientRect(h, &c);
    pixels out{c.right, c.bottom, std::vector<DWORD>(size_t(c.right) * c.bottom)};
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), c.right, -c.bottom, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    SendMessageW(h, WM_PRINT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT | PRF_ERASEBKGND);
    GdiFlush();
    std::memcpy(out.p.data(), bits, out.p.size() * 4);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return out;
}

// A system colour as a DIB pixel (0xRRGGBB).
DWORD dib(int index)
{
    const COLORREF c = GetSysColor(index);
    return DWORD(GetRValue(c)) << 16 | DWORD(GetGValue(c)) << 8 | GetBValue(c);
}

int px(float v, UINT dpi) { return int(std::lround(v * float(dpi) / 96.f)); }

// The pixel `along` the bar, `across` from its pane-before edge.
DWORD across_bar(const pixels& p, bool horizontal, int across, int along)
{
    return horizontal ? p.at(across, along) : p.at(along, across);
}

void check_thin(HWND bar, bool horizontal, bool light, const char* which)
{
    const UINT dpi = GetDpiForWindow(bar);
    const pixels p = printed(bar);
    const int wide = horizontal ? p.w : p.h, length = horizontal ? p.h : p.w;
    char why[200];
    std::snprintf(why, sizeof why, "%s: the bar is px(5) to drag (%d px at %u DPI, not %d)", which,
                  px(5, dpi), dpi, wide);
    expect(wide >= px(5, dpi) && wide >= 5, why);
    if (length <= 0 || wide < 5) return;
    const int at = px(2, dpi), line = std::max(1, px(1, dpi));
    if (dpi == 96)
    {
        std::snprintf(why, sizeof why, "%s: at 96 DPI the line is 1 px, 2 px in from the pane before", which);
        expect(at == 2 && line == 1, why);
    }
    const DWORD base = across_bar(p, horizontal, 0, 0), mark = across_bar(p, horizontal, at, 0);
    bool shape = base != mark;
    for (int along = 0; along < length; ++along)
        for (int x = 0; x < wide; ++x)
            shape = shape && across_bar(p, horizontal, x, along) == (x >= at && x < at + line ? mark : base);
    std::snprintf(why, sizeof why,
                  "%s: the base colour with a line across it at px(2), px(1) wide, along its length "
                  "(%06lx with %06lx)",
                  which, base, mark);
    expect(shape, why);
    if (light)
    {
        std::snprintf(why, sizeof why, "%s: light, the bar is the base colour (%06lx, not %06lx)", which,
                      dib(COLOR_WINDOW), base);
        expect(base == dib(COLOR_WINDOW), why);
        std::snprintf(why, sizeof why, "%s: light, its line is the divider colour (%06lx, not %06lx)", which,
                      dib(COLOR_3DLIGHT), mark);
        expect(mark == dib(COLOR_3DLIGHT), why);
    }
}
}  // namespace

int main(int argc, char** argv)
{
    HIGHCONTRASTW hc{sizeof hc};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON))
        return 77;  // high contrast's colours are its own
    const char* theme = std::getenv("RAPPORT_THEME");
    const bool light = theme && std::string_view(theme) == "light";

    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui) return 1;
    using lookup = void* (*)(rap::splitter*, unsigned);
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    lookup bar = rapport_win32_trace_splitter_bar;
#else
    HMODULE module = GetModuleHandleW(L"rapport_win32.dll");
    lookup bar = module ? reinterpret_cast<lookup>(GetProcAddress(module, "rapport_win32_trace_splitter_bar"))
                        : nullptr;
#endif
    if (!bar) return 1;

    using namespace rap::layout;
    rap::splitter *columns = nullptr, *rows = nullptr, *plain = nullptr;
    auto win = (*ui)->create(window{
        "Thin divider",
        {vbox{
            splitter{rap::orientation::horizontal, text_view{}.stretch(),
                     splitter{rap::orientation::vertical, text_view{}.stretch(), text_view{}.stretch()}
                         .thin()
                         .bind(rows)
                         .stretch()}
                .thin()
                .bind(columns)
                .stretch(),
            splitter{rap::orientation::horizontal, text_view{}.stretch(), text_view{}.stretch()}
                .bind(plain)
                .stretch(),
        }}}.size(60_ch, 30_lines));
    win->show();
    pump();

    HWND col = static_cast<HWND>(bar(columns, 0));
    HWND row = static_cast<HWND>(bar(rows, 0));
    HWND def = static_cast<HWND>(bar(plain, 0));
    expect(col && row && def, "each splitter has its bar");
    if (!col || !row || !def) return 1;

    check_thin(col, true, light, "horizontal");
    check_thin(row, false, light, "vertical");

    // Not asked for: as before, the window's colour throughout, 5 px.
    const pixels p = printed(def);
    const UINT dpi = GetDpiForWindow(def);
    expect(p.w == px(5, dpi), "an ordinary splitter's bar is px(5)");
    bool even = p.w > 0 && p.h > 0;
    for (int y = 0; even && y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) even = even && p.at(x, y) == p.at(0, 0);
    expect(even, "an ordinary splitter's bar is one colour");
    if (light) expect(p.w > 0 && p.at(0, 0) == dib(COLOR_BTNFACE), "light, an ordinary bar is the window colour");

    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}
