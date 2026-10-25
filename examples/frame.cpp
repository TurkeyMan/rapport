// rapport-frame: exercises canvas::present, a frame under the canvas's
// painting. The frame is made at exactly target()'s size each time it
// changes: a one-pixel checker in the top left (1:1, or it shimmers into
// grey), a one-pixel red border round the edge (all four sides visible, or
// the size is off), and a gradient. The overlay is painted over it. Where
// the canvas takes GPU frames (Windows: D3D11) the frame is rendered on a
// device of its own on the GPU target() names and handed over as a shared
// texture with a fence; otherwise as pixels. Keys: C clears the frame (back
// to on_paint alone) and shows it again; P switches to pixels and back;
// Esc quits. With --cycle [file] it resizes itself 60 times, presenting at
// each size, then prints how many frames were presented and released (also
// to `file`: a Windows GUI program's stdout often goes nowhere) and quits.
#include "rapport/rapport.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <d3d11_4.h>
#include <dxgi1_2.h>
#endif

using namespace rap;

namespace
{

std::vector<uint8_t> make_frame(int w, int h)
{
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];  // BGRA
            const bool edge = x == 0 || y == 0 || x == w - 1 || y == h - 1;
            const bool checker = x < 128 && y < 128;
            if (edge)
            {
                p[0] = 0, p[1] = 0, p[2] = 255;
            }
            else if (checker)
            {
                const uint8_t v = (x ^ y) & 1 ? 255 : 0;
                p[0] = p[1] = p[2] = v;
            }
            else
            {
                p[0] = uint8_t(255 * y / std::max(1, h - 1));
                p[1] = 96;
                p[2] = uint8_t(255 * x / std::max(1, w - 1));
            }
            p[3] = 255;
        }
    return px;
}

#ifdef _WIN32
// The app's side of a D3D11 frame: a device on the canvas's adapter, and
// shared textures (two, so one can be drawn while the other's still read)
// signalled on a shared fence.
class d3d11_frames
{
public:
    ~d3d11_frames()
    {
        for (slot& s : slots_) close(s);
        if (fence_handle_) CloseHandle(fence_handle_);
        if (fence_) fence_->Release();
        if (context_) context_->Release();
        if (device_) device_->Release();
    }
    bool ready_for(uint64_t luid)
    {
        if (device_ && luid == luid_) return true;
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i)
        {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            if ((uint64_t(d.AdapterLuid.LowPart) | uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32) == luid) break;
            adapter->Release();
            adapter = nullptr;
        }
        factory->Release();
        if (!adapter) return false;
        ID3D11Device* device = nullptr;
        const HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &device, nullptr, nullptr);
        adapter->Release();
        if (FAILED(hr)) return false;
        ID3D11Device5* device5 = nullptr;
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        if (FAILED(device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&device5))) ||
            FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&context_))) ||
            FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
                                        reinterpret_cast<void**>(&fence_))) ||
            FAILED(fence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle_)))
        {
            if (device5) device5->Release();
            context->Release();
            device->Release();
            return false;
        }
        context->Release();
        device_ = device5;
        device->Release();
        luid_ = luid;
        return true;
    }
    // A frame of `px` (BGRA, premultiplied), or nothing when both are busy.
    std::optional<rap::gpu_frame> make(int w, int h, const std::vector<uint8_t>& px)
    {
        slot* free = nullptr;
        for (slot& s : slots_)
            if (!s.busy && (!free || (s.w == w && s.h == h))) free = &s;
        if (!free) return std::nullopt;
        if (free->w != w || free->h != h)
        {
            close(*free);
            D3D11_TEXTURE2D_DESC d{};
            d.Width = UINT(w);
            d.Height = UINT(h);
            d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            IDXGIResource1* resource = nullptr;
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &free->texture)) ||
                FAILED(free->texture->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource))) ||
                FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &free->handle)))
            {
                if (resource) resource->Release();
                close(*free);
                return std::nullopt;
            }
            resource->Release();
            free->w = w, free->h = h;
        }
        context_->UpdateSubresource(free->texture, 0, nullptr, px.data(), UINT(w) * 4, 0);
        context_->Signal(fence_, ++fence_value_);
        context_->Flush();
        free->busy = true;
        rap::gpu_frame f;
        f.width = w, f.height = h;
        f.handle = rap::gpu_frame::d3d11{free->handle, DXGI_FORMAT_B8G8R8A8_UNORM, fence_handle_, fence_value_};
        f.release = [free] { free->busy = false; };
        return f;
    }

