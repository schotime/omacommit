#include "ImageCompare.h"
#include "Theme.h"

#include <QBuffer>
#include <QImageReader>
#include <QLocale>
#include <QPainter>

ImageCompare::ImageCompare(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(120, 120);
}

ImageCompare::Side ImageCompare::load(const QString &caption, const QByteArray &data, bool exists)
{
    Side s;
    s.caption = caption;
    s.exists = exists;
    s.bytes = data.size();
    if (!exists || data.isEmpty())
        return s;
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    if (!reader.canRead())
        return s;
    s.size = reader.size();
    const QByteArray format = reader.format();
    if ((format == "svg" || format == "svgz") && s.size.isValid() && qMax(s.size.width(), s.size.height()) < 1024)
        reader.setScaledSize(s.size.scaled(1024, 1024, Qt::KeepAspectRatio));
    s.image = reader.read();
    if (!s.size.isValid())
        s.size = s.image.size();
    return s;
}

void ImageCompare::setSides(const Side &left, const Side &right)
{
    m_left = left;
    m_right = right;
    update();
}

void ImageCompare::setStacked(bool stacked)
{
    if (m_stacked == stacked)
        return;
    m_stacked = stacked;
    update();
}

void ImageCompare::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const ThemeColors &t = Theme::instance().colors();
    p.fillRect(rect(), t.background);
    QRect a = rect(), b = rect();
    if (m_stacked) {
        a.setBottom(height() / 2 - 1);
        b.setTop(height() / 2);
        p.fillRect(QRect(0, b.top(), width(), 1), t.border);
    } else {
        a.setRight(width() / 2 - 1);
        b.setLeft(width() / 2);
        p.fillRect(QRect(b.left(), 0, 1, height()), t.border);
    }
    paintSide(p, a, m_left);
    paintSide(p, b, m_right);
}

void ImageCompare::paintSide(QPainter &p, const QRect &cell, const Side &s)
{
    const ThemeColors &t = Theme::instance().colors();
    const QFontMetrics fm(font());
    const QRect inner = cell.adjusted(16, 12, -16, -16);

    QString info;
    if (!s.exists)
        info = tr("Not in this version");
    else if (s.image.isNull())
        info = tr("Not an image that can be shown");
    else
        info = tr("%1 × %2 px  ·  %3")
                   .arg(s.size.width())
                   .arg(s.size.height())
                   .arg(QLocale().formattedDataSize(s.bytes, 1, QLocale::DataSizeTraditionalFormat));
    p.setFont(font());
    p.setPen(t.foreground);
    p.drawText(QRect(inner.left(), inner.top(), inner.width(), fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(s.caption, Qt::ElideRight, inner.width()));
    p.setPen(t.muted);
    p.drawText(QRect(inner.left(), inner.top() + fm.height(), inner.width(), fm.height()),
               Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(info, Qt::ElideRight, inner.width()));
    if (s.image.isNull())
        return;

    // Fitted, never enlarged, centred across and from the top down.
    const QRect area = inner.adjusted(0, 2 * fm.height() + 10, 0, 0);
    if (area.width() < 8 || area.height() < 8)
        return;
    QSize shown = s.size;
    if (shown.width() > area.width() || shown.height() > area.height())
        shown.scale(area.size(), Qt::KeepAspectRatio);
    const QRect target(area.left() + (area.width() - shown.width()) / 2, area.top(), shown.width(), shown.height());

    // A checkerboard behind, in the theme's own tones, so transparency shows.
    QPixmap tile(16, 16);
    tile.fill(Theme::mix(t.background, t.foreground, 0.05));
    {
        QPainter tp(&tile);
        const QColor dark = Theme::mix(t.background, t.foreground, 0.11);
        tp.fillRect(0, 0, 8, 8, dark);
        tp.fillRect(8, 8, 8, 8, dark);
    }
    p.setBrushOrigin(target.topLeft());
    p.fillRect(target, QBrush(tile));
    p.setRenderHint(QPainter::SmoothPixmapTransform, shown != s.image.size());
    p.drawImage(target, s.image);
    p.setPen(t.border);
    p.drawRect(target.adjusted(-1, -1, 0, 0));
}
