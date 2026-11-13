// The toolbar band's glass (backends/win32/win32_band_glass.cpp) when a
// control's own message destroys it: a slider in the band, its thumb
// pressed, then a move (messages, no input) changes its value, whose
// on_changed destroys the window, the slider with it, inside the glass's
// subclass passing the move on. Nothing of the glass's state may be touched
// after that: built with AddressSanitizer (MSVC's /fsanitize=address in
// CMAKE_CXX_FLAGS) a read of it freed is reported and the process fails;
// without it, it at least mustn't crash. Never shown.
//
// Before Windows 11 there's no band, so no glass: skipped (77).
//   rapport-win32-caption-glass-reentrant-test
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <memory>

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
void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++fails;
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

}  // namespace

int main(int argc, char** argv)
{
    if (windows_build() < 22000)
    {
        std::printf("SKIP before Windows 11: no tabs in the caption, no band, no glass\n");
        return 77;
    }
    rap::load_options options{.backend = "win32"};
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 2;
    using namespace rap::layout;
    std::unique_ptr<rap::window> w;
    bool armed = false;
    int calls = 0;
    w = (*ui)->create(window{"Caption glass, reentrant",
                             {toolbar{slider{0, 100, 50}.stretch().on_changed(
                                  [&](double)
                                  {
                                      ++calls;
                                      if (armed) w.reset();  // the window, and the slider, gone in its own move
                                  })},
                              label{"Content"}}}
                          .margins(false)
                          .base_surface()
                          .tabs(document_tabs{{{"a", "Alpha", {}, {}}}}));
    w->resize({900, 300});
    HWND top = FindWindowW(nullptr, L"Caption glass, reentrant");
    HWND slider = top ? FindWindowExW(top, nullptr, TRACKBAR_CLASSW, nullptr) : nullptr;
    expect(top && slider, "the window, never shown, and its slider in the band");
    if (!slider) return 1;
    // The slider's on glass: printed, its pixels have the band's layer under
    // them, none at alpha 0 (GDI's own would all be).
    {
        RECT c{};
        GetClientRect(slider, &c);
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), c.right, -c.bottom, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        SendMessageW(slider, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT | PRF_ERASEBKGND);
        GdiFlush();
        long clear = 0;
        for (long i = 0; i < long(c.right) * c.bottom; ++i) clear += (static_cast<const DWORD*>(bits)[i] >> 24) == 0;
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        expect(c.right > 0 && clear == 0, "the slider's on glass: printed, no pixel at alpha 0");
    }
    RECT thumb{};
    SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    const int x = (thumb.left + thumb.right) / 2, y = (thumb.top + thumb.bottom) / 2;
    SendMessageW(slider, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    armed = true;
    SendMessageW(slider, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + 50, y));
    expect(calls > 0 && !w, "the move changed the value, and on_changed destroyed the window inside it");
    expect(!IsWindow(slider) && !IsWindow(top), "the slider and the window gone");
    // Messages still queued for them go nowhere.
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
