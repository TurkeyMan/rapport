// A scroll pane's off-screen copy (paint_pane) has its controls as they show
// on screen (#119): printed bottom of the z-order first, so a tab control's
// pages (siblings above it) aren't covered by its body, nor a group box's
// contents by the box. Read through the backend's private seam
// rapport_win32_print_pane, which prints a panel as paint_pane does, on a
// never-shown window: each label's pixels in the pane's copy are the label's
// own (printed alone), its text and not only its background. And a
// control's own children printed too (a list's header).
#include <rapport/rapport.h>
#include <windows.h>
// After windows.h, which they need.
#include "win32_test_windows.h"
#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
extern "C" void rapport_win32_print_pane(HWND panel, HDC dc);
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

void pump()
{
    for (int round = 0; round < 10; ++round)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

HWND find_static(HWND top, const wchar_t* text)
{
    struct find
    {
        const wchar_t* text;
        HWND found = nullptr;
    } f{text};
    EnumChildWindows(
        top,
        [](HWND h, LPARAM p) -> BOOL
        {
            auto* f = reinterpret_cast<find*>(p);
            wchar_t cls[32]{}, t[128]{};
            GetClassNameW(h, cls, 32);
            GetWindowTextW(h, t, 128);
            if (_wcsicmp(cls, L"Static") || wcscmp(t, f->text)) return TRUE;
            f->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}

// A bitmap to print into, and its pixels read back.
struct canvas
{
    int w, h;
    HDC dc = nullptr;
    HBITMAP bm = nullptr;
    HGDIOBJ old = nullptr;
    canvas(int w_, int h_)
        : w(w_)
        , h(h_)
    {
        HDC screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void* bits = nullptr;
        bm = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        old = SelectObject(dc, bm);
        ReleaseDC(nullptr, screen);
        // A colour no theme paints, so what's left unprinted shows.
        RECT all{0, 0, w, h};
        HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &all, magenta);
        DeleteObject(magenta);
    }
    ~canvas()
    {
        SelectObject(dc, old);
        DeleteObject(bm);
        DeleteDC(dc);
    }
    std::vector<COLORREF> area(RECT r) const
    {
        GdiFlush();
        std::vector<COLORREF> out;
        for (int y = r.top; y < r.bottom; ++y)
            for (int x = r.left; x < r.right; ++x) out.push_back(GetPixel(dc, x, y));
        return out;
    }
};

// Kinds of colour in `px`: a label with its text has more than one.
size_t colours(const std::vector<COLORREF>& px)
{
    std::vector<COLORREF> seen;
    for (COLORREF c : px)
        if (std::find(seen.begin(), seen.end(), c) == seen.end()) seen.push_back(c);
    return seen.size();
}

// `label` (any control) in the pane's copy as it prints itself alone.
void check_control(HWND panel, HWND label, const std::string& what, void (*print_pane)(HWND, HDC))
{
    expect(label != nullptr, what + ": found");
    if (!label) return;
    RECT pc;
    GetClientRect(panel, &pc);
    RECT lr;
    GetWindowRect(label, &lr);
    MapWindowPoints(nullptr, panel, reinterpret_cast<POINT*>(&lr), 2);
    const int w = lr.right - lr.left, h = lr.bottom - lr.top;
    // The label alone, as it prints itself.
    canvas alone(w, h);
    SendMessageW(label, WM_PRINT, reinterpret_cast<WPARAM>(alone.dc),
                 PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
    const std::vector<COLORREF> own = alone.area({0, 0, w, h});
    expect(colours(own) > 1,
           what + ": printed alone, its text shows (" + std::to_string(colours(own)) + " colours)");
    // The pane's copy.
    canvas pane(pc.right, pc.bottom);
    print_pane(panel, pane.dc);
    const std::vector<COLORREF> copied = pane.area(lr);
    size_t differ = 0;
    for (size_t i = 0; i < own.size() && i < copied.size(); ++i) differ += own[i] != copied[i];
    expect(copied == own, what + ": in the pane's copy as printed alone, its text not covered (" +
                              std::to_string(differ) + " of " + std::to_string(own.size()) +
                              " pixels differ, " + std::to_string(colours(copied)) +
                              " colours there)");
}
}  // namespace

int main(int argc, char** argv)
{
    rap::load_options options{.backend = "win32"};
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui)
    {
        std::printf("no win32 backend: %s\n", ui.error().c_str());
        return 77;
    }
    void (*print_pane)(HWND, HDC) = nullptr;
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    print_pane = rapport_win32_print_pane;
#else
    if (HMODULE m = GetModuleHandleW(L"rapport_win32.dll"))
        print_pane =
            reinterpret_cast<void (*)(HWND, HDC)>(GetProcAddress(m, "rapport_win32_print_pane"));
#endif
    expect(print_pane != nullptr, "the backend's rapport_win32_print_pane seam");
    if (!print_pane) return 1;

    using namespace rap::layout;
    using namespace rap::literals;
    struct rows : rap::list_model
    {
        size_t row_count() const override { return 2; }
        std::string text(size_t row, size_t) const override { return "Row " + std::to_string(row); }
    } model;
    const wchar_t* title = L"Pane print";
    auto win = (*ui)->create(window{
        "Pane print",
        {scroll{vbox{
                    tabs{{"One", vbox{label{"On the pane's tab"}}}, {"Two", label{"Second"}}},
                    group{"A group", vbox{label{"In the pane's group"}}},
                    list{{{"Name", 120_px}, {"Size", 80_px}}, &model},
                }}
             .stretch()}}.size(360_px, 320_px));
    pump();
    HWND top = test_window(title);
    expect(top != nullptr, "the window, never shown");
    if (!top) return 1;
    HWND on_tab = find_static(top, L"On the pane's tab");
    HWND panel = on_tab ? GetParent(on_tab) : nullptr;
    expect(panel && GetPropW(panel, L"rapport-pane"),
           "the tab page's label is on a scroll pane's panel");
    if (!panel) return 1;
    check_control(panel, find_static(top, L"On the pane's tab"),
                  "a tab page's label on a scroll pane", print_pane);
    check_control(panel, find_static(top, L"In the pane's group"),
                  "a group box's label on a scroll pane", print_pane);
    // A control's own children (a list's header): printed too, over it.
    HWND list = FindWindowExW(panel, nullptr, WC_LISTVIEWW, nullptr);
    check_control(panel, list ? FindWindowExW(list, nullptr, WC_HEADERW, nullptr) : nullptr,
                  "a list's header (its child) on a scroll pane", print_pane);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
