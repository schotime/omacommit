#pragma once

#include <QColor>
#include <QHash>
#include <QSet>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTextLayout>
#include <QVector>
#include <QWidget>

#include <functional>

class ImageCompare;
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
    QColor frame, frameIdle;             // resolve: the current conflict's frame, the others'
    QColor automatic;                    // resolve: differences git merged by itself
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

    // Numbered frames around runs of lines -- the conflicts, in the resolve
    // window. With `muteOthers`, changed lines outside every frame are drawn
    // as merged automatically rather than as changes.
    struct Group {
        int first = -1, last = -1;   // lines, inclusive
        int number = 0;
        bool current = false, done = false;
    };
    void setGroups(const QVector<Group> &groups, bool muteOthers);

    explicit DiffPane(QWidget *parent = nullptr);
    void setLines(const QVector<Line> &lines);   // clears the syntax colours: set them after
    // Syntax colours, one run of spans per line of the pane (empty: plain).
    void setSyntax(const QVector<QVector<QTextLayout::FormatRange>> &perLine);
    // Recolours the rows without touching the text -- for an editable pane,
    // where replacing the text would lose the cursor and the undo history.
    void setKinds(const QVector<Line> &lines);
    void setColors(const DiffColors &c);
    void setDualNumbers(bool dual);   // two line-number columns, for the inline view
    void setShowWhitespace(bool show); // spaces as ·, tabs as →, in the muted colour
    bool showsWhitespace() const { return m_showWs; }
    int lineCount() const { return int(m_lines.size()); }
    const Line &lineAt(int i) const { return m_lines.at(i); }

    int gutterWidth() const;
    void paintGutter(QPaintEvent *e);
    void gutterMouse(QMouseEvent *e);   // the gutter's mouse, moves and presses

    // Small buttons in the gutter beside the changed line under the pointer
    // (← put back the other side, + stage, − unstage), left to right. Each acts
    // on that line, or with Shift its whole block. `chipSpan` gives the lines
    // chip `k` would act on (none: not offered here), marked while the pointer
    // is on it.
    struct Chip {
        QString glyph, tip;
    };
    void setChips(const QVector<Chip> &chips);
    std::function<QVector<int>(int k, int line, bool block)> chipSpan;
    std::function<void(int k, int line, bool block)> onChip;

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool viewportEvent(QEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    void updateGutterWidth();
    void updateGutter(const QRect &rect, int dy);

    void paintGroups(QPainter &p, const QRect &area);
    const Group *groupAt(int line) const;
    int chipWidth() const { return int(m_chips.size()) * ChipStep; }
    static constexpr int ChipStep = 18;
    int lineAtY(int y) const;
    QVector<int> offered(int line) const;   // the chips offered on a line
    void hover(int line, int chip, bool block);

    QVector<Chip> m_chips;
    int m_hover = -1;         // the changed line the pointer is on, with its chips
    int m_onChip = -1;        // the chip it is on: that chip's lines are marked
    QVector<int> m_marked;

    QWidget *m_gutter;
    QVector<Line> m_lines;
    QVector<Group> m_groups;
    bool m_muteOthers = false;
    QVector<QVector<QTextLayout::FormatRange>> m_syntax;
    DiffColors m_c;
    bool m_dual = false;
    bool m_showWs = false;
};

