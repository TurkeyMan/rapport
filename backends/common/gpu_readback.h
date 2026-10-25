// canvas::present(texture) by reading the texture back: for backends that
// can't yet take a device's textures as they are (GTK 3 always; GTK 4 and
// Qt until their zero-copy paths). The device reads it back on its own
// thread; the pixels then go to the canvas as a CPU frame on the UI thread,
// unless something newer was presented meanwhile. 8-bit textures only:
// HDR needs the direct path.
#pragma once

#include "rapport/backend.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace rap::frames
{

// The process's device for canvases without one of their own: made when
// first asked for.
inline std::shared_ptr<gpu::device> shared_gpu_device()
{
    static std::shared_ptr<gpu::device> d = gpu::device::headless();
    return d;
}

class gpu_readback
{
public:
    using post_fn = std::function<void(std::function<void()>)>;  // runs it on the UI thread
    using make_fn = std::function<std::shared_ptr<const image>(int w, int h, pixel_format, const uint8_t*, size_t stride)>;
    using show_fn = std::function<void(std::shared_ptr<const image>, frame_color)>;

    ~gpu_readback() { cancel(); }

    // Anything read back from now on is stale (a CPU frame, clear_frame, the
    // canvas going).
    void cancel() { ++*serial_; }

    void present(const gpu::texture_ptr& t, frame_color color, post_fn post, make_fn make, show_fn show)
    {
        const std::shared_ptr<gpu::device> dev = shared_gpu_device();
        if (!dev || !t || (t->format() != gpu::format::rgba8 && t->format() != gpu::format::rgba8_srgb))
        {
            std::fprintf(stderr, "rapport: canvas::present: not a texture this canvas can show "
                                 "(the canvas device's, rgba8)\n");
            return;
        }
        const uint64_t mine = ++*serial_;
        const int w = t->width(), h = t->height();
        auto pixels = std::make_shared<std::vector<std::byte>>(size_t(w) * size_t(h) * 4);
        dev->readback(t, *pixels, size_t(w) * 4, 0,
                      [serial = serial_, mine, pixels, w, h, color, post = std::move(post), make = std::move(make),
                       show = std::move(show)]
                      {
                          if (*serial != mine) return;  // superseded already
                          // RGBA to the toolkits' BGRA where it's premultiplied.
                          pixel_format format = pixel_format::rgba8;
                          if (color.premultiplied)
                          {
                              format = pixel_format::bgra8_premultiplied;
                              for (size_t i = 0; i < pixels->size(); i += 4) std::swap((*pixels)[i], (*pixels)[i + 2]);
                          }
                          post([serial, mine, pixels, w, h, format, color, make, show]
                               {
                                   if (*serial != mine) return;
                                   show(make(w, h, format, reinterpret_cast<const uint8_t*>(pixels->data()), size_t(w) * 4),
                                        color);
                               });
                      });
    }

private:
    std::shared_ptr<std::atomic<uint64_t>> serial_ = std::make_shared<std::atomic<uint64_t>>(0);
};

}  // namespace rap::frames
