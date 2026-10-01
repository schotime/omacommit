#include "Style.h"
#include "Theme.h"

#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QStyleFactory>
#include <QStyleOption>

Style::Style() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

namespace {
// Painted when drawn, so it follows a theme switch without being set again.
class ClearIconEngine : public QIconEngine {
public:
    QIconEngine *clone() const override { return new ClearIconEngine; }

    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        const ThemeColors &c = Theme::instance().colors();
        const QColor color = mode == QIcon::Disabled ? c.border
                           : mode == QIcon::Active || mode == QIcon::Selected ? c.foreground
                                                                              : c.muted;
        const qreal s = qMin(rect.width(), rect.height()) * 0.36;   // half the ✕'s span
        const QPointF mid = QRectF(rect).center();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(color, qMax<qreal>(1.5, s / 3.5), Qt::SolidLine, Qt::RoundCap));
        p->drawLine(mid + QPointF(-s, -s), mid + QPointF(s, s));
        p->drawLine(mid + QPointF(-s, s), mid + QPointF(s, -s));
        p->restore();
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pm(size * scale);
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(), size), mode, state);
        return pm;
    }
};
} // namespace

QIcon Style::standardIcon(StandardPixmap sp, const QStyleOption *opt, const QWidget *w) const
{
    if (sp == SP_LineEditClearButton)
        return QIcon(new ClearIconEngine);
    return QProxyStyle::standardIcon(sp, opt, w);
}

int Style::pixelMetric(PixelMetric m, const QStyleOption *opt, const QWidget *w) const
{
    if (m == PM_IndicatorWidth || m == PM_IndicatorHeight)
        return 15;
    return QProxyStyle::pixelMetric(m, opt, w);
}

void Style::drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p, const QWidget *w) const
{
    if (pe != PE_IndicatorCheckBox && pe != PE_IndicatorItemViewItemCheck) {
        QProxyStyle::drawPrimitive(pe, opt, p, w);
        return;
    }
    const ThemeColors &c = Theme::instance().colors();
    const bool on = opt->state & State_On;
    const bool partial = opt->state & State_NoChange;
    const bool enabled = opt->state & State_Enabled;
    const bool hover = enabled && (opt->state & State_MouseOver);

    const qreal size = qMin<qreal>(15, qMin(opt->rect.width(), opt->rect.height()));
    const QRectF box(opt->rect.x() + (opt->rect.width() - size) / 2.0 + 0.5,
                     opt->rect.y() + (opt->rect.height() - size) / 2.0 + 0.5, size - 1, size - 1);
    const QColor fill = enabled ? c.accent : Theme::mix(c.background, c.muted, 0.5);

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    if (on || partial) {
        p->setPen(Qt::NoPen);
        p->setBrush(fill);
        p->drawRoundedRect(box, 3, 3);
        // The mark in the background colour: it reads against the accent in
        // dark and light themes alike.
        QPen mark(c.background, qMax<qreal>(1.6, size / 8.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p->setPen(mark);
        p->setBrush(Qt::NoBrush);
        const qreal x = box.x(), y = box.y(), s = box.width();
        if (partial) {   // some, not all: a dash
            p->drawLine(QPointF(x + s * 0.28, y + s * 0.5), QPointF(x + s * 0.72, y + s * 0.5));
        } else {
            QPainterPath tick;
            tick.moveTo(x + s * 0.24, y + s * 0.52);
            tick.lineTo(x + s * 0.43, y + s * 0.70);
            tick.lineTo(x + s * 0.77, y + s * 0.31);
            p->drawPath(tick);
        }
    } else {
        p->setPen(QPen(hover ? c.accent : enabled ? c.muted : c.border, 1.2));
        p->setBrush(c.surface);
        p->drawRoundedRect(box, 3, 3);
    }
    p->restore();
}
