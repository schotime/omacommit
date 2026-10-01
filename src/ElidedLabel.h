#pragma once

#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>

#include <functional>

// A one-line label that never asks for more width than it gets: text that
// doesn't fit is shortened with an ellipsis in the middle -- for a path, the
// start and the repository's own name stay visible -- and the full text is
// on hover. With `onClick` set, the text is a button.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(const QString &text, QWidget *parent = nullptr) : QLabel(text, parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setToolTip(text);
        setMouseTracking(true);
    }

    std::function<void()> onClick;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = contentsRect();
        style()->drawItemText(&p, r, int(alignment()), palette(), isEnabled(), shown(), foregroundRole());
    }

    // Only the text is the button, not the empty width after it.
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (onClick)
            setCursor(onText(e->position().toPoint()) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        QLabel::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (onClick && e->button() == Qt::LeftButton && onText(e->position().toPoint()))
            onClick();
        else
            QLabel::mouseReleaseEvent(e);
    }

private:
    QString shown() const { return fontMetrics().elidedText(text(), Qt::ElideMiddle, contentsRect().width()); }
    bool onText(const QPoint &pos) const
    {
        return pos.x() - contentsRect().left() < fontMetrics().horizontalAdvance(shown());
    }
};
