#pragma once

#include <QImage>
#include <QWidget>

// Two versions of an image, side by side (or one above the other), each fitted
// to its half over a checkerboard so transparency shows, and never enlarged.
// Under each: its size in pixels and bytes, or why there is no picture.
class ImageCompare : public QWidget {
public:
    struct Side {
        QString caption;   // "Before", or what the resolve window calls that side
        bool exists = false;
        qint64 bytes = 0;
        QSize size;        // the image's own size
        QImage image;      // null when there is none or it could not be read
    };

    explicit ImageCompare(QWidget *parent = nullptr);

    // Decodes `data` by its content. SVGs are drawn large enough to stay crisp
    // when scaled down to fit; `size` keeps their own size.
    static Side load(const QString &caption, const QByteArray &data, bool exists);

    void setSides(const Side &left, const Side &right);
    bool hasImage() const { return !m_left.image.isNull() || !m_right.image.isNull(); }
    void setStacked(bool stacked);

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    void paintSide(QPainter &p, const QRect &cell, const Side &s);

    Side m_left, m_right;
    bool m_stacked = false;
};
