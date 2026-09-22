#pragma once

#include <QColor>
#include <QPlainTextEdit>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

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
    // TortoiseGitMerge's "use ..." actions. Left is HEAD and right is the file
    // on disk, so every one of them copies from left into right.
    enum class Take { Block, Line, LeftBeforeRight, WholeFile };

    explicit DiffView(QWidget *parent = nullptr);

    // Set by the owner, which does the writing. onTake gets the right side's
    // new lines (empty for WholeFile, which is better done by git).
    std::function<void(Take, const QStringList &)> onTake;
    std::function<void()> onUndo;
    void setEditable(bool editable, bool canUndo);

    int rowCount() const { return int(m_l.size()); }
    QStringList resultOf(Take how, int row) const;
    QStringList rightLines() const;

    // Text <-> lines the way the panes see it: no \r, no trailing empty line.
    static QStringList linesOf(const QByteArray &data);
    // Lines back to bytes, keeping the original's line endings and final newline.
    static QByteArray compose(const QStringList &lines, const QByteArray &original);

    void showDiff(const QString &title, const QByteArray &unifiedDiff);
    void showMessage(const QString &title, const QString &message);
    void applyTheme();

    void nextChange();
    void prevChange();

private:
    void showContextMenu(DiffPane *pane, const QPoint &pos);
    bool isChanged(int row) const;
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

    QVector<DiffPane::Line> m_l, m_r;
    bool m_editable = false;
    bool m_canUndo = false;

    QVector<int> m_changeStarts;
    int m_current = -1;
    int m_removed = 0;
    int m_added = 0;
};
