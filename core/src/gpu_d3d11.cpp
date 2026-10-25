// rap::gpu on Direct3D 11. One immediate context, under a lock: the device's
// own, or the Direct2D factory's when Direct2D draws on the same device (the
// Win32 canvas), so the two never interleave. Progress is a fence (Windows
// 10 1703 on) or, without one, event queries. A thread per device waits on
// it to finish readbacks and to drop what passes held.
#include "rapport/gpu_d3d11.h"

#include "gpu_internal.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d2d1_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace rap::gpu
{
namespace
{

// rapport's vertex stage: one triangle over the whole target, (-1, 1),
// (3, 1), (-1, -3) from SV_VertexID. FXC, vs_4_0, from:
//   float4 main(uint id : SV_VertexID) : SV_Position
//   { const float2 p = float2((id << 1) & 2, id & 2);
//     return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }
const BYTE k_fullscreen_vs[] = {
    68, 88, 66, 67, 204, 26, 106, 34, 49, 3, 239, 240, 157, 159, 136, 15,
    14, 95, 234, 211, 1, 0, 0, 0, 148, 1, 0, 0, 3, 0, 0, 0,
    44, 0, 0, 0, 96, 0, 0, 0, 148, 0, 0, 0, 73, 83, 71, 78,
    44, 0, 0, 0, 1, 0, 0, 0, 8, 0, 0, 0, 32, 0, 0, 0,
    0, 0, 0, 0, 6, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 0, 0, 83, 86, 95, 86, 101, 114, 116, 101, 120, 73, 68, 0,
    79, 83, 71, 78, 44, 0, 0, 0, 1, 0, 0, 0, 8, 0, 0, 0,
    32, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0,
    0, 0, 0, 0, 15, 0, 0, 0, 83, 86, 95, 80, 111, 115, 105, 116,
    105, 111, 110, 0, 83, 72, 68, 82, 248, 0, 0, 0, 64, 0, 1, 0,
    62, 0, 0, 0, 96, 0, 0, 4, 18, 16, 16, 0, 0, 0, 0, 0,
    6, 0, 0, 0, 103, 0, 0, 4, 242, 32, 16, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 104, 0, 0, 2, 1, 0, 0, 0, 41, 0, 0, 7,
    18, 0, 16, 0, 0, 0, 0, 0, 10, 16, 16, 0, 0, 0, 0, 0,
    1, 64, 0, 0, 1, 0, 0, 0, 1, 0, 0, 7, 18, 0, 16, 0,
    0, 0, 0, 0, 10, 0, 16, 0, 0, 0, 0, 0, 1, 64, 0, 0,
    2, 0, 0, 0, 1, 0, 0, 7, 66, 0, 16, 0, 0, 0, 0, 0,
    10, 16, 16, 0, 0, 0, 0, 0, 1, 64, 0, 0, 2, 0, 0, 0,
    86, 0, 0, 5, 50, 0, 16, 0, 0, 0, 0, 0, 134, 0, 16, 0,
    0, 0, 0, 0, 50, 0, 0, 15, 50, 32, 16, 0, 0, 0, 0, 0,
    70, 0, 16, 0, 0, 0, 0, 0, 2, 64, 0, 0, 0, 0, 0, 64,
    0, 0, 0, 192, 0, 0, 0, 0, 0, 0, 0, 0, 2, 64, 0, 0,
    0, 0, 128, 191, 0, 0, 128, 63, 0, 0, 0, 0, 0, 0, 0, 0,
    54, 0, 0, 8, 194, 32, 16, 0, 0, 0, 0, 0, 2, 64, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 63,
    62, 0, 0, 1,
};

DXGI_FORMAT dxgi_format(format f)
{
    switch (f)
    {
        case format::r8: return DXGI_FORMAT_R8_UNORM;
        case format::rg8: return DXGI_FORMAT_R8G8_UNORM;
        case format::rgba8: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case format::rgba8_srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case format::r16: return DXGI_FORMAT_R16_UNORM;
        case format::rg16: return DXGI_FORMAT_R16G16_UNORM;
        case format::rgba16: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case format::r16_snorm: return DXGI_FORMAT_R16_SNORM;
        case format::rg16_snorm: return DXGI_FORMAT_R16G16_SNORM;
        case format::rgba16_snorm: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case format::r16f: return DXGI_FORMAT_R16_FLOAT;
        case format::rg16f: return DXGI_FORMAT_R16G16_FLOAT;
        case format::rgba16f: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case format::r32f: return DXGI_FORMAT_R32_FLOAT;
        case format::rg32f: return DXGI_FORMAT_R32G32_FLOAT;
        case format::rgba32f: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

// Done once set: a readback's, finished by the device's thread.
class flag_line : public timeline
{
public:
    uint64_t completed() const override
    {
        std::lock_guard<std::mutex> l(m_);
        return set_ ? 1 : 0;
    }
    void wait(uint64_t) const override
    {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return set_; });
    }
    void set()
    {
        {
            std::lock_guard<std::mutex> l(m_);
            set_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    bool set_ = false;
};

class d3d_device;

class d3d_texture : public texture
{
public:
    int width() const override { return w; }
    int height() const override { return h; }
    gpu::format format() const override { return f; }

    const d3d_device* owner = nullptr;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    std::vector<ComPtr<ID3D11RenderTargetView>> rtv;  // per level, made when first drawn into
    int w = 0, h = 0, levels = 1;
    gpu::format f = format::rgba8;
    bool render_target = false;
};

class d3d_program : public program
{
public:
    const d3d_device* owner = nullptr;
    ComPtr<ID3D11PixelShader> ps;
};

class d3d_device : public device, public std::enable_shared_from_this<d3d_device>
{
public:
    // The lock around the immediate context.
    class lock_t
    {
    public:
        explicit lock_t(const d3d_device& d) : d_(d)
        {
            if (d_.d2d_lock_)
                d_.d2d_lock_->Enter();
            else
                d_.own_lock_.lock();
        }
        ~lock_t()
        {
            if (d_.d2d_lock_)
                d_.d2d_lock_->Leave();
            else
                d_.own_lock_.unlock();
        }

    private:
        const d3d_device& d_;
    };

    // The device's timeline: the fence, or queries without one.
    // A ticket outliving its device is done: nothing it waited for is left.
    class line : public timeline
    {
    public:
        explicit line(std::weak_ptr<d3d_device> d) : d_(std::move(d)) {}
        uint64_t completed() const override
        {
            auto d = d_.lock();
            return d ? d->completed() : UINT64_MAX;
        }
        void wait(uint64_t v) const override
        {
            if (auto d = d_.lock()) d->wait_for(v);
        }

    private:
        std::weak_ptr<d3d_device> d_;
    };

    static std::shared_ptr<d3d_device> make(ID3D11Device* d3d, ID2D1Multithread* lock, std::string* error)
    {
        // The last reference can go on the device's own worker (an app's
        // readback callback letting go): it's deleted from another thread
        // then, which joins the worker once it's done with that item.
        auto d = std::shared_ptr<d3d_device>(new d3d_device,
                                             [](d3d_device* p)
                                             {
                                                 if (p->worker_.get_id() == std::this_thread::get_id())
                                                     std::thread([p] { delete p; }).detach();
                                                 else
                                                     delete p;
                                             });
        d->d3d_ = d3d;
        d->d2d_lock_ = lock;
        d3d->GetImmediateContext(d->ctx_.GetAddressOf());
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(d->ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);
        ComPtr<ID3D11Device5> d5;
        if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(d5.GetAddressOf()))) && SUCCEEDED(d->ctx_.As(&d->ctx4_)) &&
            SUCCEEDED(d5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(d->fence_.GetAddressOf()))))
            ;
        else
            d->fence_.Reset();
        const D3D11_SAMPLER_DESC base{D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP,
                                      D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1,
                                      D3D11_COMPARISON_NEVER, {}, 0, D3D11_FLOAT32_MAX};
        D3D11_SAMPLER_DESC s = base;
        s.MaxLOD = 0;
        const bool ok_samplers = SUCCEEDED(d3d->CreateSamplerState(&s, d->samplers_[0].GetAddressOf())) &&
                                 (s.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,
                                  SUCCEEDED(d3d->CreateSamplerState(&s, d->samplers_[1].GetAddressOf()))) &&
                                 (s = base, s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                                  SUCCEEDED(d3d->CreateSamplerState(&s, d->samplers_[2].GetAddressOf())));
        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID;
        r.CullMode = D3D11_CULL_NONE;
        r.DepthClipEnable = TRUE;
        r.ScissorEnable = TRUE;
        if (!ok_samplers || FAILED(d3d->CreateRasterizerState(&r, d->raster_.GetAddressOf())) ||
            FAILED(d3d->CreateVertexShader(k_fullscreen_vs, sizeof k_fullscreen_vs, nullptr, d->vs_.GetAddressOf())))
        {
            if (error) *error = "the device's fixed state couldn't be made";
            return nullptr;
        }
        ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf()))) &&
            SUCCEEDED(dxgi->GetAdapter(d->adapter_.GetAddressOf())))
        {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(d->adapter_->GetDesc(&desc)))
                d->software_ = desc.VendorId == 0x1414 && desc.DeviceId == 0x8c;  // Microsoft Basic Render (WARP)
        }
        d->max_size_ = d3d->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION : 8192;
        d->line_ = std::make_shared<line>(d);
        d->worker_ = std::thread([raw = d.get()] { raw->work(); });
        return d;
    }

    ~d3d_device() override
    {
        {
            std::lock_guard<std::mutex> l(work_m_);
            stop_ = true;
        }
        work_cv_.notify_all();
        if (worker_.joinable()) worker_.join();  // never the worker itself: see make()
    }

    std::string_view api() const override { return "d3d11"; }
    bool software() const override { return software_; }
    bool takes(shader_language l) const override
    {
        return l == shader_language::dxbc || (l == shader_language::hlsl && compiler());
    }
    memory_budget memory() const override
    {
        ComPtr<IDXGIAdapter3> a3;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (adapter_ && SUCCEEDED(adapter_.As(&a3)) &&
            SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return {info.Budget, info.CurrentUsage};
        return {};
    }
    int max_texture_size() const override { return max_size_; }
    bool lost() const override { return d3d_->GetDeviceRemovedReason() != S_OK; }

    texture_ptr create(const texture_desc& desc) override
    {
        const int levels = std::max(1, desc.levels);
        if (desc.width <= 0 || desc.height <= 0 || desc.width > max_size_ || desc.height > max_size_ ||
            (std::max(desc.width, desc.height) >> (levels - 1)) < 1)
            return nullptr;
        auto t = std::make_shared<d3d_texture>();
        t->owner = this;
        t->w = desc.width, t->h = desc.height, t->levels = levels, t->f = desc.format;
        t->render_target = desc.render_target;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(desc.width);
        td.Height = UINT(desc.height);
        td.MipLevels = UINT(levels);
        td.ArraySize = 1;
        td.Format = dxgi_format(desc.format);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (desc.render_target ? D3D11_BIND_RENDER_TARGET : 0);
        if (FAILED(d3d_->CreateTexture2D(&td, nullptr, t->tex.GetAddressOf())) ||
            FAILED(d3d_->CreateShaderResourceView(t->tex.Get(), nullptr, t->srv.GetAddressOf())))
            return nullptr;
        t->rtv.resize(size_t(levels));
        return t;
    }

    ticket upload(const texture_ptr& tp, std::span<const std::byte> pixels, const source_layout& layout, int level,
                  rect region) override
    {
        auto* t = mine(tp);
        if (!t || level < 0 || level >= t->levels) return {};
        const int lw = std::max(1, t->w >> level), lh = std::max(1, t->h >> level);
        if (region.empty()) region = {0, 0, lw, lh};
        region.w = std::min(region.w, lw - region.x), region.h = std::min(region.h, lh - region.y);
        if (region.x < 0 || region.y < 0 || region.empty()) return {};
        const size_t stride = bytes_per_pixel(t->f) * size_t(region.w);
        std::vector<std::byte> staging(stride * size_t(region.h));
        convert(pixels, layout, region.w, region.h, t->f, staging.data(), stride);
        const D3D11_BOX box{UINT(region.x), UINT(region.y), 0, UINT(region.x + region.w), UINT(region.y + region.h), 1};
        lock_t l(*this);
        ctx_->UpdateSubresource(t->tex.Get(), UINT(level), &box, staging.data(), UINT(stride), 0);
        return ticket(line_, signal());
    }

    ticket readback(const texture_ptr& tp, std::span<std::byte> out, size_t stride, int level,
                    std::function<void()> done) override
    {
        auto* t = mine(tp);
        if (!t || level < 0 || level >= t->levels) return {};
        const int lw = std::max(1, t->w >> level), lh = std::max(1, t->h >> level);
        const size_t row = bytes_per_pixel(t->f) * size_t(lw);
        if (stride < row || out.size() < stride * size_t(lh - 1) + row) return {};
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(lw);
        td.Height = UINT(lh);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dxgi_format(t->f);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(d3d_->CreateTexture2D(&td, nullptr, staging.GetAddressOf()))) return {};
        auto flag = std::make_shared<flag_line>();
        uint64_t value;
        {
            lock_t l(*this);
            ctx_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, t->tex.Get(), UINT(level), nullptr);
            value = signal();
        }
        // Copied out by the device's thread once the GPU's there.
        queue(value, [this, staging, out, stride, row, lh, flag, done = std::move(done)]
              {
                  D3D11_MAPPED_SUBRESOURCE m{};
                  {
                      lock_t l(*this);
                      if (SUCCEEDED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
                      {
                          for (int y = 0; y < lh; ++y)
                              std::memcpy(out.data() + size_t(y) * stride,
                                          static_cast<const std::byte*>(m.pData) + size_t(y) * m.RowPitch, row);
                          ctx_->Unmap(staging.Get(), 0);
                      }
                  }
                  flag->set();
                  if (done) done();
              });
        return ticket(flag, 1);
    }

    program_result program(const shader& pixel) override
    {
        std::string errors;
        for (const shader::form& f : pixel.forms)
        {
            ComPtr<ID3DBlob> compiled;
            const void* code = f.code.data();
            size_t size = f.code.size();
            if (f.language == shader_language::hlsl)
            {
                auto compile = compiler();
                if (!compile)
                {
                    errors += "hlsl: no d3dcompiler_47.dll; ";
                    continue;
                }
                ComPtr<ID3DBlob> log;
                const char* target = d3d_->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? "ps_5_0" : "ps_4_0";
                if (FAILED(compile(code, size, pixel.name.c_str(), nullptr, nullptr, pixel.entry.c_str(), target,
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, compiled.GetAddressOf(), log.GetAddressOf())))
                {
                    errors += "hlsl: ";
                    if (log) errors.append(static_cast<const char*>(log->GetBufferPointer()), log->GetBufferSize());
                    errors += "; ";
                    continue;
                }
                code = compiled->GetBufferPointer();
                size = compiled->GetBufferSize();
            }
            else if (f.language != shader_language::dxbc)
                continue;
            auto p = std::make_shared<d3d_program>();
            p->owner = this;
            if (SUCCEEDED(d3d_->CreatePixelShader(code, size, nullptr, p->ps.GetAddressOf())))
                return {p, {}};
            errors += "dxbc: not a pixel shader this device takes; ";
        }
        return {nullptr, (pixel.name.empty() ? "shader" : pixel.name) + ": " +
                             (errors.empty() ? std::string("no form Direct3D 11 takes (dxbc or hlsl)") : errors)};
    }

    ticket run(std::span<const pass> passes) override
    {
        std::vector<std::shared_ptr<const void>> held;
        lock_t l(*this);
        for (const pass& p : passes)
        {
            auto* prog = dynamic_cast<const d3d_program*>(p.program.get());
            auto* target = mine(p.target);
            if (!prog || prog->owner != this || !target || !target->render_target || p.target_level < 0 ||
                p.target_level >= target->levels)
                continue;
            const UINT level = UINT(p.target_level);
            ComPtr<ID3D11RenderTargetView>& rtv = target->rtv[level];
            if (!rtv)
            {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format = dxgi_format(target->f);
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                rd.Texture2D.MipSlice = level;
                if (FAILED(d3d_->CreateRenderTargetView(target->tex.Get(), &rd, rtv.GetAddressOf()))) continue;
            }
            const int lw = std::max(1, target->w >> level), lh = std::max(1, target->h >> level);
            rect r = p.region.empty() ? rect{0, 0, lw, lh} : p.region;

            // Inputs unbound first: one may have been the last pass's target.
            ID3D11ShaderResourceView* none[16] = {};
            ctx_->PSSetShaderResources(0, 16, none);
            ctx_->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            const D3D11_VIEWPORT vp{0, 0, float(lw), float(lh), 0, 1};
            ctx_->RSSetViewports(1, &vp);
            const D3D11_RECT sc{r.x, r.y, r.x + r.w, r.y + r.h};
            ctx_->RSSetScissorRects(1, &sc);
            ctx_->RSSetState(raster_.Get());
            ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            ctx_->OMSetDepthStencilState(nullptr, 0);
            ctx_->IASetInputLayout(nullptr);
            ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx_->VSSetShader(vs_.Get(), nullptr, 0);
            ctx_->GSSetShader(nullptr, nullptr, 0);
            ctx_->PSSetShader(prog->ps.Get(), nullptr, 0);

            ID3D11ShaderResourceView* srvs[16] = {};
            ID3D11SamplerState* samplers[16] = {};
            const size_t n = std::min<size_t>(16, p.inputs.size());
            for (size_t i = 0; i < n; ++i)
            {
                if (auto* in = mine(p.inputs[i])) srvs[i] = in->srv.Get();
                const sampler s = i < p.samplers.size() ? p.samplers[i] : sampler::nearest_clamp;
                samplers[i] = samplers_[int(s)].Get();
                held.push_back(p.inputs[i]);
            }
            ctx_->PSSetShaderResources(0, UINT(std::max<size_t>(n, 1)), srvs);
            ctx_->PSSetSamplers(0, UINT(std::max<size_t>(n, 1)), samplers);
            if (!p.uniforms.empty() && constants(p.uniforms))
                ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
            ctx_->Draw(3, 0);
            held.push_back(p.program);
            held.push_back(p.target);
        }
        ID3D11ShaderResourceView* none[16] = {};
        ctx_->PSSetShaderResources(0, 16, none);
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        const uint64_t value = signal();
        if (!held.empty()) queue(value, [held = std::move(held)] {});  // kept until the GPU's past them
        return ticket(line_, value);
    }

    // For the Win32 canvas.
    ID3D11Texture2D* native(const texture& t) const
    {
        auto* d = dynamic_cast<const d3d_texture*>(&t);
        return d && d->owner == this ? d->tex.Get() : nullptr;
    }

