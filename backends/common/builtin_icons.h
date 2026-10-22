// Drawn icons for common freedesktop names, for backends whose platform has
// no icon theme (Windows) or whose theme may lack them (Qt off KDE). Each is
// a 16-unit design scaled to the box, in the window_text ink, which the
// caller's painter can override.
#pragma once

#include "rapport/backend.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace rap::builtin
{

// Draws `full_name` (a trailing "-symbolic" ignored) into `s`; false when it
// isn't one of these.
inline bool draw_builtin_icon(const std::string& full_name, painter& p, size_f s)
{
    std::string name = full_name;
    if (const size_t k = name.rfind("-symbolic"); k != std::string::npos && k + 9 == name.size()) name.resize(k);
    const paint ink = theme_color::window_text;
    const float u = std::min(s.w, s.h) / 16.f;
    auto pt = [u](float x, float y) { return point_f{x * u, y * u}; };
    const float w = 1.5f * u;
    auto ln = [&](float x0, float y0, float x1, float y1) { p.line(pt(x0, y0), pt(x1, y1), ink, w, line_cap::round); };
    auto box = [&](float x, float y, float bw, float bh, float r = 0) { p.rect({x * u, y * u, bw * u, bh * u}, r * u, std::nullopt, ink); };
    if (name == "go-previous") { ln(10, 3, 5, 8); ln(5, 8, 10, 13); }
    else if (name == "go-next") { ln(6, 3, 11, 8); ln(11, 8, 6, 13); }
    else if (name == "go-up") { ln(3, 10, 8, 5); ln(8, 5, 13, 10); }
    else if (name == "go-down") { ln(3, 6, 8, 11); ln(8, 11, 13, 6); }
    else if (name == "zoom-in" || name == "zoom-out")
    {
        p.circle(pt(6.5f, 6.5f), 4.5f * u, std::nullopt, ink);
        ln(10, 10, 14, 14);
        ln(4.5f, 6.5f, 8.5f, 6.5f);
        if (name == "zoom-in") ln(6.5f, 4.5f, 6.5f, 8.5f);
    }
    else if (name == "zoom-original")
    {
        box(2, 3, 12, 10, 1.5f);
        ln(6, 6, 7, 5.5f); ln(7, 5.5f, 7, 10.5f); ln(10, 6, 11, 5.5f); ln(11, 5.5f, 11, 10.5f);
        p.circle(pt(8.5f, 7), 0.6f * u, ink, std::nullopt);
        p.circle(pt(8.5f, 9.5f), 0.6f * u, ink, std::nullopt);
    }
    else if (name == "zoom-fit-best" || name == "view-fullscreen")
    {
        const bool out = name == "view-fullscreen";
        const float a = out ? 2 : 4, b = out ? 6 : 6.5f;
        ln(a, a, b, a); ln(a, a, a, b);
        ln(16 - a, a, 16 - b, a); ln(16 - a, a, 16 - a, b);
        ln(a, 16 - a, b, 16 - a); ln(a, 16 - a, a, 16 - b);
        ln(16 - a, 16 - a, 16 - b, 16 - a); ln(16 - a, 16 - a, 16 - a, 16 - b);
    }
    else if (name == "media-playback-start") p.polygon({pt(5, 3), pt(13, 8), pt(5, 13)}, ink, std::nullopt);
    else if (name == "media-playback-pause") { p.rect({4 * u, 3 * u, 3 * u, 10 * u}, 0, ink, std::nullopt); p.rect({9 * u, 3 * u, 3 * u, 10 * u}, 0, ink, std::nullopt); }
    else if (name == "document-open")
    {
        p.polygon({pt(1.5f, 4), pt(6, 4), pt(7.5f, 5.5f), pt(14.5f, 5.5f), pt(14.5f, 13), pt(1.5f, 13)}, std::nullopt, ink, w);
        ln(1.5f, 8, 14.5f, 8);
    }
    else if (name == "media-floppy" || name == "document-save")
    {
        p.polygon({pt(2, 2), pt(12, 2), pt(14, 4), pt(14, 14), pt(2, 14)}, std::nullopt, ink, w);
        box(5, 2, 6, 4);
        box(4.5f, 9, 7, 5);
    }
    else if (name == "edit-cut")
    {
        p.circle(pt(4.5f, 11.5f), 2.2f * u, std::nullopt, ink);
        p.circle(pt(11.5f, 11.5f), 2.2f * u, std::nullopt, ink);
        ln(6, 10, 12, 2); ln(10, 10, 4, 2);
    }
    else if (name == "edit-copy") { box(2, 5, 8, 9, 1); box(6, 2, 8, 9, 1); }
    else if (name == "edit-paste") { box(3, 3, 10, 11, 1); box(6, 1.5f, 4, 3, 1); }
    else if (name == "view-grid")
    {
        p.rect({2 * u, 2 * u, 5 * u, 5 * u}, u, ink, std::nullopt);
        p.rect({9 * u, 2 * u, 5 * u, 5 * u}, u, ink, std::nullopt);
        p.rect({2 * u, 9 * u, 5 * u, 5 * u}, u, ink, std::nullopt);
        p.rect({9 * u, 9 * u, 5 * u, 5 * u}, u, ink, std::nullopt);
    }
    else if (name == "document-edit")
    {
        p.polygon({pt(11, 2), pt(14, 5), pt(6, 13), pt(2.5f, 13.5f), pt(3, 10)}, std::nullopt, ink, w);
        ln(9.5f, 3.5f, 12.5f, 6.5f);
    }
    else if (name == "dialog-information")
    {
        p.circle(pt(8, 8), 6.5f * u, std::nullopt, ink);
        ln(8, 7, 8, 11.5f);
        p.circle(pt(8, 4.8f), 0.9f * u, ink, std::nullopt);
    }
    else if (name == "preferences-system")
    {
        for (int k = 0; k < 8; ++k)
        {
            const float a = k * 3.14159265f / 4;
            ln(8 + 4.5f * std::cos(a), 8 + 4.5f * std::sin(a), 8 + 6.5f * std::cos(a), 8 + 6.5f * std::sin(a));
        }
        p.circle(pt(8, 8), 4.5f * u, std::nullopt, ink);
        p.circle(pt(8, 8), 1.8f * u, std::nullopt, ink);
    }
    else if (name == "edit-find" || name == "system-search")
    {
        p.circle(pt(6.5f, 6.5f), 4.5f * u, std::nullopt, ink);
        ln(10, 10, 14, 14);
    }
    else if (name == "view-refresh")
    {
        p.arc(pt(8, 8), 5.5f * u, 0.3f, 5.4f, ink, w, line_cap::round);
        p.polygon({pt(14.5f, 6.5f), pt(13.5f, 10.5f), pt(10.5f, 8)}, ink, std::nullopt);
    }
    else if (name == "edit-delete" || name == "window-close") { ln(4, 4, 12, 12); ln(12, 4, 4, 12); }
    else if (name == "list-add") { ln(8, 3, 8, 13); ln(3, 8, 13, 8); }
    else if (name == "list-remove") ln(3, 8, 13, 8);
    else
        return false;
    return true;
}

}  // namespace rap::builtin
