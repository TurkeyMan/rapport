// The last frame a cairo canvas showed (GTK 3's and GTK 4's), kept as pixels
// to show scaled while its size changes in a burst (resize_burst.h): drawn
// into an image surface of its own and from there onto the widget's.
#pragma once

#include "rapport/backend.h"

#include <cairo.h>

#include <cmath>
#include <functional>

namespace rap::gtkcommon
{

class kept_frame
{
public:
    kept_frame() = default;
    ~kept_frame() { reset(); }
    kept_frame(const kept_frame&) = delete;
    kept_frame& operator=(const kept_frame&) = delete;

    explicit operator bool() const { return surface_ != nullptr; }
    size_f size() const { return size_; }
    void reset()
    {
        if (surface_) cairo_surface_destroy(surface_);
        surface_ = nullptr;
    }

    // `draw` (onto the cairo_t it's given, `size` across at `scale`) kept,
    // and shown on `cr` at its top left as if drawn there.
    void draw(cairo_t* cr, size_f size, float scale, const std::function<void(cairo_t*)>& draw)
    {
        const int w = int(std::ceil(size.w * scale)), h = int(std::ceil(size.h * scale));
        if (w <= 0 || h <= 0) return;
        if (!surface_ || w != width_ || h != height_)
        {
            reset();
            surface_ = cairo_surface_create_similar_image(cairo_get_target(cr), CAIRO_FORMAT_ARGB32, w, h);
            width_ = w, height_ = h;
        }
        cairo_surface_set_device_scale(surface_, scale, scale);
        cairo_t* off = cairo_create(surface_);
        cairo_set_operator(off, CAIRO_OPERATOR_CLEAR);
        cairo_paint(off);
        cairo_set_operator(off, CAIRO_OPERATOR_OVER);
        draw(off);
        cairo_destroy(off);
        cairo_surface_flush(surface_);
        size_ = size;
        show(cr, {0, 0, size.w, size.h});
    }

    // Scaled into `to`, bilinear.
    void show(cairo_t* cr, rect_f to) const
    {
        if (!surface_ || size_.w <= 0 || size_.h <= 0) return;
        cairo_save(cr);
        cairo_translate(cr, to.x, to.y);
        cairo_scale(cr, to.w / size_.w, to.h / size_.h);
        cairo_set_source_surface(cr, surface_, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);  // no seam at the edges
        cairo_rectangle(cr, 0, 0, size_.w, size_.h);
        cairo_fill(cr);
        cairo_restore(cr);
    }

private:
    cairo_surface_t* surface_ = nullptr;
    int width_ = 0, height_ = 0;
    size_f size_;
};

}  // namespace rap::gtkcommon
