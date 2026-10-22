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

inline QColor qcolor(Color c)
{
    return QColor(c.r, c.g, c.b, c.a);
}

inline Color fromQColor(const QColor& c)
{
    return {uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), uint8_t(c.alpha())};
}

// A theme role from `w`'s palette (the application's without one). Roles a
// palette lacks are derived as the GTK backends derive them, from the text
// colour and whether the theme is dark.
inline Color themeColor(const QWidget* w, ThemeColor role)
{
    const QPalette pal = w ? w->palette() : QApplication::palette();
    const QColor text = pal.color(QPalette::Text);
    const bool dark = text.lightnessF() > 0.5;
    auto faded = [&](double a)
    {
        QColor c = text;
        c.setAlphaF(float(a));
        return fromQColor(c);
    };
    switch (role)
    {
        case ThemeColor::Window: return fromQColor(pal.color(QPalette::Window));
        case ThemeColor::WindowText: return fromQColor(pal.color(QPalette::WindowText));
        case ThemeColor::Base: return fromQColor(pal.color(QPalette::Base));
        case ThemeColor::AlternateBase: return fromQColor(pal.color(QPalette::AlternateBase));
        case ThemeColor::Text: return fromQColor(text);
        case ThemeColor::PlaceholderText: return fromQColor(pal.color(QPalette::PlaceholderText));
        case ThemeColor::Highlight: return fromQColor(pal.color(QPalette::Highlight));
        case ThemeColor::HighlightText: return fromQColor(pal.color(QPalette::HighlightedText));
        case ThemeColor::Mid: return faded(0.35);
        case ThemeColor::Accent: return fromQColor(pal.color(QPalette::Accent));
        case ThemeColor::Link: return fromQColor(pal.color(QPalette::Link));
        case ThemeColor::Positive: return dark ? Color{0x8f, 0xf0, 0xa4} : Color{0x26, 0xa2, 0x69};
        case ThemeColor::Negative: return dark ? Color{0xf6, 0x61, 0x51} : Color{0xc0, 0x1c, 0x28};
        case ThemeColor::Neutral: return faded(0.6);
    }
    return fromQColor(text);
}

inline Color resolve(const QWidget* w, const Paint& p)
{
    if (auto* c = std::get_if<Color>(&p)) return *c;
    return themeColor(w, std::get<ThemeColor>(p));
}

// Pixels uploaded once and drawn many times. Premultiplied ARGB32 is
// rapport's BGRA byte order on little-endian machines, so most uploads are a
// straight copy.
class QtImage : public Image
{
public:
    QtImage(int w, int h, PixelFormat format, const uint8_t* pixels, size_t stride)
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
            if (format_ == PixelFormat::Bgra8Premultiplied)
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
    PixelFormat format_;
};

// A Painter over a QPainter that's already begun on `widget` (or on an
// item view's viewport, for list cells). Coordinates are logical pixels;
// Qt applies the device pixel ratio.
class QtPainter : public Painter
{
public:
    QtPainter(QPainter& p, const QWidget* widget, std::optional<Paint> defaultText = {})
        : p_(p)
        , w_(widget)
        , defaultText_(defaultText ? *defaultText : Paint{ThemeColor::Text})
    {
        p_.setRenderHint(QPainter::Antialiasing, true);
        p_.setRenderHint(QPainter::TextAntialiasing, true);
    }

    Color resolve(const Paint& p) const override
    {
        if (auto* role = std::get_if<ThemeColor>(&p))
            for (auto& [r, c] : overrides_)
                if (r == *role) return c;
        return qt6::resolve(w_, p);
    }

    // Replaces a theme colour for this painter (a list's soft selection).
    void overrideColor(ThemeColor role, Color c) { overrides_.push_back({role, c}); }

    void line(PointF a, PointF b, const Paint& paint, float width, LineCap cap) override
    {
        p_.setPen(pen(paint, width, cap));
        p_.setBrush(Qt::NoBrush);
        p_.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
    }

    void curve(PointF a, PointF c1, PointF c2, PointF b, const Paint& paint, float width) override
    {
        QPainterPath path(QPointF(a.x, a.y));
        path.cubicTo(QPointF(c1.x, c1.y), QPointF(c2.x, c2.y), QPointF(b.x, b.y));
        p_.strokePath(path, pen(paint, width, LineCap::Butt));
    }

    void circle(PointF c, float r, std::optional<Paint> fill, std::optional<Paint> stroke) override
    {
        use(fill, stroke, 1);
        p_.drawEllipse(QPointF(c.x, c.y), r, r);
    }

