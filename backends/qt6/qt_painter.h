// The Qt 6 backend's painting, shared by canvases and painted list cells:
// a Painter over a QPainter, theme colours from the widget's palette, and
// images as QImages.
#pragma once

#include "rapport/backend.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPolygonF>
#include <QWidget>

#include <algorithm>
#include <cstring>
#include <numbers>
#include <utility>
#include <vector>

namespace rap::qt6
{

inline QColor qcolor(color c)
{
    return QColor(c.r, c.g, c.b, c.a);
}

inline color from_q_color(const QColor& c)
{
    return {uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), uint8_t(c.alpha())};
}

// A theme role from `w`'s palette (the application's without one). Roles a
// palette lacks are derived as the GTK backends derive them, from the text
// colour and whether the theme is dark.
inline color theme_color_of(const QWidget* w, theme_color role)
{
    const QPalette pal = w ? w->palette() : QApplication::palette();
    const QColor text = pal.color(QPalette::Text);
    const bool dark = text.lightnessF() > 0.5;
    auto faded = [&](double a)
    {
        QColor c = text;
        c.setAlphaF(float(a));
        return from_q_color(c);
    };
    switch (role)
    {
        case theme_color::window: return from_q_color(pal.color(QPalette::Window));
        case theme_color::window_text: return from_q_color(pal.color(QPalette::WindowText));
        case theme_color::base: return from_q_color(pal.color(QPalette::Base));
        case theme_color::alternate_base: return from_q_color(pal.color(QPalette::AlternateBase));
        case theme_color::text: return from_q_color(text);
        case theme_color::placeholder_text: return from_q_color(pal.color(QPalette::PlaceholderText));
        case theme_color::highlight: return from_q_color(pal.color(QPalette::Highlight));
        case theme_color::highlight_text: return from_q_color(pal.color(QPalette::HighlightedText));
        case theme_color::mid: return faded(0.35);
        case theme_color::accent: return from_q_color(pal.color(QPalette::Accent));
        case theme_color::link: return from_q_color(pal.color(QPalette::Link));
        case theme_color::positive: return dark ? color{0x8f, 0xf0, 0xa4} : color{0x26, 0xa2, 0x69};
        case theme_color::negative: return dark ? color{0xf6, 0x61, 0x51} : color{0xc0, 0x1c, 0x28};
        case theme_color::neutral: return faded(0.6);
    }
    return from_q_color(text);
}

inline color resolve(const QWidget* w, const paint& p)
{
    if (auto* c = std::get_if<color>(&p)) return *c;
    return theme_color_of(w, std::get<theme_color>(p));
}

// Pixels uploaded once and drawn many times. Premultiplied ARGB32 is
// rapport's BGRA byte order on little-endian machines, so most uploads are a
// straight copy.
class qt_image : public image
{
public:
    qt_image(int w, int h, pixel_format format, const uint8_t* pixels, size_t stride)
        : image_(std::max(1, w), std::max(1, h), QImage::Format_ARGB32_Premultiplied)
        , format_(format)
    {
        update(pixels, stride);
    }
    int width() const override { return image_.width(); }
    int height() const override { return image_.height(); }
    void update(const uint8_t* pixels, size_t stride) override
    {
        if (!pixels) return;
        const int w = image_.width();
        for (int y = 0; y < image_.height(); ++y)
        {
            const uint8_t* src = pixels + size_t(y) * stride;
            uint8_t* dst = image_.scanLine(y);
            if (format_ == pixel_format::bgra8_premultiplied)
            {
                std::memcpy(dst, src, size_t(w) * 4);
                continue;
            }
            for (int x = 0; x < w; ++x)  // straight RGBA to premultiplied BGRA
            {
                const uint8_t r = src[x * 4], g = src[x * 4 + 1], b = src[x * 4 + 2], a = src[x * 4 + 3];
                dst[x * 4] = uint8_t((b * a + 127) / 255);
                dst[x * 4 + 1] = uint8_t((g * a + 127) / 255);
                dst[x * 4 + 2] = uint8_t((r * a + 127) / 255);
                dst[x * 4 + 3] = a;
            }
        }
    }
    const QImage& image() const { return image_; }

private:
    QImage image_;
    pixel_format format_;
};

// A Painter over a QPainter that's already begun on `widget` (or on an
// item view's viewport, for list cells). Coordinates are logical pixels;
// Qt applies the device pixel ratio.
class qt_painter : public painter
{
public:
    qt_painter(QPainter& p, const QWidget* widget, std::optional<paint> default_text = {})
        : p_(p)
        , w_(widget)
        , default_text_(default_text ? *default_text : paint{theme_color::text})
    {
        p_.setRenderHint(QPainter::Antialiasing, true);
        p_.setRenderHint(QPainter::TextAntialiasing, true);
    }

    color resolve(const paint& p) const override
    {
        if (auto* role = std::get_if<theme_color>(&p))
            for (auto& [r, c] : overrides_)
                if (r == *role) return c;
        return qt6::resolve(w_, p);
    }

    // Replaces a theme colour for this painter (a list's soft selection).
    void override_color(theme_color role, color c) { overrides_.push_back({role, c}); }

    void line(point_f a, point_f b, const paint& paint, float width, line_cap cap) override
    {
        p_.setPen(pen(paint, width, cap));
        p_.setBrush(Qt::NoBrush);
        p_.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
    }

