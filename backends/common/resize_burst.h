// A canvas's size changing in a burst (canvas::stretch_while_resizing): the
// last frame it showed, kept by the backend as it paints, shown scaled at
// each new size until no change has come for settle_ms.
#pragma once

#include "rapport/backend.h"

#include <algorithm>
#include <functional>
#include <optional>

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
    bool pending = false;                 // a change came, not yet settled
    // canvas::stretch_to: a rect of the kept frame, and where it goes now.
    std::optional<std::pair<rect_f, rect_f>> mapping;

    // Where the kept frame (`kept` across) goes on the canvas (`now`
    // across): placed so the app's rect of it lands where the app said, or
    // else scaled whole as `how` says.
    rect_f where(size_f kept, size_f now) const
    {
        if (mapping && mapping->first.w > 0 && mapping->first.h > 0)
        {
            const auto& [from, to] = *mapping;
            const float sx = to.w / from.w, sy = to.h / from.h;
            return {to.x - from.x * sx, to.y - from.y * sy, kept.w * sx, kept.h * sy};
        }
        return stretched(kept, now, how);
    }

    // Whether the backend keeps each frame it shows, to stretch.
    bool keeps() const { return how != resize_stretch::none; }
    // A size change; the backend (re)starts its settle timer. A lone change
    // (an app fitting its window to what it shows) is painted at once; the
    // second within the settle time makes it a burst, stretched when asked
    // for and there's a frame to stretch.
    void resized(bool have_last)
    {
        if (pending && keeps() && have_last) stretching = true;
        pending = true;
    }
    // The settle timer: the burst is over. The backend asks for its new
    // frame, then calls what this returns last of all: the app may close
    // the window from it, the canvas going with it.
    std::function<void(size_f)> settle()
    {
        stretching = pending = false;
        mapping.reset();
        return settled;
    }
};

}  // namespace rap::frames