// The two versions of a file, as bytes, for showing it as an image. A side
// that does not exist (a new or deleted file) has `has...` false.
struct ImageSides {
    QByteArray before, after;
    bool hasBefore = true, hasAfter = true;
};
using ImageFetch = std::function<ImageSides()>;

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
    // The left file as it is, for "Use left whole file": with whitespace
    // ignored, the left pane's unchanged lines carry the right side's whitespace.
    std::function<QByteArray()> leftFile;
    bool showWhitespace() const;
    // Marks whitespace problems on the right side, keyed by its 1-based line.
    void setWhitespaceIssues(const QHash<int, QString> &byLine);

    // `images` fetches the two versions, only when they are needed: a binary
    // diff is shown as images when either side is one, and an SVG gets a
    // button to switch between its text and the picture.
    void showDiff(const QString &title, const QByteArray &unifiedDiff, bool editable = false,
                  const ImageFetch &images = {});
    void showMessage(const QString &title, const QString &message);
    void applyTheme();

    void setEditable(bool editable);
    // Resolve window: a caption over each pane, tints instead of red/green,
    // and no change arrows (the window steps through its conflicts instead).
    void setPaneCaptions(const QString &left, const QString &right);
    // The file shown, for its language: set before showDiff.
    void setFileName(const QString &path) { m_fileName = path; }
    // Resolve window: the conflicts as runs of aligned rows, framed and
    // numbered in both panes, with differences outside them drawn as merged
    // automatically. Empty clears it.
    void setConflictRows(const QVector<DiffPane::Group> &groups);
    // Resolve window: its own actions at the top of the panes' menu (inserted
    // before `before`), and clicks on a row of the left or right pane.
    std::function<void(QMenu *menu, QAction *before, int row, bool right)> extendMenu;
    std::function<void(int row, bool right, bool doubleClick)> onRowClicked;
    void setSideTints(const QColor &left, const QColor &right);
    // Off: no ↑↓ buttons and no Alt+Up/Down -- the resolve window steps
    // through its conflicts instead, with arrows of its own.
    void setChangeNavigation(bool enabled);
    int rowForLine(bool right, int lineNumber) const;
    void revealRow(int row);
    bool isDirty() const;
    bool save();       // flushes pending typing, then onSave; true when nothing is left unsaved
    void discard();    // forget unsaved edits
    void take(Take how, int row);
    void undo();
    void redo();

    int rowCount() const { return int(m_l.size()); }

    // Changed lines picked in the diff, by aligned row: `left` rows whose old
    // line is picked, `right` rows whose new line is.
    struct Selection {
        QSet<int> left, right;
        bool isEmpty() const { return left.isEmpty() && right.isEmpty(); }
    };
    // Actions on picked lines, offered first in the context menu while set:
    // the block under the pointer, and the line under it or the selected lines
    // ("Stage block" / "Stage line"). Cleared whenever something other than a
    // diff is shown.
    // `glyph`, when given, is also a chip in the gutter beside each changed line.
    void setLineActions(const QString &verb, std::function<void(const Selection &)> action,
                        const QString &glyph = QString());
    // One side with the picked lines taken over from the other, as git stages
    // lines: moving onto the left (staging), a picked new line is added and a
    // picked old line dropped; onto the right (unstaging), the reverse. Lines
    // not picked come from `exactTarget` (that side's file) by line number, so
    // ignoring whitespace can't leak into the result.
    QStringList linesApplied(const Selection &picked, bool toLeft, const QStringList &exactTarget) const;
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
    bool showImages(const QString &title);
    void updateImageButton();
    void applyIssues();
    bool inlineWanted() const;
    void buildInline();
    void applyConflictRows();
    void highlightRows();
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
    bool rowInView(int row) const;
    bool currentAway() const;
    void updateStats();

    DiffPane *m_left;
    DiffPane *m_right;
    DiffPane *m_inline;
    QLabel *m_inlineCaption;
    ImageCompare *m_image;
    QToolButton *m_imageBtn;
    ImageFetch m_imageFetch;
    QByteArray m_textDiff;         // an SVG's diff, to switch back to from its picture
    bool m_textEditable = false;
    bool m_svgAsImage = false;     // remembered
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
    Selection blockAt(int row) const;
    // paneLine: the pane's own line under the pointer; with `withSelection`,
    // the selected lines instead when it is among them.
    Selection pickedLines(DiffPane *pane, int paneLine, bool withSelection = true) const;
    void updateChips();
    QVector<int> paneLinesOf(DiffPane *pane, const Selection &s) const;

    QVector<DiffPane::Group> m_conflictRows;
    QString m_fileName;
    QString m_lineVerb;
    QString m_lineGlyph;
    enum class ChipAct { Revert, LineAction };
    QVector<ChipAct> m_chipActs;   // what each of the panes' chips does
    std::function<void(const Selection &)> m_lineAction;

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