private:
    d3d_device() = default;

    d3d_texture* mine(const texture_ptr& t) const
    {
        auto* d = dynamic_cast<d3d_texture*>(t.get());
        return d && d->owner == this ? d : nullptr;
    }

    using compile_fn = decltype(&D3DCompile);
    static compile_fn compiler()
    {
        static compile_fn f = []() -> compile_fn
        {
            HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
            return m ? reinterpret_cast<compile_fn>(GetProcAddress(m, "D3DCompile")) : nullptr;
        }();
        return f;
    }

    // The uniform block into the constant buffer; under the lock.
    bool constants(std::span<const std::byte> u)
    {
        const UINT size = UINT((std::min<size_t>(u.size(), 65536) + 15) & ~size_t(15));
        if (!cb_ || cb_size_ < size)
        {
            cb_.Reset();
            const D3D11_BUFFER_DESC bd{size, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
            if (FAILED(d3d_->CreateBuffer(&bd, nullptr, cb_.GetAddressOf()))) return false;
            cb_size_ = size;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        std::memset(m.pData, 0, cb_size_);
        std::memcpy(m.pData, u.data(), std::min<size_t>(u.size(), cb_size_));
        ctx_->Unmap(cb_.Get(), 0);
        return true;
    }

    // The next point on the timeline, queued now; under the lock.
    uint64_t signal()
    {
        const uint64_t v = ++next_;
        if (fence_)
            ctx4_->Signal(fence_.Get(), v);
        else
        {
            ComPtr<ID3D11Query> q;
            const D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
            if (SUCCEEDED(d3d_->CreateQuery(&qd, q.GetAddressOf())))
            {
                ctx_->End(q.Get());
                std::lock_guard<std::mutex> g(queries_m_);
                queries_.emplace_back(v, q);
            }
        }
        ctx_->Flush();
        return v;
    }
    uint64_t completed() const
    {
        if (fence_) return fence_->GetCompletedValue();
        lock_t l(*this);  // before the queries' lock, as signal() takes them
        std::lock_guard<std::mutex> g(queries_m_);
        while (!queries_.empty())
        {
            const HRESULT hr = ctx_->GetData(queries_.front().second.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE) break;
            done_ = queries_.front().first;  // done, or the device is gone: either way, past it
            queries_.pop_front();
        }
        return done_;
    }
    void wait_for(uint64_t v) const
    {
        if (fence_)
        {
            if (fence_->GetCompletedValue() >= v) return;
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (e && SUCCEEDED(fence_->SetEventOnCompletion(v, e)))
                WaitForSingleObject(e, lost() ? 0 : INFINITE);
            if (e) CloseHandle(e);
            return;
        }
        while (completed() < v && !lost()) Sleep(1);
    }

    // Work for the device's thread once the GPU reaches `value`, in order.
    void queue(uint64_t value, std::function<void()> f)
    {
        {
            std::lock_guard<std::mutex> l(work_m_);
            work_.emplace_back(value, std::move(f));
        }
        work_cv_.notify_one();
    }
    void work()
    {
        for (;;)
        {
            std::pair<uint64_t, std::function<void()>> item;
            {
                std::unique_lock<std::mutex> l(work_m_);
                work_cv_.wait(l, [&] { return stop_ || !work_.empty(); });
                if (work_.empty()) return;  // stopping, and nothing left
                item = std::move(work_.front());
                work_.pop_front();
            }
            wait_for(item.first);
            item.second();
        }
    }

    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11DeviceContext4> ctx4_;
    ComPtr<ID3D11Fence> fence_;
    ComPtr<IDXGIAdapter> adapter_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11SamplerState> samplers_[3];
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11Buffer> cb_;
    UINT cb_size_ = 0;
    ID2D1Multithread* d2d_lock_ = nullptr;  // kept alive by whoever made us (the Win32 backend's device)
    mutable std::mutex own_lock_;
    std::shared_ptr<line> line_;
    uint64_t next_ = 0;  // under the lock
    mutable std::mutex queries_m_;
    mutable std::deque<std::pair<uint64_t, ComPtr<ID3D11Query>>> queries_;
    mutable uint64_t done_ = 0;
    bool software_ = false;
    int max_size_ = 16384;

    std::mutex work_m_;
    std::condition_variable work_cv_;
    std::deque<std::pair<uint64_t, std::function<void()>>> work_;
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace

std::shared_ptr<device> make_d3d11_headless(uint64_t adapter)
{
    ComPtr<IDXGIAdapter1> chosen;
    if (adapter)
    {
        ComPtr<IDXGIFactory4> factory;
        LUID luid{DWORD(adapter & 0xffffffff), LONG(adapter >> 32)};
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))))
            factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(chosen.GetAddressOf()));
    }
    ComPtr<ID3D11Device> d3d;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 flags, nullptr, 0, D3D11_SDK_VERSION, d3d.GetAddressOf(), nullptr, nullptr)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                 d3d.GetAddressOf(), nullptr, nullptr)))
        return nullptr;
    return d3d_device::make(d3d.Get(), nullptr, nullptr);
}

namespace d3d11
{

std::shared_ptr<device> wrap(ID3D11Device* d3d, ID2D1Multithread* lock)
{
    return d3d ? d3d_device::make(d3d, lock, nullptr) : nullptr;
}

ID3D11Texture2D* native(const device& dev, const texture& t)
{
    auto* d = dynamic_cast<const d3d_device*>(&dev);
    return d ? d->native(t) : nullptr;
}

}  // namespace d3d11
}  // namespace rap::gpu
