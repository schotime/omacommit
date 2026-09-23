#pragma once

#include <QProxyStyle>

// Fusion, with og's own checkboxes: a stylesheet can colour a checkbox but
// not draw a tick without image files, which couldn't follow the theme. These
// are painted from the theme's colours, so they re-colour with it.
class Style : public QProxyStyle {
public:
    Style();
    void drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p, const QWidget *w) const override;
    int pixelMetric(PixelMetric m, const QStyleOption *opt = nullptr, const QWidget *w = nullptr) const override;
};
