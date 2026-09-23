#pragma once

#include <QLabel>
#include <QPainter>
#include <QStyle>

// A one-line label that never asks for more width than it gets: text that
// doesn't fit is shortened with an ellipsis in the middle -- for a path, the
// start and the repository's own name stay visible -- and the full text is
// on hover.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(const QString &text, QWidget *parent = nullptr) : QLabel(text, parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setToolTip(text);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = contentsRect();
        const QString shown = fontMetrics().elidedText(text(), Qt::ElideMiddle, r.width());
        style()->drawItemText(&p, r, int(alignment()), palette(), isEnabled(), shown, foregroundRole());
    }
};
