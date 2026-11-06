// rap::gpu's insides, shared by the core's device implementations.
#pragma once

#include "rapport/gpu.h"

#include <cstddef>
#include <memory>
#include <span>

namespace rap::gpu
{

// A compiled pixel shader; each backend's derives from it.
class program
{
public:
    virtual ~program() = default;
};

#ifdef RAPPORT_TEST_READBACK
// Test seam for a staging map failure on the device worker.
void fail_next_readback_map();
bool readback_map_failure_requested();
#else
inline bool readback_map_failure_requested() { return false; }
#endif

struct format_info
{
    enum class kind
    {
        unorm8,
        unorm16,
        snorm16,
        half,
        float32,
        packed1010102,  // rgb10a2: four channels in one 32-bit word
    } sample;
    int channels;
    // Bytes a channel takes; the packed format's word counts as one 4-byte "channel".
    size_t sample_bytes() const { return sample == kind::unorm8 ? 1 : sample == kind::float32 || sample == kind::packed1010102 ? 4 : 2; }
};
format_info info(format);

// The final row needs no trailing padding. Avoid overflowing on a large stride.
inline bool rows_fit(size_t bytes, size_t row, size_t stride, int height)
{
    return height > 0 && row > 0 && stride >= row && bytes >= row &&
           size_t(height - 1) <= (bytes - row) / stride;
}
bool upload_fits(size_t bytes, const source_layout&, int width, int height);

// The app's pixels (laid out as `layout`, `w` × `h`) in `target`'s format,
// rows of `out_stride` bytes: same-typed samples copied bit for bit, others
// through float; channels the source lacks are 0, and alpha 1.
void convert(std::span<const std::byte> pixels, const source_layout& layout, int w, int h, format target,
             std::byte* out, size_t out_stride);

#ifdef _WIN32
std::shared_ptr<device> make_d3d11_headless(uint64_t adapter);
#elif defined(RAPPORT_GPU_VULKAN)
std::shared_ptr<device> make_vulkan_headless(uint64_t adapter);
#endif

}  // namespace rap::gpu
