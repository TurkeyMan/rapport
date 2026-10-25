// rapport-gpu: exercises rap::gpu.
//
// --check [file]: no window. A headless device uploads a 3-channel 16-bit
// image (expanded to rgba16 on the way), a pass scales it into an rgba8
// target, another draws into the second mip level of a texture, and both
// are read back and compared with the same arithmetic on the CPU. Prints
// what it found (also to `file`: a Windows GUI program's stdout often goes
// nowhere); exit status 0 when everything matched.
//
// Otherwise a window: the canvas's own device renders a frame at its
// target's exact size (a one-pixel checker top left, a one-pixel red border,
// a gradient) and presents the texture under the painted overlay. Esc quits.
//
// The shaders are HLSL (shaders/), compiled for the platform at build time
// by rapport_add_shader.
#include "rapport/rapport.h"
#include "shaders/count.h"
#include "shaders/fill.h"
#include "shaders/show_count.h"
#include "shaders/pattern.h"
#include "shaders/scale.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace rap;

namespace
{




template <class T>
std::span<const std::byte> bytes_of(const T& v)
{
    return std::as_bytes(std::span(&v, 1));
}

int run_check(std::FILE* log)
{
    auto say = [&](const char* fmt, auto... args)
    {
        std::printf(fmt, args...);
        if (log) std::fprintf(log, fmt, args...);
    };
    auto dev = gpu::device::headless();
    if (!dev)
    {
        say("no headless device\n");
        return 1;
    }
    const auto mem = dev->memory();
    say("device: %s%s, budget %llu MB, used %llu MB, max texture %d\n", std::string(dev->api()).c_str(),
        dev->software() ? " (software)" : "", (unsigned long long)(mem.budget >> 20), (unsigned long long)(mem.used >> 20),
        dev->max_texture_size());

    // A 3-channel 16-bit source: every value somewhere, odd sizes.
    const int w = 257, h = 131;
    std::vector<uint16_t> rgb(size_t(w) * h * 3);
    for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = uint16_t((i * 2654435761u) >> 16);
    const auto t0 = std::chrono::steady_clock::now();
    auto src = dev->create({w, h, gpu::format::rgba16});
    auto dst = dev->create({w, h, gpu::format::rgba8, 1, true});
    auto scale = dev->program(shaders::scale());
    auto fill = dev->program(shaders::fill());
    if (!src || !dst || !scale.program || !fill.program)
    {
        say("setup failed: %s%s\n", scale.error.c_str(), fill.error.c_str());
        return 1;
    }
    dev->upload(src, std::as_bytes(std::span(rgb)), {3, gpu::sample_type::u16});
    struct
    {
        float gain = 1.75f, pad[3] = {};
    } p1;
    gpu::pass pass;
    pass.program = scale.program;
    pass.inputs = {src};
    pass.uniforms = bytes_of(p1);
    pass.target = dst;
    dev->run(std::span(&pass, 1));
    std::vector<std::byte> out(size_t(w) * h * 4);
    dev->readback(dst, out, size_t(w) * 4).wait();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    int worst = 0;
    size_t off = 0;
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (int c = 0; c < 4; ++c)
        {
            const float v = c < 3 ? std::min(1.0f, rgb[i * 3 + size_t(c)] / 65535.0f * p1.gain) : 1.0f;
            const int want = int(std::lround(v * 255.0f));
            const int got = std::to_integer<int>(out[i * 4 + size_t(c)]);
            if (std::abs(got - want) > worst) worst = std::abs(got - want);
            if (got != want) ++off;
        }
    say("upload rgb16 -> rgba16, scale pass -> rgba8, readback: %.1f ms; largest difference %d, %zu of %zu values differ\n",
        ms, worst, off, size_t(w) * h * 4);

    // The second level of a three-level texture, drawn and read alone.
    auto mips = dev->create({64, 48, gpu::format::rgba8, 3, true});
    struct
    {
        float colour[4] = {64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f};
    } p2;
    gpu::pass level;
    level.program = fill.program;
    level.uniforms = bytes_of(p2);
    level.target = mips;
    level.target_level = 1;
    dev->run(std::span(&level, 1));
    std::vector<std::byte> lv(32 * 24 * 4);
    dev->readback(mips, lv, 32 * 4, 1).wait();
    bool level_ok = true;
    for (size_t i = 0; i < lv.size(); i += 4)
        level_ok &= std::to_integer<int>(lv[i]) == 64 && std::to_integer<int>(lv[i + 1]) == 128 &&
                    std::to_integer<int>(lv[i + 2]) == 191 && std::to_integer<int>(lv[i + 3]) == 255;
    say("mip level 1 (32x24): %s (first pixel %d %d %d %d)\n", level_ok ? "as drawn" : "WRONG",
        std::to_integer<int>(lv[0]), std::to_integer<int>(lv[1]), std::to_integer<int>(lv[2]), std::to_integer<int>(lv[3]));

    // Compute: a histogram of an 8-bit texture's red, cleared and dispatched
    // twice in one run (so the clear -> dispatch and dispatch -> dispatch
    // ordering is exercised), then a pass reading the buffer.
    bool compute_ok = false;
    {
        const int cw = 300, ch = 200;  // not a multiple of the group size
        std::vector<uint8_t> rgba(size_t(cw) * ch * 4);
        std::vector<uint32_t> expect(256);
        for (size_t i = 0; i < size_t(cw) * ch; ++i)
        {
            rgba[i * 4] = uint8_t((i * 7919) >> 3);
            rgba[i * 4 + 3] = 255;
            expect[rgba[i * 4]] += 2;
        }
        auto tex = dev->create({cw, ch, gpu::format::rgba8});
        auto bins = dev->create_buffer(256 * 4);
        auto count = dev->compute_program(shaders::count());
        auto show = dev->program(shaders::show_count());
        auto row = dev->create({256, 1, gpu::format::rgba8, 1, true});
        if (tex && bins && count.program && show.program && row)
        {
            dev->upload(tex, std::as_bytes(std::span(rgba)), {4, gpu::sample_type::u8});
            dev->clear(bins, 0);
            struct
            {
                uint32_t w, h, pad[2] = {};
            } cu{uint32_t(cw), uint32_t(ch)};
            gpu::dispatch d;
            d.program = count.program;
            d.inputs = {tex};
            d.buffers = {bins};
            d.uniforms = bytes_of(cu);
            d.groups[0] = (cw + 15) / 16;
            d.groups[1] = (ch + 15) / 16;
            const gpu::dispatch twice[] = {d, d};
            dev->run(std::span(twice));
            gpu::pass p;
            p.program = show.program;
            p.buffers = {bins};
            p.target = row;
            dev->run(std::span(&p, 1));
            std::vector<std::byte> counts(256 * 4), shown(256 * 4);
            dev->readback(bins, counts).wait();
            dev->readback(row, shown, 256 * 4).wait();
            int wrong = 0, shown_wrong = 0;
            for (int b = 0; b < 256; ++b)
            {
                uint32_t got;
                std::memcpy(&got, counts.data() + b * 4, 4);
                wrong += got != expect[size_t(b)];
                const int want = int(std::lround(std::min(expect[size_t(b)], 1024u) / 1024.0 * 255));
                shown_wrong += std::abs(std::to_integer<int>(shown[size_t(b) * 4]) - want) > 1;
            }
            say("compute: histogram of %dx%d, cleared and dispatched twice: %d of 256 bins wrong; a pass reading it: %d wrong\n",
                cw, ch, wrong, shown_wrong);
            compute_ok = wrong == 0 && shown_wrong == 0;
        }
        else
            say("compute: setup failed: %s%s\n", count.error.c_str(), show.error.c_str());
    }
    // A buffer's words uploaded into its middle: the rest stays as cleared.
    bool upload_ok = false;
    if (auto words = dev->create_buffer(64 * 4))
    {
        std::vector<uint32_t> in(32);
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = uint32_t(0x9E3779B9u * (i + 1));
        dev->clear(words, 0xAAAAAAAAu);
        dev->upload(words, std::as_bytes(std::span(in)), 16 * 4);
        std::vector<std::byte> out(64 * 4);
        dev->readback(words, out).wait();
        int wrong = 0;
        for (size_t i = 0; i < 64; ++i)
        {
            uint32_t got;
            std::memcpy(&got, out.data() + i * 4, 4);
            wrong += got != (i >= 16 && i < 48 ? in[i - 16] : 0xAAAAAAAAu);
        }
        say("buffer upload: 32 words at word 16 of 64: %d of 64 wrong\n", wrong);
        upload_ok = wrong == 0;
    }
    return worst <= 1 && level_ok && compute_ok && upload_ok ? 0 : 2;  // one step: a driver's float rounding (see gpu.h)
}

}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::string(argv[1]) == "--check")
    {
        std::FILE* log = argc > 2 ? std::fopen(argv[2], "w") : nullptr;
        const int rc = run_check(log);
        if (log) std::fclose(log);
        return rc;
    }

    auto ui = ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "%s\n", ui.error().c_str());
        return 1;
    }
    rap::canvas* canvas = nullptr;
    std::shared_ptr<gpu::device> dev;
    gpu::program_ptr pattern;
    std::string status = "waiting for the canvas's device";

    auto render = [&]
    {
        const frame_target t = canvas->target();
        if (t.width <= 0 || t.height <= 0) return;
        if (!dev) dev = gpu::device::of(*canvas);
        if (!dev)
        {
            canvas->invalidate();
            return;
        }
        if (!pattern)
        {
            auto r = dev->program(shaders::pattern());
            pattern = r.program;
            if (!pattern)
            {
                status = "pattern: " + r.error;
                canvas->invalidate();
                return;
            }
        }
        auto target = dev->create({t.width, t.height, gpu::format::rgba8, 1, true});
        struct
        {
            float size[2], pad[2] = {};
        } u{{float(t.width), float(t.height)}};
        gpu::pass p;
        p.program = pattern;
        p.uniforms = bytes_of(u);
        p.target = target;
        const gpu::ticket ready = dev->run(std::span(&p, 1));
        canvas->present(target, ready);
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s%s: %dx%d at %.3g", std::string(dev->api()).c_str(),
                      dev->software() ? " (software)" : "", t.width, t.height, double(t.scale));
        status = buf;
    };

    using namespace rap::layout;
    auto win = (*ui)->create(layout::window{
        "rapport gpu",
        {vbox{
            layout::canvas{}
                .on_paint(
                    [&](painter& p, size_f)
                    {
                        if (!dev) render();  // the canvas's device may have come since: it redraws when it does
                        p.rect({140, 20, 420, 28}, 4, color{0, 0, 0, 160}, std::nullopt);
                        p.text({150, 26}, status, {.fg = color{255, 255, 255}});
                    })
                .on_key(
                    [&](const key_event& e)
                    {
                        if (e.press && e.key == key::escape) return (*ui)->quit(), true;
                        return false;
                    })
                .bind(canvas)
                .stretch(),
        }},
    });
    win->resize({720, 480});
    canvas->on_frame_target([&](const frame_target&) { render(); });
    win->show();
    canvas->focus();
    render();
    return (*ui)->run();
}
