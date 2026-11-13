// The caption tabs' toolbar band on screen, as the compositor shows it: no
// holes. On Windows 11 the band (Explorer's address row) is inside the
// frame extended into the client area, where the compositor reads each
// pixel's alpha, and every native control in it has to have its alpha right
// (backends/win32/win32_band_glass.cpp): a pixel GDI left at alpha 0 there
// would be glass.
//
// A Windows.Graphics.Capture frame has the backdrop composed in, so its
// alpha is 255 everywhere, holes too (it's checked, as asked, but can't see
// one). So the window's captured twice, its frame light and then dark
// (DWMWA_USE_IMMERSIVE_DARK_MODE, put back after), which changes the solid
// colour the compositor puts behind an inactive window's extended frame
// (the window's shown without activating). The strip's empty part, past
// "+", is alpha 0 (the backdrop alone): each pixel's alpha is then
// 1 - (its change / the backdrop's change). Every pixel of the band's rows,
// 2 px in from the window's outline, must be at least the band's layer's
// alpha (WinUI's LayerOnMicaBaseAltFillColorDefault: 0xB3 light, 0x73
// dark); the line under the band and the text field's inside opaque.
//
// Shown, so CI only in practice. A real skip (77), never a pass, when
// there's nothing to see: before Windows 11 (no band), in high contrast (the
// band's off the extended frame), Windows.Graphics.Capture not supported,
// no frame arriving, or the two backdrops not differing enough to tell.
//   rapport-win32-caption-glass-test (RAPPORT_THEME=light for light)
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(                                                                                   \
    linker,                                                                                        \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) dxgi_access : ::IUnknown
{
    virtual HRESULT __stdcall GetInterface(GUID const& id, void** object) = 0;
};

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
int skip(const std::string& why)
{
    std::printf("SKIP %s\n", why.c_str());
    std::fflush(stdout);
    return 77;
}
std::string str(long v)
{
    return std::to_string(v);
}
std::string fixed(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
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

// A captured frame, BGRA, in the window's extended frame bounds.
struct frame
{
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    uint32_t at(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h ? px[size_t(y) * w + x] : 0; }
};

struct capturer
{
    wd3d::IDirect3DDevice device{nullptr};
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID3D11DeviceContext> context;

    bool make()
    {
        for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
            if (SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                            D3D11_SDK_VERSION, d3d.put(), nullptr, context.put())))
                break;
        if (!d3d) return false;
        winrt::com_ptr<::IInspectable> inspectable;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(d3d.as<IDXGIDevice>().get(), inspectable.put()))) return false;
        device = inspectable.as<wd3d::IDirect3DDevice>();
        return true;
    }

    // The window's next frame (its second: the first may predate the
    // session), this thread pumping its messages meanwhile; empty if none
    // came within a few seconds.
    frame grab(HWND top)
    {
        frame out;
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        if (FAILED(interop->CreateForWindow(top, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item))))
            return out;
        auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
        auto session = pool.CreateCaptureSession(item);
        try
        {
            session.IsCursorCaptureEnabled(false);
        }
        catch (...)
        {
        }
        HANDLE arrived = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wgc::Direct3D11CaptureFrame got{nullptr};
        int frames = 0;
        pool.FrameArrived(
            [&](const wgc::Direct3D11CaptureFramePool& p, const winrt::Windows::Foundation::IInspectable&)
            {
                auto f = p.TryGetNextFrame();
                if (++frames >= 2 && !got)
                {
                    got = f;
                    SetEvent(arrived);
                }
            });
        session.StartCapture();
        for (int i = 0; i < 100 && MsgWaitForMultipleObjects(1, &arrived, FALSE, 50, QS_ALLINPUT) != WAIT_OBJECT_0; ++i)
        {
            pump(1);
            // A window not changing may make no new frame: ask for one.
            if (i % 5 == 4) RedrawWindow(top, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        if (got)
        {
            winrt::com_ptr<ID3D11Texture2D> texture;
            got.Surface().as<dxgi_access>()->GetInterface(IID_PPV_ARGS(texture.put()));
            D3D11_TEXTURE2D_DESC d{};
            texture->GetDesc(&d);
            const auto size = got.ContentSize();
            const UINT w = std::min<UINT>(d.Width, UINT(size.Width)), h = std::min<UINT>(d.Height, UINT(size.Height));
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            d.MiscFlags = 0;
            winrt::com_ptr<ID3D11Texture2D> staging;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(d3d->CreateTexture2D(&d, nullptr, staging.put())))
            {
                context->CopyResource(staging.get(), texture.get());
                if (SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m)))
                {
                    out.w = int(w);
                    out.h = int(h);
                    out.px.resize(size_t(w) * h);
                    for (UINT y = 0; y < h; ++y)
                        std::memcpy(out.px.data() + size_t(y) * w, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch,
                                    size_t(w) * 4);
                    context->Unmap(staging.get(), 0);
                }
            }
        }
        session.Close();
        pool.Close();
        CloseHandle(arrived);
        return out;
    }
};

