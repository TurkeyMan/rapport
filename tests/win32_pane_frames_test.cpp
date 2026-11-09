// Controls' frames look the same on a scroll pane, whose content is printed
// off screen and copied (paint_pane), as when Windows paints them. A pane's
// copy validates its controls' frame paints, and printing a themed client
// edge gives the classic sunken one, so each is painted for real after the
// copy: on screen after layout passes must match a real frame paint, for
// every class with a client edge, light and dark (RAPPORT_THEME=light). The
// frames rapport paints itself (a frameless entry's edge, dark mode's rich
// text, list box and separator) must print the same too.
#include <rapport/rapport.h>
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Common controls 6: themed client edges, which are what can't be printed.
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
// The two frames alike, else which pixels differ (and how many).
void expect_same(const std::vector<DWORD>& got, const std::vector<DWORD>& want, const char* why)
{
    size_t differ = 0, first = 0;
    for (size_t i = 0; i < got.size() && i < want.size(); ++i)
        if (got[i] != want[i] && !differ++) first = i;
    if (got.size() == want.size() && !differ) return;
    std::fprintf(stderr, "failed: %s (%zu of %zu pixels differ; first at %zu: %06lx, not %06lx)\n", why, differ, want.size(),
                 first, first < got.size() ? got[first] : 0ul, first < want.size() ? want[first] : 0ul);
    ++failures;
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

// What `paint` draws into a w x h bitmap.
template <class F>
pixels capture(int w, int h, F&& paint)
{
    pixels out{w, h, std::vector<DWORD>(size_t(w) * h)};
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    paint(mem);
    GdiFlush();
    std::memcpy(out.p.data(), bits, out.p.size() * 4);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return out;
}

// The window as on screen: the screen's pixels where it lies (it's in
// front, topmost). Not PrintWindow, which has a GDI window print itself
// (WM_PRINT) even with PW_RENDERFULLCONTENT.
pixels on_screen(HWND root)
{
    RECT r;
    GetWindowRect(root, &r);
    DwmFlush();
    return capture(r.right - r.left, r.bottom - r.top, [&](HDC dc) {
        HDC screen = GetDC(nullptr);
        expect(BitBlt(dc, 0, 0, r.right - r.left, r.bottom - r.top, screen, r.left, r.top, SRCCOPY) != FALSE,
               "the screen could be read");
        ReleaseDC(nullptr, screen);
    });
}

// The outer two pixels of `h`'s window: its frame.
std::vector<DWORD> ring(const pixels& from, HWND root, HWND h)
{
    RECT rr, hr;
    GetWindowRect(root, &rr);
    GetWindowRect(h, &hr);
    OffsetRect(&hr, -rr.left, -rr.top);
    std::vector<DWORD> out;
    for (int y = hr.top; y < hr.bottom; ++y)
        for (int x = hr.left; x < hr.right; ++x)
            if (x - hr.left < 2 || hr.right - 1 - x < 2 || y - hr.top < 2 || hr.bottom - 1 - y < 2)
                out.push_back(x >= 0 && y >= 0 && x < from.w && y < from.h ? from.at(x, y) : 0);
    return out;
}

std::vector<DWORD> printed_ring(HWND h)
{
    RECT r;
    GetWindowRect(h, &r);
    const int w = r.right - r.left, ht = r.bottom - r.top;
    pixels p = capture(w, ht, [&](HDC dc) {
        SendMessageW(h, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_NONCLIENT | PRF_CLIENT | PRF_ERASEBKGND);
    });
    std::vector<DWORD> out;
    for (int y = 0; y < ht; ++y)
        for (int x = 0; x < w; ++x)
            if (x < 2 || w - 1 - x < 2 || y < 2 || ht - 1 - y < 2) out.push_back(p.at(x, y));
    return out;
}

// The shown window of class `cls` after skipping `skip` (an etched static,
// for "Static").
HWND find_class(HWND root, const wchar_t* cls, int skip)
{
    struct search { const wchar_t* cls; int skip; HWND found; } s{cls, skip, nullptr};
    EnumChildWindows(root, [](HWND h, LPARAM lp) -> BOOL {
        auto* s = reinterpret_cast<search*>(lp);
        wchar_t name[64]{};
        GetClassNameW(h, name, 64);
        if (_wcsicmp(name, s->cls) || !IsWindowVisible(h)) return TRUE;
        const LONG_PTR type = GetWindowLongPtrW(h, GWL_STYLE) & SS_TYPEMASK;
        if (!_wcsicmp(name, L"Static") && type != SS_ETCHEDHORZ && type != SS_ETCHEDVERT) return TRUE;
        if (s->skip-- > 0) return TRUE;
        s->found = h;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.found;
}

struct rows : rap::list_model
{
    size_t row_count() const override { return 3; }
    std::string text(size_t, size_t) const override { return "row"; }
};
}

int main(int argc, char** argv)
{
    rap::load_options options{.backend = "win32"};
    options.look.color_scheme = rap::color_scheme::dark;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 1;
    using namespace rap::layout;
    const char* theme = std::getenv("RAPPORT_THEME");
    const bool dark = !theme || std::strcmp(theme, "light");
    rows model;
    auto win = (*ui)->create(window{"Rapport pane frames", {scroll{vbox{
        entry{}.frame(false),
        entry{},
        hbox{
            list{{{"Name", {}, true}}, &model},
            tree{{rap::tree_item{"a", "a"}, rap::tree_item{"b", "b"}}},
        },
        text_view{"rich text"}.read_only(),
        separator{},
        tabs{{"one", label{"first"}}, {"two", label{"second"}}}.sidebar(),
    }}}});
    win->show();
    HWND root = FindWindowW(nullptr, L"Rapport pane frames");
    if (!root) return 2;
    // On the desktop: a CI machine's is small, and pixels off it can't be read.
    SetWindowPos(root, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // RAPPORT_TEST_AT=x,y: where to show it (away from someone's work).
    if (const char* at = std::getenv("RAPPORT_TEST_AT"))
    {
        int x = 0, y = 0;
        if (std::sscanf(at, "%d,%d", &x, &y) == 2)
            SetWindowPos(root, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // In front of everything: its pixels on screen are its own.
    SetWindowPos(root, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    pump();
    // Layout passes that move the pane's content (each control's width
    // follows the window's), each copied from the pane's print.
    for (int i = 0; i < 6; ++i)
    {
        SetWindowPos(root, nullptr, 0, 0, 480 + (i % 2 ? 40 : 0), 640,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump();
    }
    const struct { const wchar_t* cls; int skip; const char* what; bool own; } controls[] = {
        {L"Edit", 0, "frameless entry", true},
        {L"Edit", 1, "entry", false},
        {L"SysListView32", 0, "list view", false},
        {L"SysTreeView32", 0, "tree view", false},
        {L"RICHEDIT50W", 0, "rich text", true},
        {L"ListBox", 0, "list box", true},
        {L"Static", 0, "separator", true},
    };
    std::vector<HWND> hs;
    for (auto& c : controls)
    {
        HWND h = find_class(root, c.cls, c.skip);
        if (!h) { std::fprintf(stderr, "no %s\n", c.what); return 2; }
        hs.push_back(h);
    }
    // Each wholly in view: inside every window it's in (the pane's host clips
    // its content), and on the desktop.
    const RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                       GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                       GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    for (size_t i = 0; i < hs.size(); ++i)
    {
        RECT hr, meet;
        GetWindowRect(hs[i], &hr);
        bool whole = IntersectRect(&meet, &hr, &desktop) && EqualRect(&meet, &hr);
        for (HWND up = GetParent(hs[i]); up && whole; up = up == root ? nullptr : GetParent(up))
        {
            RECT client;
            GetClientRect(up, &client);
            MapWindowPoints(up, nullptr, reinterpret_cast<POINT*>(&client), 2);
            whole = IntersectRect(&meet, &hr, &client) && EqualRect(&meet, &hr);
        }
        if (!whole)
        {
            std::fprintf(stderr, "%s not wholly in view\n", controls[i].what);
            return 2;
        }
    }
    const pixels after_layout = on_screen(root);
    std::vector<std::vector<DWORD>> printed;
    for (HWND h : hs) printed.push_back(printed_ring(h));
    // What Windows paints when each frame really is painted.
    for (HWND h : hs) RedrawWindow(h, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    pump();
    const pixels painted = on_screen(root);
    for (size_t i = 0; i < hs.size(); ++i)
    {
        char why[128];
        const auto native = ring(painted, root, hs[i]);
        std::snprintf(why, sizeof why, "%s: on screen after layout, as painted", controls[i].what);
        expect_same(ring(after_layout, root, hs[i]), native, why);
        if (!controls[i].own || !dark) continue;  // themed: printing isn't how it gets there
        std::snprintf(why, sizeof why, "%s: printed, as painted", controls[i].what);
        expect_same(printed[i], native, why);
    }
    return failures ? 1 : 0;
}
