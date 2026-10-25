// The frame a canvas shows under its painting (canvas::present), drawn by
// the backends that composite it with the painter: CPU pixels for now.
#pragma once

#include "rapport/backend.h"

#include <cmath>
#include <memory>

namespace rap::frames
{

// What a canvas `size` logical pixels across, drawn at `scale`, takes as
// CPU frames.
inline frame_target pixels_target(size_f size, float scale)
{
    frame_target t;
    t.width = int(std::lround(size.w * scale));
    t.height = int(std::lround(size.h * scale));
    t.scale = scale;
    return t;
}

struct frame_layer
{
    std::shared_ptr<const image> pixels;
    frame_color color;

    explicit operator bool() const { return bool(pixels); }

    // The canvas's background where the frame doesn't reach, then the frame,
    // one of its pixels to one of the painter's.
    void paint(painter& p, size_f size) const
    {
        if (!pixels) return;
        const float k = p.pixel_scale();
        const float w = float(pixels->width()) / k, h = float(pixels->height()) / k;
        const rap::paint background = theme_color::window;
        if (w < size.w) p.rect({w, 0, size.w - w, size.h}, 0, background, std::nullopt);
        if (h < size.h) p.rect({0, h, std::min(w, size.w), size.h - h}, 0, background, std::nullopt);
        p.image(*pixels, {0, 0, float(pixels->width()), float(pixels->height())}, {0, 0, w, h},
                filter::nearest);
    }
};

}  // namespace rap::frames