    void curve(point_f a, point_f c1, point_f c2, point_f b, const paint& paint, float width) override
    {
        QPainterPath path(QPointF(a.x, a.y));
        path.cubicTo(QPointF(c1.x, c1.y), QPointF(c2.x, c2.y), QPointF(b.x, b.y));
        p_.strokePath(path, pen(paint, width, line_cap::butt));
    }

    void circle(point_f c, float r, std::optional<paint> fill, std::optional<paint> stroke) override
    {
        use(fill, stroke, 1);
        p_.drawEllipse(QPointF(c.x, c.y), r, r);
    }

    void rect(rect_f r, float radius, std::optional<paint> fill, std::optional<paint> stroke) override
    {
        use(fill, stroke, 1);
        const QRectF q(r.x, r.y, r.w, r.h);
        if (radius > 0)
            p_.drawRoundedRect(q, radius, radius);
        else if (fill && !stroke)
            p_.fillRect(q, qcolor(resolve(*fill)));  // no antialiased seams between abutting boxes
        else
            p_.drawRect(q);
    }

    void polygon(const std::vector<point_f>& pts, std::optional<paint> fill, std::optional<paint> stroke,
                 float width) override
    {
        if (pts.empty()) return;
        QPolygonF poly;
        poly.reserve(qsizetype(pts.size()));
        for (point_f pt : pts) poly << QPointF(pt.x, pt.y);
        use(fill, stroke, width);
        p_.drawPolygon(poly);
    }

    // rapport's angles are radians clockwise from 3 o'clock; Qt's arcs are in
    // degrees anticlockwise.
    void arc(point_f c, float r, float start, float end, const paint& paint, float width, line_cap cap) override
    {
        const QRectF box(c.x - r, c.y - r, 2 * r, 2 * r);
        const double to_deg = 180 / std::numbers::pi;
        QPainterPath path;
        path.arcMoveTo(box, -start * to_deg);
        path.arcTo(box, -start * to_deg, -(end - start) * to_deg);
        p_.strokePath(path, pen(paint, width, cap));
    }

    void image(const rap::image& img, rect_f src, rect_f dst, filter filter, float alpha) override
    {
        if (src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0) return;
        p_.save();
        p_.setRenderHint(QPainter::SmoothPixmapTransform, filter == filter::linear);
        p_.setOpacity(p_.opacity() * std::clamp(alpha, 0.f, 1.f));
        p_.drawImage(QRectF(dst.x, dst.y, dst.w, dst.h), static_cast<const qt_image&>(img).image(),
                     QRectF(src.x, src.y, src.w, src.h));
        p_.restore();
    }

    float device_scale() const override { return w_ ? float(w_->devicePixelRatioF()) : 1.f; }

    void push_clip(rect_f r) override
    {
        p_.save();
        p_.setClipRect(QRectF(r.x, r.y, r.w, r.h), Qt::IntersectClip);
    }
    void pop_clip() override { p_.restore(); }

    void text(point_f at, const std::string& s, const text_style& style) override
    {
        const QString str = QString::fromUtf8(s.data(), qsizetype(s.size()));
        const QFont f = font(style);
        const QRectF box = bounds(str, f, at);
        if (style.bg) p_.fillRect(box, qcolor(resolve(*style.bg)));
        p_.setFont(f);
        p_.setPen(qcolor(resolve(style.fg ? *style.fg : default_text_)));
        p_.drawText(box, text_flags, str);
    }

    size_f measure(const std::string& s, const text_style& style) const override
    {
        const QRectF box = bounds(QString::fromUtf8(s.data(), qsizetype(s.size())), font(style), {0, 0});
        return {float(box.width()), float(box.height())};
    }

private:
    // Top-left aligned, line breaks kept, nothing wrapped.
    static constexpr int text_flags = Qt::AlignLeft | Qt::AlignTop | Qt::TextDontClip | Qt::TextExpandTabs;

    QFont font(const text_style& style) const
    {
        QFont f = style.monospace ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                                  : (w_ ? w_->font() : QApplication::font());
        if (style.monospace && w_) f.setPointSizeF(w_->font().pointSizeF());
        if (style.size > 0) f.setPointSizeF(style.size);
        if (style.bold) f.setBold(true);
        if (style.italic) f.setItalic(true);
        return f;
    }

    static QRectF bounds(const QString& s, const QFont& f, point_f at)
    {
        const QFontMetricsF fm(f);
        QRectF box = fm.boundingRect(QRectF(at.x, at.y, 1e6, 1e6), text_flags, s.isEmpty() ? QStringLiteral(" ") : s);
        if (s.isEmpty()) box.setWidth(0);
        return box;
    }

    QPen pen(const paint& paint, float width, line_cap cap) const
    {
        QPen pen(qcolor(resolve(paint)), width);
        pen.setCapStyle(cap == line_cap::round ? Qt::RoundCap : Qt::FlatCap);
        pen.setJoinStyle(Qt::RoundJoin);
        return pen;
    }

    void use(const std::optional<paint>& fill, const std::optional<paint>& stroke, float width)
    {
        p_.setBrush(fill ? QBrush(qcolor(resolve(*fill))) : QBrush(Qt::NoBrush));
        if (stroke)
            p_.setPen(pen(*stroke, width, line_cap::butt));
        else
            p_.setPen(Qt::NoPen);
    }

    QPainter& p_;
    const QWidget* w_;
    paint default_text_;
    std::vector<std::pair<theme_color, color>> overrides_;
};

}  // namespace rap::qt6