    void rect(RectF r, float radius, std::optional<Paint> fill, std::optional<Paint> stroke) override
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

    void polygon(const std::vector<PointF>& pts, std::optional<Paint> fill, std::optional<Paint> stroke,
                 float width) override
    {
        if (pts.empty()) return;
        QPolygonF poly;
        poly.reserve(qsizetype(pts.size()));
        for (PointF pt : pts) poly << QPointF(pt.x, pt.y);
        use(fill, stroke, width);
        p_.drawPolygon(poly);
    }

    // rapport's angles are radians clockwise from 3 o'clock; Qt's arcs are in
    // degrees anticlockwise.
    void arc(PointF c, float r, float start, float end, const Paint& paint, float width, LineCap cap) override
    {
        const QRectF box(c.x - r, c.y - r, 2 * r, 2 * r);
        const double toDeg = 180 / std::numbers::pi;
        QPainterPath path;
        path.arcMoveTo(box, -start * toDeg);
        path.arcTo(box, -start * toDeg, -(end - start) * toDeg);
        p_.strokePath(path, pen(paint, width, cap));
    }

    void image(const Image& img, RectF src, RectF dst, Filter filter, float alpha) override
    {
        if (src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0) return;
        p_.save();
        p_.setRenderHint(QPainter::SmoothPixmapTransform, filter == Filter::Linear);
        p_.setOpacity(p_.opacity() * std::clamp(alpha, 0.f, 1.f));
        p_.drawImage(QRectF(dst.x, dst.y, dst.w, dst.h), static_cast<const QtImage&>(img).image(),
                     QRectF(src.x, src.y, src.w, src.h));
        p_.restore();
    }

    float deviceScale() const override { return w_ ? float(w_->devicePixelRatioF()) : 1.f; }

    void pushClip(RectF r) override
    {
        p_.save();
        p_.setClipRect(QRectF(r.x, r.y, r.w, r.h), Qt::IntersectClip);
    }
    void popClip() override { p_.restore(); }

    void text(PointF at, const std::string& s, const TextStyle& style) override
    {
        const QString str = QString::fromUtf8(s.data(), qsizetype(s.size()));
        const QFont f = font(style);
        const QRectF box = bounds(str, f, at);
        if (style.bg) p_.fillRect(box, qcolor(resolve(*style.bg)));
        p_.setFont(f);
        p_.setPen(qcolor(resolve(style.fg ? *style.fg : defaultText_)));
        p_.drawText(box, kTextFlags, str);
    }

    SizeF measure(const std::string& s, const TextStyle& style) const override
    {
        const QRectF box = bounds(QString::fromUtf8(s.data(), qsizetype(s.size())), font(style), {0, 0});
        return {float(box.width()), float(box.height())};
    }

private:
    // Top-left aligned, line breaks kept, nothing wrapped.
    static constexpr int kTextFlags = Qt::AlignLeft | Qt::AlignTop | Qt::TextDontClip | Qt::TextExpandTabs;

    QFont font(const TextStyle& style) const
    {
        QFont f = style.monospace ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                                  : (w_ ? w_->font() : QApplication::font());
        if (style.monospace && w_) f.setPointSizeF(w_->font().pointSizeF());
        if (style.size > 0) f.setPointSizeF(style.size);
        if (style.bold) f.setBold(true);
        if (style.italic) f.setItalic(true);
        return f;
    }

    static QRectF bounds(const QString& s, const QFont& f, PointF at)
    {
        const QFontMetricsF fm(f);
        QRectF box = fm.boundingRect(QRectF(at.x, at.y, 1e6, 1e6), kTextFlags, s.isEmpty() ? QStringLiteral(" ") : s);
        if (s.isEmpty()) box.setWidth(0);
        return box;
    }

    QPen pen(const Paint& paint, float width, LineCap cap) const
    {
        QPen pen(qcolor(resolve(paint)), width);
        pen.setCapStyle(cap == LineCap::Round ? Qt::RoundCap : Qt::FlatCap);
        pen.setJoinStyle(Qt::RoundJoin);
        return pen;
    }

    void use(const std::optional<Paint>& fill, const std::optional<Paint>& stroke, float width)
    {
        p_.setBrush(fill ? QBrush(qcolor(resolve(*fill))) : QBrush(Qt::NoBrush));
        if (stroke)
            p_.setPen(pen(*stroke, width, LineCap::Butt));
        else
            p_.setPen(Qt::NoPen);
    }

    QPainter& p_;
    const QWidget* w_;
    Paint defaultText_;
    std::vector<std::pair<ThemeColor, Color>> overrides_;
};

}  // namespace rap::qt6
