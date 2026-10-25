// The parts of rap::gpu every backend shares: formats, the staging
// conversion, and finding a device.
#include "rapport/gpu.h"

#include "gpu_internal.h"
#include "rapport/widgets.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rap::gpu
{

format_info info(format f)
{
    using k = format_info::kind;
    switch (f)
    {
        case format::r8: return {k::unorm8, 1};
        case format::rg8: return {k::unorm8, 2};
        case format::rgba8: return {k::unorm8, 4};
        case format::rgba8_srgb: return {k::unorm8, 4};
        case format::r16: return {k::unorm16, 1};
        case format::rg16: return {k::unorm16, 2};
        case format::rgba16: return {k::unorm16, 4};
        case format::r16_snorm: return {k::snorm16, 1};
        case format::rg16_snorm: return {k::snorm16, 2};
        case format::rgba16_snorm: return {k::snorm16, 4};
        case format::r16f: return {k::half, 1};
        case format::rg16f: return {k::half, 2};
        case format::rgba16f: return {k::half, 4};
        case format::r32f: return {k::float32, 1};
        case format::rg32f: return {k::float32, 2};
        case format::rgba32f: return {k::float32, 4};
    }
    return {k::unorm8, 4};
}

size_t bytes_per_pixel(format f)
{
    const format_info i = info(f);
    return size_t(i.channels) * i.sample_bytes();
}

namespace
{

uint16_t float_to_half(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const int32_t exp = int32_t((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (((x >> 23) & 0xff) == 0xff)  // inf, NaN
        return uint16_t(sign | 0x7c00 | (mant ? 0x200 : 0));
    if (exp >= 31) return uint16_t(sign | 0x7c00);
    if (exp <= 0)
    {
        if (exp < -10) return uint16_t(sign);
        mant |= 0x800000;
        const int shift = 14 - exp;
        uint32_t h = mant >> shift;
        const uint32_t rest = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rest > half || (rest == half && (h & 1))) ++h;
        return uint16_t(sign | h);
    }
    uint32_t h = (uint32_t(exp) << 10) | (mant >> 13);
    const uint32_t rest = mant & 0x1fff;
    if (rest > 0x1000 || (rest == 0x1000 && (h & 1))) ++h;  // may carry into the exponent: right
    return uint16_t(sign | h);
}

float half_to_float(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f, mant = h & 0x3ff, x;
    if (exp == 0)
    {
        if (!mant)
            x = sign;
        else
        {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) mant <<= 1, --exp;
            x = sign | (exp << 23) | ((mant & 0x3ff) << 13);
        }
    }
    else if (exp == 31)
        x = sign | 0x7f800000 | (mant << 13);
    else
        x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

size_t sample_bytes(sample_type t)
{
    return t == sample_type::u8 ? 1 : t == sample_type::f32 ? 4 : 2;
}

// The source's sample type as the format's kind, where they're the same
// bits: copied as they are.
bool same_bits(sample_type t, format_info::kind k)
{
    using K = format_info::kind;
    return (t == sample_type::u8 && k == K::unorm8) || (t == sample_type::u16 && k == K::unorm16) ||
           (t == sample_type::i16 && k == K::snorm16) || (t == sample_type::f16 && k == K::half) ||
           (t == sample_type::f32 && k == K::float32);
}

float to_float(const std::byte* p, sample_type t)
{
    switch (t)
    {
        case sample_type::u8: return float(std::to_integer<uint8_t>(*p)) / 255.0f;
        case sample_type::u16:
        {
            uint16_t v;
            std::memcpy(&v, p, 2);
            return float(v) / 65535.0f;
        }
        case sample_type::i16:
        {
            int16_t v;
            std::memcpy(&v, p, 2);
            return std::max(-1.0f, float(v) / 32767.0f);
        }
        case sample_type::f16:
        {
            uint16_t v;
            std::memcpy(&v, p, 2);
            return half_to_float(v);
        }
        case sample_type::f32:
        {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
    }
    return 0;
}

void from_float(float v, format_info::kind k, std::byte* out)
{
    using K = format_info::kind;
    switch (k)
    {
        case K::unorm8: *out = std::byte(uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f))); return;
        case K::unorm16:
        {
            const uint16_t x = uint16_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f));
            std::memcpy(out, &x, 2);
            return;
        }
        case K::snorm16:
        {
            const int16_t x = int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
            std::memcpy(out, &x, 2);
            return;
        }
        case K::half:
        {
            const uint16_t x = float_to_half(v);
            std::memcpy(out, &x, 2);
            return;
        }
        case K::float32: std::memcpy(out, &v, 4); return;
    }
}

}  // namespace

void convert(std::span<const std::byte> pixels, const source_layout& layout, int w, int h, format target,
             std::byte* out, size_t out_stride)
{
    const format_info fi = info(target);
    const int c = std::clamp(layout.channels, 1, 4), n = fi.channels;
    const size_t in_sample = sample_bytes(layout.type), out_sample = fi.sample_bytes();
    const size_t in_pixel = in_sample * size_t(c), out_pixel = out_sample * size_t(n);
    const size_t in_stride = layout.stride ? layout.stride : in_pixel * size_t(w);
    const bool bits = same_bits(layout.type, fi.sample);

    // What a channel the source doesn't have holds: 0, and alpha 1.
    std::byte zero[4] = {}, one[4] = {};
    from_float(1.0f, fi.sample, one);

    for (int y = 0; y < h; ++y)
    {
        const std::byte* src = pixels.data() + size_t(y) * in_stride;
        std::byte* dst = out + size_t(y) * out_stride;
        if (bits && c == n)
        {
            std::memcpy(dst, src, out_pixel * size_t(w));
            continue;
        }
        for (int x = 0; x < w; ++x, src += in_pixel, dst += out_pixel)
            for (int k = 0; k < n; ++k)
            {
                std::byte* d = dst + size_t(k) * out_sample;
                if (k < c)
                {
                    if (bits)
                        std::memcpy(d, src + size_t(k) * in_sample, out_sample);
                    else
                        from_float(to_float(src + size_t(k) * in_sample, layout.type), fi.sample, d);
                }
                else
                    std::memcpy(d, k == 3 ? one : zero, out_sample);
            }
    }
}

std::shared_ptr<device> device::of(const rap::canvas& c)
{
    return c.gpu_device();
}

std::shared_ptr<device> device::headless(uint64_t adapter)
{
#ifdef _WIN32
    return make_d3d11_headless(adapter);
#elif defined(RAPPORT_GPU_VULKAN)
    return make_vulkan_headless(adapter);
#else
    (void)adapter;
    return nullptr;
#endif
}

}  // namespace rap::gpu
