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
            .size(1300_px, 300_px)
            .base_surface(true)
            .tabs(document_tabs{{{"a", "Alpha", {}, {}}, {"b", "Beta", {}, {}}}}.current("a")));
    win->show();
    pump(50);
    HWND top = FindWindowW(nullptr, L"Caption glass");
    HWND strip = top ? FindWindowExW(top, nullptr, L"rapportCaptionTabs", nullptr) : nullptr;
    HWND edit = top ? FindWindowExW(top, nullptr, L"Edit", nullptr) : nullptr;
    expect(top && strip && edit, "the window, its tabs in the caption, its text field");
    if (!top || !strip || !edit) return 1;

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
    // The current tab: the layer, as the band.
    {
        RECT tab{};
        SendMessageW(strip, q, MAKEWPARAM(3, 0), reinterpret_cast<LPARAM>(&tab));
        const double a = alpha(s.left + tab.left + 20, s.top + tab.top + 4);
        expect(std::fabs(a - layer) < tolerance, "the current tab at the layer's alpha, " + fixed(layer) + ": " + fixed(a));
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
