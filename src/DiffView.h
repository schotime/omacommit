#pragma once

#include <QColor>
#include <QHash>
#include <QPlainTextEdit>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QLabel;
class QScrollBar;
class QStackedWidget;
class QTimer;
class QToolButton;

struct DiffColors {
    QColor bg, fg, gutterBg, gutterFg, border;
    QColor removed, removedStrong, added, addedStrong, empty;
    QColor mine, theirs, marker, base;   // conflict sections, in the resolve window
    QColor issue;                        // whitespace problems on added lines
};

// One side of the side-by-side diff: read-only editor with a line-number
// gutter, full-width change backgrounds and word-level highlights.
class DiffPane : public QPlainTextEdit {
public:
    enum Kind { Same, Removed, Added, Empty, Mine, Theirs, Base, Marker };
    static constexpr int FillerState = 1;   // QTextBlock::userState of a filler row
    struct Line {
        QString text;
        int number = -1;
        Kind kind = Same;
        int hlStart = -1;   // intra-line change range [hlStart, hlEnd)
        int hlEnd = -1;
        int number2 = -1;   // inline view: the new side's line number (number is the old side's)
        QString issue;      // whitespace problem git would report on this line, if any
    };

    explicit DiffPane(QWidget *parent = nullptr);
    void setLines(const QVector<Line> &lines);
    // Recolours the rows without touching the text -- for an editable pane,
    // where replacing the text would lose the cursor and the undo history.
    void setKinds(const QVector<Line> &lines);
    void setColors(const DiffColors &c);
    void setDualNumbers(bool dual);   // two line-number columns, for the inline view
    void setShowWhitespace(bool show); // spaces as ·, tabs as →

    int gutterWidth() const;
    void paintGutter(QPaintEvent *e);

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool viewportEvent(QEvent *e) override;

private:
    void updateGutterWidth();
    void updateGutter(const QRect &rect, int dy);

    QWidget *m_gutter;
    QVector<Line> m_lines;
    DiffColors m_c;
    bool m_dual = false;
};

class DiffView : public QWidget {
public:
    // TortoiseGitMerge's "use ..." actions. Left is HEAD and right is the file,
    // so every one of them copies from left into right.
    enum class Take { Block, Line, LeftBeforeRight, WholeFile };

    explicit DiffView(QWidget *parent = nullptr);

    // Set by the owner. Edits -- typed into the right pane or taken from the
    // left -- live in a buffer here until saved; nothing touches the file
    // before onSave.
    std::function<QByteArray(const QStringList &)> rediff;   // right lines -> unified diff against left
    std::function<bool(const QStringList &)> onSave;          // write them; false if not written
    std::function<void()> onSaveRequested;                    // the Save button
    std::function<void()> onOptionsChanged;                   // whitespace settings: show the diff again
    bool showWhitespace() const;
    // Marks whitespace problems on the right side, keyed by its 1-based line.
    void setWhitespaceIssues(const QHash<int, QString> &byLine);

    void showDiff(const QString &title, const QByteArray &unifiedDiff, bool editable = false);
    void showMessage(const QString &title, const QString &message);
    void applyTheme();

    void setEditable(bool editable);
    // Resolve window: a caption over each pane, tints instead of red/green,
    // and no Alt+Up/Down (the window uses them to move between conflicts).
    void setPaneCaptions(const QString &left, const QString &right);
    void setSideTints(const QColor &left, const QColor &right);
    void setNavShortcutsEnabled(bool enabled);
    int rowForLine(bool right, int lineNumber) const;
    void revealRow(int row);
    bool isDirty() const;
    bool save();       // flushes pending typing, then onSave; true when nothing is left unsaved
    void discard();    // forget unsaved edits
    void take(Take how, int row);
    void undo();
    void redo();

    int rowCount() const { return int(m_l.size()); }
    QStringList resultOf(Take how, int row) const;
    QStringList rightLines() const;   // the right side as shown (the buffer, once flushed)

    // Text <-> lines the way the panes see it: no \r, no trailing empty line.
    static QStringList linesOf(const QByteArray &data);
    // Lines back to bytes, keeping the original's line endings and final newline.
    static QByteArray compose(const QStringList &lines, const QByteArray &original);

    void nextChange();
    void prevChange();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void setRows(const QVector<DiffPane::Line> &left, const QVector<DiffPane::Line> &right);
    // Side by side or inline: the rows are the same, only the drawing differs.
    bool showingRows() const;
    void applyIssues();
    bool inlineWanted() const;
    void buildInline();
    void updateMode();
    void updateNarrow();
    QScrollBar *activeScrollBar() const;
    bool rowsFromDiff(const QByteArray &diff, const QStringList &fallback, bool *binary);
    void applyBuffer(const QStringList &buffer);
    QStringList editorLines() const;
    void onTyped();
    void flushPending();
    void updateHeader();
    void showContextMenu(DiffPane *pane, const QPoint &pos);
    bool isChanged(int row) const;
    void gotoRow(int row);
    void updateNav();
    void updateStats();

    DiffPane *m_left;
    DiffPane *m_right;
    DiffPane *m_inline;
    QLabel *m_inlineCaption;
    QToolButton *m_modeBtn;
    QToolButton *m_optsBtn;
    QAction *m_wsShow;
    QAction *m_wsIgnore;
    QLabel *m_wsNote;
    QLabel *m_issueNote;
    QHash<int, QString> m_issues;
    bool m_wantEditable = false;
    QLabel *m_title;
    QLabel *m_stats;
    QLabel *m_message;
    QStackedWidget *m_stack;
    QToolButton *m_save;
    QToolButton *m_prev;
    QToolButton *m_next;
    QLabel *m_leftCaption;
    QLabel *m_rightCaption;
    QColor m_leftTint, m_rightTint;
    QTimer *m_rediffTimer;

    QString m_name;
    QVector<DiffPane::Line> m_l, m_r;
    bool m_editable = false;
    QVector<int> m_inlineToRow, m_rowToInline;
    bool m_prefInline = false;   // the user's choice, remembered
    bool m_narrow = false;       // too little width for two readable sides
    bool m_forceSplit = false;   // side by side asked for while narrow, until it widens

    // The right side as last re-diffed, and as it is on disk.
    QStringList m_buffer, m_original;
    QVector<QStringList> m_undo, m_redo;
    bool m_pending = false;     // typed into the pane since the last re-diff
    bool m_rendering = false;   // setPlainText in progress: not the user typing

    QVector<int> m_changeStarts;
    int m_current = -1;
    int m_removed = 0;
    int m_added = 0;
};