private:
    struct slot
    {
        ID3D11Texture2D* texture = nullptr;
        HANDLE handle = nullptr;
        int w = 0, h = 0;
        bool busy = false;
    };
    static void close(slot& s)
    {
        if (s.handle) CloseHandle(s.handle);
        if (s.texture) s.texture->Release();
        s = {};
    }
    ID3D11Device5* device_ = nullptr;
    ID3D11DeviceContext4* context_ = nullptr;
    ID3D11Fence* fence_ = nullptr;
    HANDLE fence_handle_ = nullptr;
    uint64_t fence_value_ = 0, luid_ = 0;
    slot slots_[2];
};
#endif

}  // namespace

int main(int argc, char** argv)
{
    auto ui = ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "%s\n", ui.error().c_str());
        return 1;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("backend: %s\n", std::string((*ui)->backend_name()).c_str());

#ifdef _WIN32
    d3d11_frames gpu;  // before the window: the canvas releases into it as it goes
#endif
    rap::canvas* canvas = nullptr;
    std::unique_ptr<rap::window> win;
    bool shown = true, pixels_only = false;
    int presented = 0, gpu_presented = 0, released = 0, skipped = 0;
    std::string how = "-";

    auto present = [&]
    {
        const frame_target t = canvas->target();
        if (!shown || t.width <= 0 || t.height <= 0) return;
        const auto px = make_frame(t.width, t.height);
#ifdef _WIN32
        if (!pixels_only && t.kind == frame_target::api::d3d11 && gpu.ready_for(t.device))
            if (auto f = gpu.make(t.width, t.height, px))
            {
                f->release = [inner = std::move(f->release), &released]
                {
                    ++released;
                    inner();
                };
                canvas->present(std::move(*f));
                ++gpu_presented;
                how = "d3d11";
                ++presented;
                return;
            }
#endif
        if (!pixels_only && t.kind == frame_target::api::d3d11) ++skipped;  // both textures still being read
        how = "pixels";
        std::shared_ptr<const image> frame =
            (*ui)->create_image(t.width, t.height, pixel_format::bgra8_premultiplied, px.data(), size_t(t.width) * 4);
        canvas->present(std::move(frame));
        ++presented;
    };

    using namespace rap::layout;
    win = (*ui)->create(layout::window{
        "rapport frame",
        {vbox{
            layout::canvas{}
                .on_paint(
                    [&](painter& p, size_f size)
                    {
                        const frame_target t = canvas->target();
                        char buf[200];
                        std::snprintf(buf, sizeof buf, "%s (%s): target %dx%d at %.3g, canvas %.1fx%.1f, device %.3g, #%d",
                                      shown ? "frame" : "no frame", how.c_str(), t.width, t.height, double(t.scale),
                                      double(size.w), double(size.h), double(canvas->device_scale()), presented);
                        p.rect({140, 20, 580, 28}, 4, color{0, 0, 0, 160}, std::nullopt);
                        p.text({150, 26}, buf, {.fg = color{255, 255, 255}});
                        p.rect({140, 60, 120, 60}, 0, std::nullopt, color{0, 255, 0});
                    })
                .on_key(
                    [&](const key_event& e)
                    {
                        if (!e.press) return false;
                        if (e.key == key::escape) return (*ui)->quit(), true;
                        if (e.key == key::character && e.codepoint == U'c')
                        {
                            shown = !shown;
                            if (shown)
                                present();
                            else
                                canvas->clear_frame();
                            return true;
                        }
                        if (e.key == key::character && e.codepoint == U'p')
                        {
                            pixels_only = !pixels_only;
                            present();
                            return true;
                        }
                        return false;
                    })
                .bind(canvas)
                .stretch(),
        }},
    });
    win->resize({720, 480});
    canvas->on_frame_target([&](const frame_target&) { present(); });
    win->show();
    canvas->focus();
    present();

    subscription cycle;
    int step = 0;
    if (argc > 1 && std::string(argv[1]) == "--cycle")
        cycle = (*ui)->timer(30, true,
                             [&]
                             {
                                 if (++step <= 60)
                                 {
                                     win->resize({float(600 + (step % 7) * 23), float(400 + (step % 5) * 17)});
                                     return;
                                 }
                                 char line[160];
                                 std::snprintf(line, sizeof line,
                                               "cycle: %s, presented %d (%d as textures, %d released), fell back to pixels %d\n",
                                               how.c_str(), presented, gpu_presented, released, skipped);
                                 std::fputs(line, stdout);
                                 if (argc > 2)
                                     if (std::FILE* f = std::fopen(argv[2], "w"))
                                     {
                                         std::fputs(line, f);
                                         std::fclose(f);
                                     }
                                 (*ui)->quit();
                             });
    return (*ui)->run();
}
