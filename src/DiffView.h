#pragma once

#include <QColor>
#include <QPlainTextEdit>
#include <QVector>
#include <QWidget>

class QLabel;
class QStackedWidget;
class QToolButton;

struct DiffColors {
    QColor bg, fg, gutterBg, gutterFg, border;
    QColor removed, removedStrong, added, addedStrong, empty;
};

// One side of the side-by-side diff: read-only editor with a line-number
// gutter, full-width change backgrounds and word-level highlights.
class DiffPane : public QPlainTextEdit {
public:
    enum Kind { Same, Removed, Added, Empty };
    struct Line {
        QString text;
        int number = -1;
        Kind kind = Same;
        int hlStart = -1;   // intra-line change range [hlStart, hlEnd)
        int hlEnd = -1;
    };

    explicit DiffPane(QWidget *parent = nullptr);
    void setLines(const QVector<Line> &lines);
    void setColors(const DiffColors &c);

    int gutterWidth() const;
    void paintGutter(QPaintEvent *e);

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void updateGutterWidth();
    void updateGutter(const QRect &rect, int dy);

    QWidget *m_gutter;
    QVector<Line> m_lines;
    DiffColors m_c;
};

class DiffView : public QWidget {
public:
    explicit DiffView(QWidget *parent = nullptr);

    void showDiff(const QString &title, const QByteArray &unifiedDiff);
    void showMessage(const QString &title, const QString &message);
    void applyTheme();

    void nextChange();
    void prevChange();

private:
    void gotoRow(int row);
    void updateNav();
    void updateStats();

    DiffPane *m_left;
    DiffPane *m_right;
    QLabel *m_title;
    QLabel *m_stats;
    QLabel *m_message;
    QStackedWidget *m_stack;
    QToolButton *m_prev;
    QToolButton *m_next;

    QVector<int> m_changeStarts;
    int m_current = -1;
    int m_removed = 0;
    int m_added = 0;
};