int channel(uint32_t v, int shift)
{
    return int((v >> shift) & 0xff);
}

RECT rect_in(HWND top, HWND child)
{
    RECT r{};
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, top, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

}  // namespace

int main(int argc, char** argv)
{
    const DWORD build = windows_build();
    if (build < 22000) return skip("before Windows 11 (build " + str(long(build)) + "): no tabs in the caption, no band");
    if (high_contrast()) return skip("high contrast: the band's opaque and off the extended frame");
    try
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    }
    catch (...)
    {
    }
    bool supported = false;
    try
    {
        supported = wgc::GraphicsCaptureSession::IsSupported();
    }
    catch (...)
    {
    }
    if (!supported) return skip("Windows.Graphics.Capture isn't supported here");

    rap::load_options options{.backend = "win32"};
    options.look.color_scheme = rap::color_scheme::dark;
    auto ui = rap::ui::load(argc, argv, options);
    if (!ui) return 2;
    char theme[16]{};
    GetEnvironmentVariableA("RAPPORT_THEME", theme, sizeof theme);
    const bool dark = std::strcmp(theme, "light") != 0;
    std::printf("build %lu, %s\n", build, dark ? "dark" : "light");

    using namespace rap::layout;
    const char* title = "Caption glass";
    auto win = (*ui)->create(
        window{title,
               {toolbar{tool_button{rap::icon::named("go-previous"), "Back"},
                        tool_button{rap::icon::named("go-next"), "Forward"}, separator{}, button{"Go"},
                        menu_button{"Menu"}, check{"Check"}, label{"Label"}, drop_down{{"One", "Two"}},
                        combo{{"One", "Two"}, "One"}, hbox{tool_button{rap::icon::named("go-up"), "Tool"}},
                        entry{"Text"}.clearable().stretch()},
                label{"Content"}}}
            .size(900_px, 300_px)
            .base_surface(true)
            .tabs(document_tabs{{{"a", "Alpha", {}, {}}, {"b", "Beta", {}, {}}}}.current("a")));
    win->show();
    pump(50);
    HWND top = FindWindowW(nullptr, L"Caption glass");
    HWND strip = top ? FindWindowExW(top, nullptr, L"rapportCaptionTabs", nullptr) : nullptr;
    HWND edit = top ? FindWindowExW(top, nullptr, L"Edit", nullptr) : nullptr;
    expect(top && strip && edit, "the window, its tabs in the caption, its text field");
    if (!top || !strip || !edit) return 1;
    // Wholly on its monitor, at its work area's top left (a runner's screen
    // may be 1024 wide): what's off it isn't composed as on it.
    {
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(top, MONITOR_DEFAULTTONEAREST), &mi);
        RECT w{}, efb{};
        GetWindowRect(top, &w);
        DwmGetWindowAttribute(top, DWMWA_EXTENDED_FRAME_BOUNDS, &efb, sizeof efb);
        SetWindowPos(top, nullptr, mi.rcWork.left + 8 - (efb.left - w.left), mi.rcWork.top + 8 - (efb.top - w.top), 0,
                     0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump(20);
        GetWindowRect(top, &w);
        DwmGetWindowAttribute(top, DWMWA_EXTENDED_FRAME_BOUNDS, &efb, sizeof efb);
        note("window " + str(w.left) + "," + str(w.top) + ".." + str(w.right) + "," + str(w.bottom) + ", its frame " +
             str(efb.left) + "," + str(efb.top) + ".." + str(efb.right) + "," + str(efb.bottom) + ", the work area " +
             str(mi.rcWork.left) + "," + str(mi.rcWork.top) + ".." + str(mi.rcWork.right) + "," +
             str(mi.rcWork.bottom) + (GetForegroundWindow() == top ? ", active" : ", inactive"));
        // Off the screen the compositor needn't put the backdrop under a
        // window's extended frame, and its pixels there could come out the
        // same in both frames, alpha 1 to this test. (The first version's
        // 1300 px window, on the runner's 1024 px screen, read 1.00 on the
        // current tab though not on the band: the window's now sized and
        // placed to fit, and the strip's pixel is read back from its own DC
        // too, which needs no compositor.)
        expect(efb.left >= mi.rcMonitor.left && efb.top >= mi.rcMonitor.top && efb.right <= mi.rcMonitor.right &&
                   efb.bottom <= mi.rcMonitor.bottom,
               "the window wholly on its monitor, where the compositor composes it");
    }

    capturer cap;
    if (!cap.make()) return skip("no Direct3D 11 device to capture with");
    // The window's client area in the frame: from the extended frame bounds.
    RECT bounds{};
    DwmGetWindowAttribute(top, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof bounds);
    POINT origin{0, 0};
    ClientToScreen(top, &origin);
    const int ox = origin.x - bounds.left, oy = origin.y - bounds.top;

    BOOL was = FALSE;
    DwmGetWindowAttribute(top, 20, &was, sizeof was);
    frame f[2];
    for (int i = 0; i < 2; ++i)
    {
        const BOOL frame_dark = i;
        DwmSetWindowAttribute(top, 20, &frame_dark, sizeof frame_dark);
        pump(10);
        DwmFlush();
        f[i] = cap.grab(top);
    }
    DwmSetWindowAttribute(top, 20, &was, sizeof was);
    if (f[0].px.empty() || f[1].px.empty()) return skip("no frame arrived from Windows.Graphics.Capture");
    // RAPPORT_GLASS_DUMP=dir: both frames written there, raw BGRA with their
    // width and height first, to look at.
    {
        char dir[MAX_PATH]{};
        if (GetEnvironmentVariableA("RAPPORT_GLASS_DUMP", dir, sizeof dir))
            for (int i = 0; i < 2; ++i)
                if (FILE* out = std::fopen((std::string(dir) + "\\frame" + str(i) + ".bgra").c_str(), "wb"))
                {
                    const int32_t size[2] = {f[i].w, f[i].h};
                    std::fwrite(size, sizeof size, 1, out);
                    std::fwrite(f[i].px.data(), 4, f[i].px.size(), out);
                    std::fclose(out);
                }
    }
    expect(f[0].w == f[1].w && f[0].h == f[1].h, "both frames the same size, " + str(f[0].w) + "x" + str(f[0].h));

    // The backdrop alone: the strip's empty part past "+".
    const UINT q = RegisterWindowMessageW(L"rapport-caption-tabs");
    RECT add{}, client{};
    SendMessageW(strip, q, MAKEWPARAM(5, 0), reinterpret_cast<LPARAM>(&add));
    const RECT s = rect_in(top, strip);
    GetClientRect(top, &client);
    const POINT ref{s.left + (add.right + (s.right - s.left)) / 2, s.top + (s.bottom - s.top) / 2};
    const uint32_t b0 = f[0].at(ref.x + ox, ref.y + oy), b1 = f[1].at(ref.x + ox, ref.y + oy);
    const int db = channel(b0, 16) + channel(b0, 8) + channel(b0, 0) - channel(b1, 16) - channel(b1, 8) - channel(b1, 0);
    char hexes[64];
    std::snprintf(hexes, sizeof hexes, "#%06X then #%06X", b0 & 0xffffff, b1 & 0xffffff);
    note(std::string("the backdrop past \"+\", frame light then dark: ") + hexes);
    if (std::abs(db) < 3 * 40)
        return skip("the backdrop didn't change enough between a light and a dark frame to tell alpha by (" + str(db / 3) +
                    " levels): the printed-pixel checks (win32-caption-tabs) are the gate");
    auto alpha = [&](int x, int y)
    {
        const uint32_t p0 = f[0].at(x + ox, y + oy), p1 = f[1].at(x + ox, y + oy);
        const int dp = channel(p0, 16) + channel(p0, 8) + channel(p0, 0) - channel(p1, 16) - channel(p1, 8) - channel(p1, 0);
        return 1.0 - double(dp) / db;
    };

    // The band's rows: from the strip's bottom down to its line, which the
    // window's erase prints (as win32-caption-tabs reads it); its line the
    // last row. Every control in it is checked where it is.
    const int from = s.bottom;
    int line = from;
    {
        RECT bar = rect_in(top, FindWindowExW(top, nullptr, L"ToolbarWindow32", nullptr));
        line = bar.bottom;  // the band's to the row's bottom, then the line
        for (HWND c = GetWindow(top, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
            if (c != strip && (GetWindowLongPtrW(c, GWL_STYLE) & WS_VISIBLE))
            {
                const RECT r = rect_in(top, c);
                if (r.top < from + 60 && r.bottom > from) line = std::max<int>(line, r.bottom);
            }
    }
    const double layer = (dark ? 0x73 : 0xB3) / 255.0, tolerance = 0.08;
    // The line: the first row from the bottom of the controls that's opaque
    // all along (the row under the toolbar's).
    int line_row = -1;
    for (int y = line; y < line + 8 && line_row < 0; ++y)
    {
        bool opaque = true;
        for (int x = 2; x < client.right - 2 && opaque; x += 7) opaque = alpha(x, y) > 1 - tolerance;
        if (opaque) line_row = y;
    }
    expect(line_row >= 0, "the line under the band, opaque, found at row " + str(line_row));
    const int bottom = line_row >= 0 ? line_row : line;
    long checked = 0, holes = 0, capture_alpha = 0;
    double least = 2;
    POINT worst{};
    for (int y = from; y < bottom; ++y)
        for (int x = 2; x < client.right - 2; ++x)
        {
            const double a = alpha(x, y);
            ++checked;
            if ((f[0].at(x + ox, y + oy) >> 24) != 255) ++capture_alpha;
            if (a < least)
            {
                least = a;
                worst = {x, y};
            }
            if (a < layer - tolerance) ++holes;
        }
    expect(capture_alpha == 0, "the capture's own alpha 255 throughout the band's rows (" + str(capture_alpha) +
                                   " not; it can't see a hole, the backdrop's composed in)");
    expect(checked > 0 && holes == 0, "no holes: every pixel of the band's rows, " + str(from) + ".." + str(bottom) +
                                          ", at least the layer's alpha, " + fixed(layer) + " (" + str(holes) +
                                          " of " + str(checked) + " below; the least " + fixed(least) + " at " +
                                          str(worst.x) + "," + str(worst.y) + ")");
    // The text field's inside: opaque.
    {
        const RECT e = rect_in(top, edit);
        long under = 0, n = 0;
        for (int y = e.top + 3; y < e.bottom - 3; ++y)
            for (int x = e.left + 3; x < e.right - 3; ++x, ++n)
                if (alpha(x, y) < 1 - tolerance) ++under;
        expect(n > 0 && under == 0, "the text field opaque inside (" + str(under) + " of " + str(n) + " not)");
    }
    // The current tab: the layer, as the band. Its fill above its title
    // (from 2 to 6 px down, past its corners, before its close button),
    // each pixel's alpha, the median asserted.
    {
        const int current = int(SendMessageW(strip, q, MAKEWPARAM(1, 0), 0));
        RECT tab{}, close{};
        SendMessageW(strip, q, MAKEWPARAM(3, current), reinterpret_cast<LPARAM>(&tab));
        SendMessageW(strip, q, MAKEWPARAM(4, current), reinterpret_cast<LPARAM>(&close));
        std::vector<double> fill;
        for (int y = tab.top + 2; y <= tab.top + 6; ++y)
            for (int x = tab.left + 12; x < (IsRectEmpty(&close) ? tab.right - 12 : close.left); ++x)
                fill.push_back(alpha(s.left + x, s.top + y));
        std::sort(fill.begin(), fill.end());
        const double median = fill.empty() ? -1 : fill[fill.size() / 2];
        // What's there, to tell why if it isn't: the two frames' pixels, the
        // strip printed, and the strip's pixel read back from its own DC.
        auto px = [&](const frame& fr, int x, int y)
        {
            char b[16];
            std::snprintf(b, sizeof b, "%08X", fr.at(x + ox, y + oy));
            return std::string(b);
        };
        const int sx = s.left + tab.left + 20, sy = s.top + tab.top + 4;
        std::string printed = "?", readback = "?";
        {
            RECT c{};
            GetClientRect(strip, &c);
            HDC screen = GetDC(nullptr);
            HDC mem = CreateCompatibleDC(screen);
            BITMAPINFO bi{};
            bi.bmiHeader = {sizeof(BITMAPINFOHEADER), c.right, -c.bottom, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ old = SelectObject(mem, bmp);
            auto* p = static_cast<uint32_t*>(bits);
            std::fill_n(p, size_t(c.right) * c.bottom, 0u);
            SendMessageW(strip, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT);
            GdiFlush();
            char b[16];
            std::snprintf(b, sizeof b, "%08X", p[size_t(tab.top + 4) * c.right + tab.left + 20]);
            printed = b;
            std::fill_n(p, size_t(c.right) * c.bottom, 0x12345678u);
            HDC dc = GetDC(strip);
            BitBlt(mem, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
            ReleaseDC(strip, dc);
            GdiFlush();
            std::snprintf(b, sizeof b, "%08X", p[size_t(tab.top + 4) * c.right + tab.left + 20]);
            readback = b;
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            ReleaseDC(nullptr, screen);
        }
        note("the current tab, " + str(current) + ", at " + str(tab.left) + "," + str(tab.top) + ".." + str(tab.right) +
             "," + str(tab.bottom) + " in the strip at " + str(s.left) + "," + str(s.top) + "; at " + str(sx) + "," +
             str(sy) + " the frames " + px(f[0], sx, sy) + " then " + px(f[1], sx, sy) + ", printed " + printed +
             ", read back from its DC " + readback + "; its middle " + px(f[0], s.left + (tab.left + tab.right) / 2, s.top + tab.bottom - 3) +
             " then " + px(f[1], s.left + (tab.left + tab.right) / 2, s.top + tab.bottom - 3) + "; the strip left of it " +
             px(f[0], s.left + 3, s.top + 20) + " then " + px(f[1], s.left + 3, s.top + 20) + "; least and most " +
             (fill.empty() ? std::string("-") : fixed(fill.front()) + ", " + fixed(fill.back())));
        // The strip's own pixel, read back from its DC (the window's
        // surface, before the compositor): as printed, alpha and all.
        expect(readback == printed, "the current tab's pixel on the window's surface as printed: " + readback);
        expect(std::fabs(median - layer) < tolerance, "the current tab at the layer's alpha, " + fixed(layer) +
                                                          ": " + fixed(median) + " (the median of " +
                                                          str(long(fill.size())) + " pixels of its fill)");
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
