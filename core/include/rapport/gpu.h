// rapport owns the GPU. The app brings shaders and
// data; rapport runs its passes on the device its canvases present on
// (Direct3D 11 on Windows, Vulkan on Linux, lavapipe without a GPU), so a
// result is shown without leaving the GPU.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rap
{
class canvas;
struct frame_color;  // widgets.h
}

namespace rap::gpu
{

// -- formats ------------------------------------------------------------------

// unorm samples as v / (2^n - 1) and snorm as max(v / (2^(n-1) - 1), -1),
// exactly for every value on both APIs (so a GL reference stays identical).
enum class format
{
    r8, rg8, rgba8,  // unorm
    rgba8_srgb,
    r16, rg16, rgba16,                    // unorm
    r16_snorm, rg16_snorm, rgba16_snorm,  // -32767..32767 -> -1..1
    r16f, rg16f, rgba16f,
    r32f, rg32f, rgba32f,
    // No 3-channel formats: no API renders to them. upload() takes 3-channel
    // sources and expands them (see source_layout).
};
size_t bytes_per_pixel(format);

// A rectangle of pixels.
struct rect
{
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

// What the app's pixels are, for upload(): rapport converts to the
// texture's format in its staging copy, so a raw's RGB16 goes up as it is.
enum class sample_type
{
    u8, u16, i16, f16, f32,
};
struct source_layout
{
    int channels = 4;  // 1..4; fewer than the texture's: the rest are 0, alpha 1
    sample_type type = sample_type::u8;
    size_t stride = 0;  // bytes per row; 0 = packed
};

// -- shaders ------------------------------------------------------------------

enum class shader_language
{
    dxbc,   // D3D11 bytecode (FXC, or D3DCompile): Direct3D 11 needs this, not DXIL
    spirv,  // Vulkan
    hlsl,   // source; compiled at run time where the backend can (Windows: D3DCompile)
};

// One shader, in as many forms as the app has: the device uses the first it
// takes (see device::takes). Build time is the expected way: rapport's CMake
// helper (rapport_add_shaders) compiles HLSL for the platform being built,
// with FXC on Windows (dxbc) and glslang elsewhere (spirv), and embeds it.
// Keep shaders to Shader Model 5, which both compilers agree on.
struct shader
{
    struct form
    {
        shader_language language;
        std::span<const std::byte> code;  // bytes (hlsl: UTF-8 source)
    };
    std::vector<form> forms;
    std::string entry = "main";
    std::string name;  // for errors and captures
};

// -- resources ----------------------------------------------------------------

// A texture on a device. Shared: a pass, a present and the app can all hold
// it; it's freed when the last goes (after the GPU's done with it).
class texture
{
public:
    virtual ~texture() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual gpu::format format() const = 0;
};
using texture_ptr = std::shared_ptr<texture>;

// A point on a device's timeline (a D3D11 fence, a Vulkan timeline
// semaphore): reached when the GPU has done what was queued before it.
// Backends implement it.
class timeline
{
public:
    virtual ~timeline() = default;
    virtual uint64_t completed() const = 0;
    virtual void wait(uint64_t value) const = 0;
};

// What the GPU has queued so far, to wait on or test. Cheap to copy; a
// default one is done already.
class ticket
{
public:
    ticket() = default;
    ticket(std::shared_ptr<const timeline> line, uint64_t value) : line_(std::move(line)), value_(value) {}
    bool done() const { return !line_ || line_->completed() >= value_; }  // finished on the GPU
    void wait() const                                                      // blocks: workers only
    {
        if (!done()) line_->wait(value_);
    }

private:
    std::shared_ptr<const timeline> line_;
    uint64_t value_ = 0;
};

// -- passes -------------------------------------------------------------------

enum class sampler
{
    nearest_clamp,
    linear_clamp,
    linear_mip_clamp,  // trilinear across the texture's levels (zoomed out)
};

// A fullscreen triangle over `target` (compute later): `pixel` reads
// `inputs` by slot (t0.. / binding 0..) with `samplers` alongside, and one
// uniform block (b0 / binding N) packed as std140 and cbuffer agree: 16-byte
// rows, no vec3 straddling them.
class program;  // a compiled pixel shader, from device::program
using program_ptr = std::shared_ptr<const program>;

// In flight, a pass holds its program and textures; run() copies uniforms.
struct pass
{
    program_ptr program;
    std::vector<texture_ptr> inputs;
    std::vector<sampler> samplers;  // one per input; nearest_clamp when short
    std::span<const std::byte> uniforms;
    texture_ptr target;
    int target_level = 0;  // the mip level drawn into (the app makes its levels itself)
    rect region;           // only part of the target (a tile), in that level's pixels; empty = all
};

// -- device -------------------------------------------------------------------

struct memory_budget
{
    uint64_t budget = 0;  // what the OS lets this process use now
    uint64_t used = 0;
};

// One per GPU. Thread-safe: any thread creates resources and submits; rapport
// serialises submission on the device's queue. Workers do the heavy work;
// the UI thread only presents.
class device
{
public:
    virtual ~device() = default;

    // The device a canvas presents on (null until it has one: watch
    // canvas::on_frame_target), or one for headless work (--process,
    // thumbnails), on the given adapter or the default.
    static std::shared_ptr<device> of(const rap::canvas&);
    static std::shared_ptr<device> headless(uint64_t adapter = 0);

    virtual std::string_view api() const = 0;  // "d3d11", "vulkan"
    virtual bool software() const = 0;          // lavapipe / WARP
    virtual bool takes(shader_language) const = 0;
    virtual memory_budget memory() const = 0;
    virtual int max_texture_size() const = 0;

    // Textures. `render_target`: passes can draw into it. Null when it
    // can't be made (out of memory, past max_texture_size()): the app falls
    // back, or tiles.
    struct texture_desc
    {
        int width = 0, height = 0;
        gpu::format format = format::rgba8;
        int levels = 1;  // mip levels, each half the last
        bool render_target = false;
    };
    virtual texture_ptr create(const texture_desc&) = 0;
    // Copies `pixels` (laid out as `layout`) into staging, converting to the
    // texture's format, before it returns: the caller's buffer is free at
    // once. The GPU side is done by the ticket.
    virtual ticket upload(const texture_ptr&, std::span<const std::byte> pixels, const source_layout&,
                          int level = 0, rect region = {}) = 0;
    // Fills `out` (the texture's format, rows of `stride` bytes) by the time
    // the ticket is done; `out` must outlive that. `done`, if given, runs
    // then on a rapport thread.
    virtual ticket readback(const texture_ptr&, std::span<std::byte> out, size_t stride, int level = 0,
                            std::function<void()> done = {}) = 0;

    // A program: a pixel shader (the vertex stage is rapport's fullscreen
    // triangle). An error says which form failed and why.
    struct program_result
    {
        program_ptr program;
        std::string error;
    };
    virtual program_result program(const shader& pixel) = 0;

    // Queues passes in order; the ticket covers them all.
    virtual ticket run(std::span<const pass>) = 0;

    // The device went away (driver reset, eGPU unplugged): every texture and
    // program is dead, and the app makes a new device and starts over.
    virtual bool lost() const = 0;
};

}  // namespace rap::gpu

