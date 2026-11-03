// A canvas's size changing in a burst (canvas::stretch_while_resizing): the
// last frame it showed, kept by the backend as it paints, shown scaled at
// each new size until no change has come for settle_ms.
#pragma once

#include "rapport/backend.h"

#include <algorithm>
#include <functional>

namespace rap::frames
{

inline constexpr int settle_ms = 150;

// Where a frame `from` across goes on a canvas `to` across (logical pixels).
inline rect_f stretched(size_f from, size_f to, resize_stretch how)
{
    if (how != resize_stretch::uniform || from.w <= 0 || from.h <= 0) return {0, 0, to.w, to.h};
    const float k = std::min(to.w / from.w, to.h / from.h);
    const float w = from.w * k, h = from.h * k;
    return {(to.w - w) / 2, (to.h - h) / 2, w, h};
}

struct resize_burst
{
    resize_stretch how = resize_stretch::none;
    paint background = theme_color::window;
    std::function<void(size_f)> settled;  // canvas::on_resize_settled
    bool stretching = false;              // showing the last frame scaled

    // Whether the backend keeps each frame it shows, to stretch.
    bool keeps() const { return how != resize_stretch::none; }
    // A size change; the backend (re)starts its settle timer. Stretched when
    // asked for and there's a frame to stretch.
    void resized(bool have_last)
    {
        if (keeps() && have_last) stretching = true;
    }
    // The settle timer: the burst is over. The backend asks for its new
    // frame, then calls what this returns last of all: the app may close
    // the window from it, the canvas going with it.
    std::function<void(size_f)> settle()
    {
        stretching = false;
        return settled;
    }
};

}  // namespace rap::frames
