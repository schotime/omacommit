#pragma once

#include <QLayout>
#include <QStyle>
#include <QWidget>

// Lays its widgets out in a row and wraps them onto further rows when the
// width runs out, so a row of buttons never forces a minimum width wider than
// its widest button (Qt ships no such layout; this follows its example).
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget *parent = nullptr, int spacing = 6) : QLayout(parent) { setSpacing(spacing); setContentsMargins(0, 0, 0, 0); }
    ~FlowLayout() override { while (QLayoutItem *item = takeAt(0)) delete item; }

    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int i) const override { return m_items.value(i); }
    QLayoutItem *takeAt(int i) override { return i >= 0 && i < m_items.size() ? m_items.takeAt(i) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    void setGeometry(const QRect &r) override { QLayout::setGeometry(r); arrange(r, true); }
    QSize sizeHint() const override
    {
        // One row when there is room for it.
        int w = 0, h = 0;
        for (QLayoutItem *item : m_items) {
            w += item->sizeHint().width() + (w ? spacing() : 0);
            h = qMax(h, item->sizeHint().height());
        }
        return {w, h};
    }
    QSize minimumSize() const override
    {
        QSize s;
        for (QLayoutItem *item : m_items)
            s = s.expandedTo(item->minimumSize());
        return s;
    }

private:
    int arrange(const QRect &rect, bool apply) const
    {
        int x = rect.x(), y = rect.y(), lineHeight = 0;
        for (QLayoutItem *item : m_items) {
            const QSize size = item->sizeHint();
            if (x > rect.x() && x + size.width() > rect.right() + 1) {   // wrap
                x = rect.x();
                y += lineHeight + spacing();
                lineHeight = 0;
            }
            if (apply)
                item->setGeometry(QRect(QPoint(x, y), size));
            x += size.width() + spacing();
            lineHeight = qMax(lineHeight, size.height());
        }
        return y + lineHeight - rect.y();
    }

    QList<QLayoutItem *> m_items;
};
