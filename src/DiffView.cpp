#include "DiffView.h"
#include "GitRepo.h"
#include "ImageCompare.h"
#include "Syntax.h"
#include "Theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>
#include <QHelpEvent>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

class Gutter : public QWidget {
public:
    explicit Gutter(DiffPane *pane) : QWidget(pane), m_pane(pane) {}
    QSize sizeHint() const override { return {m_pane->gutterWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent *e) override { m_pane->paintGutter(e); }

private:
    DiffPane *m_pane;
};

struct Row {
    QString l, r;
    int ln = -1, rn = -1;
    DiffPane::Kind lk = DiffPane::Same, rk = DiffPane::Same;
};

QString stripCr(QString s)
{
    if (s.endsWith(u'\r'))
        s.chop(1);
    return s;
}

// Turns unified diff output into aligned side-by-side rows. Runs of removed
// and added lines are paired up; the shorter side gets filler rows.
QVector<Row> parseUnified(const QByteArray &data, bool *binary)
{
    QVector<Row> rows;
    *binary = false;
    static const QRegularExpression hunkRe(QStringLiteral(R"(^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@)"));

    bool inHunk = false;
    int l = 0, r = 0;
    QStringList dels, adds;

    auto flush = [&] {
        const int n = qMax(dels.size(), adds.size());
        for (int i = 0; i < n; ++i) {
            Row row;
            if (i < dels.size()) { row.l = stripCr(dels[i]); row.ln = l++; row.lk = DiffPane::Removed; }
            else row.lk = DiffPane::Empty;
            if (i < adds.size()) { row.r = stripCr(adds[i]); row.rn = r++; row.rk = DiffPane::Added; }
            else row.rk = DiffPane::Empty;
            rows.push_back(row);
        }
        dels.clear();
        adds.clear();
    };

    const QStringList lines = QString::fromUtf8(data).split(u'\n');
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("@@"))) {
            flush();
            const auto m = hunkRe.match(line);
            if (m.hasMatch()) {
                l = m.captured(1).toInt();
                r = m.captured(2).toInt();
                inHunk = true;
            }
            continue;
        }
        if (!inHunk) {
            if (line.startsWith(QLatin1String("Binary files")) || line.startsWith(QLatin1String("GIT binary patch")))
                *binary = true;
            continue;
        }
        if (line.isEmpty())
            continue;
        const QChar c = line.at(0);
        if (c == u'-') {
            dels << line.mid(1);
        } else if (c == u'+') {
            adds << line.mid(1);
        } else if (c == u' ') {
            flush();
            Row row;
            row.l = row.r = stripCr(line.mid(1));
            row.ln = l++;
            row.rn = r++;
            rows.push_back(row);
        } else if (c == u'\\') {
            // "\ No newline at end of file"
        } else {
            flush();
            inHunk = false;
        }
    }
    flush();
    return rows;
}

// Cheap word-level highlight: everything between the common prefix and suffix.
void inlineRange(const QString &a, const QString &b, int &as, int &ae, int &bs, int &be)
{
    const int n = qMin(a.size(), b.size());
    int p = 0;
    while (p < n && a[p] == b[p])
        ++p;
    int s = 0;
    while (s < n - p && a[a.size() - 1 - s] == b[b.size() - 1 - s])
        ++s;
    if (p + s == 0)
        return;   // nothing in common: the full-line colour says enough
    as = p; ae = a.size() - s;
    bs = p; be = b.size() - s;
}

} // namespace

// ---------------------------------------------------------------- DiffPane

DiffPane::DiffPane(QWidget *parent) : QPlainTextEdit(parent)
{
    setObjectName(QStringLiteral("diffPane"));
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFrameShape(QFrame::NoFrame);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_gutter = new Gutter(this);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { updateGutterWidth(); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &r, int dy) { updateGutter(r, dy); });
    updateGutterWidth();
}

void DiffPane::setLines(const QVector<Line> &lines)
{
    m_lines = lines;
    QStringList text;
    text.reserve(lines.size());
    for (const Line &l : lines)
        text << l.text;
    setPlainText(text.join(u'\n'));
    // Filler rows only exist to keep the panes aligned. Tagging them lets an
    // edited right pane be read back as the file: an untouched, still-empty
    // filler is not a line.
    int i = 0;
    for (QTextBlock b = document()->begin(); b.isValid() && i < lines.size(); b = b.next(), ++i)
        b.setUserState(lines.at(i).kind == Empty ? FillerState : -1);
    m_syntax.clear();
    updateGutterWidth();
    viewport()->update();
}

// Laid straight onto each line's layout, as QSyntaxHighlighter does, so the
// text itself -- and the undo history of an edited pane -- is untouched.
void DiffPane::setSyntax(const QVector<QVector<QTextLayout::FormatRange>> &perLine)
{
    if (perLine.isEmpty() && m_syntax.isEmpty())
        return;
    m_syntax = perLine;
    QTextDocument *doc = document();
    const bool modified = doc->isModified();
    int i = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next(), ++i)
        if (QTextLayout *layout = b.layout())
            layout->setFormats(i < m_syntax.size() ? m_syntax.at(i) : QList<QTextLayout::FormatRange>());
    doc->markContentsDirty(0, doc->characterCount());
    doc->setModified(modified);
}

void DiffPane::setGroups(const QVector<Group> &groups, bool muteOthers)
{
    m_groups = groups;
    m_muteOthers = muteOthers;
    viewport()->update();
}

const DiffPane::Group *DiffPane::groupAt(int line) const
{
    for (const Group &g : m_groups)
        if (line >= g.first && line <= g.last)
            return &g;
    return nullptr;
}

// Each group framed, with its number in a tab at the frame's top right: in
// the accent colour for the current one, dashed with a tick once resolved.
void DiffPane::paintGroups(QPainter &p, const QRect &area)
{
    if (m_groups.isEmpty())
        return;
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF off = contentOffset();
    const int w = viewport()->width();
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    const QFontMetrics sfm(small);
    for (const Group &g : m_groups) {
        const QTextBlock first = document()->findBlockByNumber(g.first);
        const QTextBlock last = document()->findBlockByNumber(g.last);
        if (!first.isValid() || !last.isValid())
            continue;
        const qreal top = blockBoundingGeometry(first).translated(off).top();
        const qreal bottom = blockBoundingGeometry(last).translated(off).bottom();
        if (bottom < area.top() || top > area.bottom())
            continue;
        const QColor col = g.current ? m_c.frame : m_c.frameIdle;
        QPen pen(col, g.current ? 2.0 : 1.0, g.done ? Qt::DashLine : Qt::SolidLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const QRectF box(1.5, top + 0.5, w - 3, bottom - top - 1);
        p.drawRoundedRect(box, 3, 3);
        const QString label = g.done ? QStringLiteral("✓ %1").arg(g.number) : QString::number(g.number);
        const qreal lw = sfm.horizontalAdvance(label) + 10, lh = sfm.height() + 2;
        const QRectF tab(box.right() - lw - 4, box.top(), lw, lh);
        p.setPen(Qt::NoPen);
        p.setBrush(g.current ? m_c.frame : m_c.bg);
        p.drawRoundedRect(tab, 3, 3);
        p.setPen(g.current ? m_c.bg : col);
        p.setFont(small);
        p.drawText(tab, Qt::AlignCenter, label);
    }
}

void DiffPane::setKinds(const QVector<Line> &lines)
{
    m_lines = lines;
    updateGutterWidth();
    viewport()->update();
    m_gutter->update();
}

void DiffPane::setColors(const DiffColors &c)
{
    m_c = c;
    setTabStopDistance(fontMetrics().horizontalAdvance(u' ') * 4);
    updateGutterWidth();
    viewport()->update();
    m_gutter->update();
}

int DiffPane::gutterWidth() const
{
    int maxNo = 1;
    for (const Line &l : m_lines)
        maxNo = qMax(maxNo, qMax(l.number, l.number2));
    const int digits = qMax(3, int(QString::number(maxNo).size()));
    const int column = fontMetrics().horizontalAdvance(u'9') * digits;
    return m_dual ? column * 2 + 26 : column + 18;
}

bool DiffPane::viewportEvent(QEvent *e)
{
    // Hovering a marked line says what git would complain about.
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        const int i = cursorForPosition(he->pos()).blockNumber();
        const QString issue = i >= 0 && i < m_lines.size() ? m_lines.at(i).issue : QString();
        if (issue.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(he->globalPos(), tr("Whitespace: %1").arg(issue), viewport());
        return true;
    }
    return QPlainTextEdit::viewportEvent(e);
}

// Drawn by paintEvent rather than Qt's ShowTabsAndSpaces, which can only use
// the text colour; the markers belong in the background, not the text.
void DiffPane::setShowWhitespace(bool show)
{
    m_showWs = show;
    viewport()->update();
}

void DiffPane::setDualNumbers(bool dual)
{
    m_dual = dual;
    updateGutterWidth();
    m_gutter->update();
}

void DiffPane::updateGutterWidth()
{
    setViewportMargins(gutterWidth(), 0, 0, 0);
}

void DiffPane::updateGutter(const QRect &rect, int dy)
{
    if (dy)
        m_gutter->scroll(0, dy);
    else
        m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
}

void DiffPane::resizeEvent(QResizeEvent *e)
{
    QPlainTextEdit::resizeEvent(e);
    const QRect cr = contentsRect();
    m_gutter->setGeometry(cr.left(), cr.top(), gutterWidth(), cr.height());
}

void DiffPane::paintEvent(QPaintEvent *e)
{
    {
        QPainter p(viewport());
        const QPointF off = contentOffset();
        const int w = viewport()->width();
        for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
            const QRectF r = blockBoundingGeometry(block).translated(off);
            if (r.top() > e->rect().bottom())
                break;
            const int i = block.blockNumber();
            if (i >= m_lines.size())
                break;
            const Line &ln = m_lines.at(i);
            const QRectF full(0, r.top(), w, r.height());
            if (m_muteOthers && (ln.kind == Removed || ln.kind == Added || ln.kind == Empty) && !groupAt(i)) {
                p.fillRect(full, m_c.automatic);   // git merged this by itself: nothing to decide
                continue;
            }
            switch (ln.kind) {
            case Removed: p.fillRect(full, m_c.removed); break;
            case Added:   p.fillRect(full, m_c.added); break;
            case Empty:   p.fillRect(full, QBrush(m_c.empty, Qt::BDiagPattern)); break;
            case Mine:    p.fillRect(full, m_c.mine); break;
            case Theirs:  p.fillRect(full, m_c.theirs); break;
            case Base:    p.fillRect(full, m_c.base); break;
            case Marker:  p.fillRect(full, m_c.marker); break;
            case Same:    break;
            }
            if (ln.hlEnd > ln.hlStart && ln.hlStart >= 0) {
                QTextLayout *layout = block.layout();
                if (layout && layout->lineCount() > 0) {
                    const QTextLine tl = layout->lineAt(0);
                    const qreal x1 = tl.cursorToX(ln.hlStart);
                    const qreal x2 = tl.cursorToX(ln.hlEnd);
                    const qreal left = r.left() + layout->position().x();
                    p.fillRect(QRectF(left + x1, r.top(), x2 - x1, r.height()),
                               ln.kind == Removed ? m_c.removedStrong : m_c.addedStrong);
                }
            }
            if (!ln.issue.isEmpty()) {
                // Solid red over the problem, as git colours it: the trailing
                // whitespace, the indentation, or the stray blank line.
                QTextLayout *layout = block.layout();
                if (layout && layout->lineCount() > 0) {
                    const QTextLine tl = layout->lineAt(0);
                    const QString &s = ln.text;
                    int from = 0, to = int(s.size());
                    if (ln.issue.contains(QLatin1String("trailing whitespace"))) {
                        from = int(s.size());
                        while (from > 0 && s.at(from - 1).isSpace())
                            --from;
                    } else if (ln.issue.contains(QLatin1String("indent"))) {
                        to = 0;
                        while (to < s.size() && (s.at(to) == u' ' || s.at(to) == u'\t'))
                            ++to;
                    }
                    const qreal left = r.left() + layout->position().x();
                    qreal x1 = tl.cursorToX(from), x2 = tl.cursorToX(to);
                    if (x2 - x1 < 1)   // nothing to cover (a blank line): a block the width of a character
                        x2 = x1 + fontMetrics().horizontalAdvance(u' ');
                    p.fillRect(QRectF(left + x1, r.top(), x2 - x1, r.height()), m_c.issue);
                }
            }
        }
    }
    QPlainTextEdit::paintEvent(e);
    {
        QPainter p(viewport());
        paintGroups(p, e->rect());
    }

    if (m_showWs) {   // · for each space, → across each tab, in the muted colour
        QPainter p(viewport());
        p.setFont(font());
        p.setPen(m_c.gutterFg);
        const QPointF off = contentOffset();
        for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
            const QRectF r = blockBoundingGeometry(block).translated(off);
            if (r.top() > e->rect().bottom())
                break;
            QTextLayout *layout = block.layout();
            if (!layout || layout->lineCount() == 0)
                continue;
            const QTextLine tl = layout->lineAt(0);
            const qreal left = r.left() + layout->position().x();
            const QString text = block.text();
            for (int i = 0; i < text.size(); ++i) {
                const QChar ch = text.at(i);
                if (ch != u' ' && ch != u'\t')
                    continue;
                const qreal x1 = left + tl.cursorToX(i), x2 = left + tl.cursorToX(i + 1);
                const QRectF cell(x1, r.top(), x2 - x1, tl.height());
                p.drawText(cell, (ch == u' ' ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter),
                           ch == u' ' ? QStringLiteral("·") : QStringLiteral("→"));
            }
        }
    }
}

void DiffPane::paintGutter(QPaintEvent *e)
{
    QPainter p(m_gutter);
    p.fillRect(e->rect(), m_c.gutterBg);
    p.setFont(font());
    const int w = m_gutter->width();
    const QPointF off = contentOffset();

    for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
        const QRectF r = blockBoundingGeometry(block).translated(off);
        if (r.top() > e->rect().bottom())
            break;
        const int i = block.blockNumber();
        if (i >= m_lines.size())
            break;
        const Line &ln = m_lines.at(i);
        const QRectF row(0, r.top(), w, r.height());
        if (ln.kind == Removed)
            p.fillRect(row, m_c.removed);
        else if (ln.kind == Added)
            p.fillRect(row, m_c.added);
        else if (ln.kind == Empty)
            p.fillRect(row, QBrush(m_c.empty, Qt::BDiagPattern));
        else if (ln.kind == Mine)
            p.fillRect(row, m_c.mine);
        else if (ln.kind == Theirs)
            p.fillRect(row, m_c.theirs);
        else if (ln.kind == Marker)
            p.fillRect(row, m_c.marker);
        p.setPen(ln.kind == Same ? m_c.gutterFg : m_c.fg);
        if (m_dual) {   // old | new
            const qreal half = (w - 10) / 2.0;
            if (ln.number > 0)
                p.drawText(QRectF(0, r.top(), half - 4, r.height()), Qt::AlignRight | Qt::AlignVCenter,
                           QString::number(ln.number));
            if (ln.number2 > 0)
                p.drawText(QRectF(half, r.top(), half, r.height()), Qt::AlignRight | Qt::AlignVCenter,
                           QString::number(ln.number2));
        } else if (ln.number > 0) {
            p.drawText(QRectF(0, r.top(), w - 10, r.height()), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(ln.number));
        }
    }
    p.setPen(m_c.border);
    p.drawLine(w - 1, e->rect().top(), w - 1, e->rect().bottom());
}

// ---------------------------------------------------------------- DiffView

DiffView::DiffView(QWidget *parent) : QWidget(parent)
{
    m_title = new QLabel;
    m_title->setObjectName(QStringLiteral("section"));
    m_title->setMinimumWidth(40);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_stats = new QLabel;
    m_stats->setTextFormat(Qt::RichText);

    m_prev = new QToolButton;
    m_prev->setText(QStringLiteral("↑"));
    m_prev->setToolTip(tr("Previous change (Alt+Up)"));
    m_prev->setShortcut(QKeySequence(QStringLiteral("Alt+Up")));
    m_next = new QToolButton;
    m_next->setText(QStringLiteral("↓"));
    m_next->setToolTip(tr("Next change (Alt+Down)"));
    m_next->setShortcut(QKeySequence(QStringLiteral("Alt+Down")));

    m_modeBtn = new QToolButton;
    m_prefInline = QSettings().value(QStringLiteral("diff/inline"), false).toBool();

    // Whitespace settings, remembered: showing it, and ignoring changes to it.
    m_optsBtn = new QToolButton;
    m_optsBtn->setText(QStringLiteral("⋯"));
    m_optsBtn->setToolTip(tr("Whitespace settings"));
    m_optsBtn->setPopupMode(QToolButton::InstantPopup);
    auto *opts = new QMenu(m_optsBtn);
    m_wsShow = opts->addAction(tr("Show whitespace (spaces ·, tabs →)"));
    m_wsShow->setCheckable(true);
    m_wsShow->setChecked(QSettings().value(QStringLiteral("diff/showWhitespace"), false).toBool());
    m_wsIgnore = opts->addAction(tr("Ignore whitespace changes"));
    m_wsIgnore->setCheckable(true);
    m_wsIgnore->setChecked(QSettings().value(QStringLiteral("diff/ignoreWhitespace"), false).toBool());
    m_optsBtn->setMenu(opts);
    GitRepo::setIgnoreWhitespace(m_wsIgnore->isChecked());
    m_wsNote = new QLabel(tr("whitespace ignored"));
    m_wsNote->setObjectName(QStringLiteral("muted"));
    m_wsNote->setToolTip(tr("Changes that only add, remove or re-indent whitespace are hidden "
                            "(they are still saved and committed). Turn it off in the ⋯ menu."));
    m_wsNote->setVisible(m_wsIgnore->isChecked());
    m_issueNote = new QLabel;
    m_issueNote->setToolTip(tr("Trailing whitespace and similar problems on lines you are adding, by git's "
                               "whitespace rules (what git diff --check reports). Hover a red mark for details."));
    m_issueNote->hide();

    // An SVG is text, so it opens as a diff; this switches to the picture.
    m_imageBtn = new QToolButton;
    m_imageBtn->hide();
    m_svgAsImage = QSettings().value(QStringLiteral("diff/svgAsImage"), false).toBool();

    m_save = new QToolButton;
    m_save->setText(tr("Save"));
    m_save->setToolTip(tr("Write the edited file to disk (Ctrl+S)"));
    m_save->hide();

    m_left = new DiffPane;
    m_right = new DiffPane;
    m_left->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);   // one scrollbar drives both
    // The document is rebuilt on every re-diff, which would wipe Qt's own undo
    // history anyway; undo works on buffer snapshots instead.
    m_right->setUndoRedoEnabled(false);
    m_right->installEventFilter(this);
    m_left->viewport()->installEventFilter(this);    // row clicks, for the resolve window
    m_right->viewport()->installEventFilter(this);

    // Re-diff once typing pauses, not on every keystroke.
    m_rediffTimer = new QTimer(this);
    m_rediffTimer->setSingleShot(true);
    m_rediffTimer->setInterval(300);
    connect(m_rediffTimer, &QTimer::timeout, this, [this] { flushPending(); });
    connect(m_right, &QPlainTextEdit::textChanged, this, [this] { onTyped(); });

    // Each pane can carry a caption (the resolve window names its sides).
    auto wrap = [](QLabel *caption, DiffPane *pane) {
        auto *w = new QWidget;
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(0);
        caption->setObjectName(QStringLiteral("muted"));
        caption->setContentsMargins(10, 4, 10, 4);
        // A long caption is clipped rather than widening its pane.
        caption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        caption->hide();
        l->addWidget(caption);
        l->addWidget(pane, 1);
        return w;
    };
    m_leftCaption = new QLabel;
    m_rightCaption = new QLabel;
    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(wrap(m_leftCaption, m_left));
    split->addWidget(wrap(m_rightCaption, m_right));
    // The two sides start level and stay level as the window resizes (until
    // the divider is dragged).
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);

    m_message = new QLabel;
    m_message->setObjectName(QStringLiteral("muted"));
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);

    // One column: each block's removed lines above its added ones, like git diff.
    // Read-only -- typing needs the two sides lined up.
    m_inline = new DiffPane;
    m_inline->setDualNumbers(true);
    m_inline->installEventFilter(this);
    m_inline->viewport()->installEventFilter(this);
    m_inlineCaption = new QLabel;
    m_inlineCaption->setObjectName(QStringLiteral("muted"));
    m_inlineCaption->setContentsMargins(10, 4, 10, 4);
    m_inlineCaption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_inlineCaption->hide();
    auto *inlinePage = new QWidget;
    auto *il = new QVBoxLayout(inlinePage);
    il->setContentsMargins(0, 0, 0, 0);
    il->setSpacing(0);
    il->addWidget(m_inlineCaption);
    il->addWidget(m_inline, 1);

    m_stack = new QStackedWidget;
    m_stack->addWidget(split);        // 0: side by side
    m_stack->addWidget(m_message);    // 1: a message instead of a diff
    m_stack->addWidget(inlinePage);   // 2: inline
    m_image = new ImageCompare;
    m_stack->addWidget(m_image);      // 3: an image's two versions

    auto *head = new QHBoxLayout;
    head->setContentsMargins(10, 6, 8, 6);
    head->addWidget(m_title, 1);
    head->addWidget(m_wsNote);
    head->addWidget(m_issueNote);
    head->addSpacing(8);
    head->addWidget(m_stats);
    head->addSpacing(10);
    head->addWidget(m_save);
    head->addWidget(m_imageBtn);
    head->addWidget(m_optsBtn);
    head->addWidget(m_modeBtn);
    head->addWidget(m_prev);
    head->addWidget(m_next);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(head);
    lay->addWidget(m_stack, 1);

    // Both panes always have the same number of rows, so scroll values map 1:1.
    auto sync = [](QScrollBar *a, QScrollBar *b) {
        QObject::connect(a, &QScrollBar::valueChanged, b, &QScrollBar::setValue);
        QObject::connect(b, &QScrollBar::valueChanged, a, &QScrollBar::setValue);
    };
    sync(m_left->verticalScrollBar(), m_right->verticalScrollBar());
    // Whether ↑↓ have somewhere to go depends on what is in view.
    for (DiffPane *pane : {m_right, m_inline})
        connect(pane->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { updateNav(); });
    sync(m_left->horizontalScrollBar(), m_right->horizontalScrollBar());

    connect(m_save, &QToolButton::clicked, this, [this] {
        if (onSaveRequested)
            onSaveRequested();
    });
    connect(m_prev, &QToolButton::clicked, this, [this] { prevChange(); });
    connect(m_next, &QToolButton::clicked, this, [this] { nextChange(); });

    // A click always flips what you see: from the automatic inline fallback it
    // means side by side anyway (until there is room again).
    connect(m_modeBtn, &QToolButton::clicked, this, [this] {
        if (inlineWanted()) {   // not the page shown: images and messages have their own
            m_prefInline = false;
            m_forceSplit = m_narrow;
        } else {
            m_prefInline = true;
            m_forceSplit = false;
        }
        QSettings().setValue(QStringLiteral("diff/inline"), m_prefInline);
        updateMode();
    });

    connect(m_imageBtn, &QToolButton::clicked, this, [this] {
        if (m_stack->currentIndex() == 3) {
            m_svgAsImage = false;
            QSettings().setValue(QStringLiteral("diff/svgAsImage"), false);
            const QHash<int, QString> issues = m_issues;
            const QString name = m_name;
            const QByteArray text = m_textDiff;
            const ImageFetch fetch = m_imageFetch;
            showDiff(name, text, m_textEditable, fetch);
            setWhitespaceIssues(issues);
        } else if (!isDirty()) {
            m_textEditable = m_wantEditable;   // as the owner left it after showDiff
            m_svgAsImage = true;
            QSettings().setValue(QStringLiteral("diff/svgAsImage"), true);
            m_issues.clear();
            showImages(m_name);
        }
    });

    connect(m_wsShow, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("diff/showWhitespace"), on);
        for (DiffPane *pane : {m_left, m_right, m_inline})
            pane->setShowWhitespace(on);
        if (onOptionsChanged)
            onOptionsChanged();
    });
    connect(m_wsIgnore, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("diff/ignoreWhitespace"), on);
        GitRepo::setIgnoreWhitespace(on);
        m_wsNote->setVisible(on);
        setEditable(m_wantEditable);
        if (onOptionsChanged)
            onOptionsChanged();
    });
    for (DiffPane *pane : {m_left, m_right, m_inline})
        pane->setShowWhitespace(m_wsShow->isChecked());

    for (DiffPane *pane : {m_left, m_right, m_inline}) {
        pane->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(pane, &QWidget::customContextMenuRequested, this,
                [this, pane](const QPoint &pos) { showContextMenu(pane, pos); });
    }

    applyTheme();
    showMessage({}, tr("Select a file to see its changes"));
    updateMode();   // label the mode button
}

void DiffView::showDiff(const QString &title, const QByteArray &diff, bool editable, const ImageFetch &images)
{
    const ImageFetch fetch = images;   // may be m_imageFetch itself, which showMessage clears
    const bool svg = fetch && title.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive);
    if (svg && m_svgAsImage) {
        m_imageFetch = fetch;
        m_textDiff = diff;
        m_textEditable = editable;
        if (showImages(title))
            return;
    }

    // Re-showing the same file (after a refresh or a save) keeps the scroll
    // position instead of jumping back to the first change.
    const bool sameFile = showingRows() && title == m_name;
    const int keepScroll = activeScrollBar()->value();

    m_issues.clear();
    bool binary = false;
    if (!rowsFromDiff(diff, {}, &binary)) {
        m_imageFetch = fetch;
        if (binary && fetch && showImages(title))
            return;
        showMessage(title, binary                        ? tr("Binary file, no text diff")
                           : GitRepo::ignoreWhitespace() ? tr("Only whitespace changed (whitespace is being ignored)")
                                                         : tr("No content changes"));
        return;
    }
    m_name = title;
    m_stack->setCurrentIndex(inlineWanted() ? 2 : 0);
    updateStats();   // computed while a message may still have been showing
    m_buffer = m_original = rightLines();
    m_undo.clear();
    m_redo.clear();
    m_pending = false;
    m_rediffTimer->stop();
    setEditable(editable);

    m_current = -1;
    updateNav();
    m_imageFetch = fetch;
    m_textDiff = diff;
    updateImageButton();
    QTimer::singleShot(0, this, [this, sameFile, keepScroll] {   // after layout, so the viewport height is known
        if (sameFile)
            activeScrollBar()->setValue(keepScroll);
        else if (!m_changeStarts.isEmpty())
            nextChange();
    });
}

void DiffView::showMessage(const QString &title, const QString &message)
{
    m_name = title;
    m_stats->clear();
    m_message->setText(message);
    m_l.clear();
    m_r.clear();
    m_issues.clear();
    m_issueNote->hide();
    m_buffer.clear();
    m_original.clear();
    m_undo.clear();
    m_redo.clear();
    m_pending = false;
    m_rediffTimer->stop();
    m_rendering = true;
    m_left->setLines({});
    m_right->setLines({});
    m_inline->setLines({});
    m_rendering = false;
    m_stack->setCurrentIndex(1);
    m_changeStarts.clear();
    m_current = -1;
    m_removed = m_added = 0;
    m_imageFetch = nullptr;
    m_textDiff.clear();
    m_lineAction = nullptr;
    setEditable(false);
    updateNav();
}

void DiffView::setLineActions(const QString &verb, std::function<void(const Selection &)> action)
{
    m_lineVerb = verb;
    m_lineAction = std::move(action);
}

DiffView::Selection DiffView::blockAt(int row) const
{
    Selection s;
    if (row < 0 || row >= m_l.size() || !isChanged(row))
        return s;
    int bs = row, be = row + 1;
    while (bs > 0 && isChanged(bs - 1))
        --bs;
    while (be < m_l.size() && isChanged(be))
        ++be;
    for (int i = bs; i < be; ++i) {
        if (m_l.at(i).kind != DiffPane::Empty)
            s.left.insert(i);
        if (m_r.at(i).kind != DiffPane::Empty)
            s.right.insert(i);
    }
    return s;
}

// The selected lines when the pointer is in the selection, else the line under
// it. Side by side a row's old and new line go together (a changed line is the
// pair); inline they are separate lines, picked separately.
DiffView::Selection DiffView::pickedLines(DiffPane *pane, int paneLine) const
{
    Selection s;
    int first = paneLine, last = paneLine;
    const QTextCursor c = pane->textCursor();
    if (c.hasSelection()) {
        const QTextDocument *doc = pane->document();
        const int a = doc->findBlock(c.selectionStart()).blockNumber();
        QTextBlock endBlock = doc->findBlock(c.selectionEnd());
        int b = endBlock.blockNumber();
        if (b > a && c.selectionEnd() == endBlock.position())
            --b;   // a selection ending at a line's very start doesn't take that line
        if (first >= a && first <= b) {
            first = a;
            last = b;
        }
    }
    if (first < 0)
        return s;
    for (int line = first; line <= last; ++line) {
        if (pane == m_inline) {
            const int r = m_inlineToRow.value(line, -1);
            if (r < 0 || line >= m_inline->lineCount() || !isChanged(r))
                continue;
            // Which half of the row this inline line is: its old line carries
            // only the old number.
            const DiffPane::Line &l = m_inline->lineAt(line);
            if (l.kind == DiffPane::Removed)
                s.left.insert(r);
            else if (l.kind == DiffPane::Added)
                s.right.insert(r);
        } else if (line < m_l.size() && isChanged(line)) {
            if (m_l.at(line).kind != DiffPane::Empty)
                s.left.insert(line);
            if (m_r.at(line).kind != DiffPane::Empty)
                s.right.insert(line);
        }
    }
    return s;
}

QStringList DiffView::linesApplied(const Selection &picked, bool toLeft, const QStringList &exactTarget) const
{
    QStringList out;
    auto exact = [&](const DiffPane::Line &l) {
        return l.number >= 1 && l.number <= exactTarget.size() ? exactTarget.at(l.number - 1) : l.text;
    };
    for (int i = 0; i < m_l.size();) {
        if (!isChanged(i)) {
            out << exact(toLeft ? m_l.at(i) : m_r.at(i));
            ++i;
            continue;
        }
        int be = i;
        while (be < m_l.size() && isChanged(be))
            ++be;
        // Within a block the old lines come before the new ones, as in git.
        for (int j = i; j < be; ++j) {
            const DiffPane::Line &l = m_l.at(j);
            if (l.kind == DiffPane::Empty)
                continue;
            const bool pickedHere = picked.left.contains(j);
            if (toLeft ? !pickedHere : pickedHere)   // staging keeps unpicked old lines; unstaging restores picked ones
                out << (toLeft ? exact(l) : l.text);
        }
        for (int j = i; j < be; ++j) {
            const DiffPane::Line &r = m_r.at(j);
            if (r.kind == DiffPane::Empty)
                continue;
            const bool pickedHere = picked.right.contains(j);
            if (toLeft ? pickedHere : !pickedHere)   // staging adds picked new lines; unstaging keeps unpicked ones
                out << (toLeft ? r.text : exact(r));
        }
        i = be;
    }
    return out;
}

// The file's two versions as pictures, when at least one of them is an image.
bool DiffView::showImages(const QString &title)
{
    const ImageFetch fetch = m_imageFetch;
    const QByteArray textDiff = m_textDiff;
    if (!fetch)
        return false;
    const ImageSides sides = fetch();
    const QString leftName = m_leftCaption->text().isEmpty() ? tr("Before") : m_leftCaption->text();
    const QString rightName = m_rightCaption->text().isEmpty() ? tr("After") : m_rightCaption->text();
    const ImageCompare::Side left = ImageCompare::load(leftName, sides.before, sides.hasBefore);
    const ImageCompare::Side right = ImageCompare::load(rightName, sides.after, sides.hasAfter);
    if (left.image.isNull() && right.image.isNull())
        return false;
    showMessage(title, {});
    m_imageFetch = fetch;
    m_textDiff = textDiff;
    m_image->setSides(left, right);
    m_image->setStacked(inlineWanted());
    m_stack->setCurrentIndex(3);
    updateImageButton();
    return true;
}

// Shown for an SVG only: its text and its picture are both worth seeing.
void DiffView::updateImageButton()
{
    const bool onImage = m_stack->currentIndex() == 3;
    const bool svg = m_imageFetch && m_name.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive);
    m_imageBtn->setVisible(svg && (onImage || showingRows()));
    m_imageBtn->setText(onImage ? tr("Source") : tr("Picture"));
    m_imageBtn->setToolTip(onImage ? tr("Show the SVG's source as a diff") : tr("Show the SVG as a picture"));
    const bool dirty = isDirty();
    m_imageBtn->setEnabled(onImage || !dirty);
    if (!onImage && dirty)
        m_imageBtn->setToolTip(tr("Save or discard your edits first"));
}

// Parses a unified diff into the panes. When the diff has no hunks -- the
// edited text is identical to the left -- `fallback` is shown as unchanged
// lines instead, so an edit that undoes every change still leaves the file on
// screen. Returns false when there is nothing to show.
bool DiffView::rowsFromDiff(const QByteArray &diff, const QStringList &fallback, bool *binary)
{
    QVector<Row> rows = parseUnified(diff, binary);
    if (*binary)
        return false;
    if (rows.isEmpty()) {
        if (fallback.isEmpty())
            return false;
        for (int i = 0; i < fallback.size(); ++i) {
            Row r;
            r.l = r.r = fallback.at(i);
            r.ln = r.rn = i + 1;
            rows << r;
        }
    }

    QVector<DiffPane::Line> left, right;
    left.reserve(rows.size());
    right.reserve(rows.size());
    for (const Row &r : rows) {
        DiffPane::Line a{r.l, r.ln, r.lk};
        DiffPane::Line b{r.r, r.rn, r.rk};
        if (r.lk == DiffPane::Removed && r.rk == DiffPane::Added)
            inlineRange(r.l, r.r, a.hlStart, a.hlEnd, b.hlStart, b.hlEnd);
        left << a;
        right << b;
    }
    setRows(left, right);
    return true;
}

void DiffView::setRows(const QVector<DiffPane::Line> &left, const QVector<DiffPane::Line> &right)
{
    m_l = left;
    m_r = right;
    m_changeStarts.clear();
    m_removed = m_added = 0;
    bool prevChanged = false;
    for (int i = 0; i < m_l.size(); ++i) {
        if (m_l.at(i).kind == DiffPane::Removed) ++m_removed;
        if (m_r.at(i).kind == DiffPane::Added) ++m_added;
        const bool changed = isChanged(i);
        if (changed && !prevChanged)
            m_changeStarts << i;
        prevChanged = changed;
    }
    for (DiffPane::Line &l : m_r)
        l.issue = l.kind == DiffPane::Empty ? QString() : m_issues.value(l.number);
    m_rendering = true;
    m_left->setLines(m_l);
    m_right->setLines(m_r);
    m_rendering = false;
    buildInline();
    updateStats();
    updateHeader();
}

void DiffView::applyTheme()
{
    const ThemeColors &t = Theme::instance().colors();
    const qreal soft = t.light ? 0.16 : 0.20;
    const qreal strong = t.light ? 0.36 : 0.45;
    DiffColors c;
    c.bg = t.background;
    c.fg = t.foreground;
    c.gutterBg = t.background;
    c.gutterFg = t.muted;
    c.border = t.border;
    const QColor left = m_leftTint.isValid() ? m_leftTint : t.red;
    const QColor right = m_rightTint.isValid() ? m_rightTint : t.green;
    c.removed = Theme::mix(t.background, left, soft);
    c.removedStrong = Theme::mix(t.background, left, strong);
    c.added = Theme::mix(t.background, right, soft);
    c.addedStrong = Theme::mix(t.background, right, strong);
    c.empty = t.border;
    c.issue = Theme::mix(t.background, t.red, 0.8);
    c.frame = t.accent;
    c.frameIdle = t.muted;
    c.automatic = Theme::mix(t.background, t.muted, t.light ? 0.08 : 0.10);
    m_issueNote->setStyleSheet(QStringLiteral("color: %1;").arg(t.red.name()));
    m_left->setColors(c);
    m_right->setColors(c);
    m_inline->setColors(c);
    updateMode();   // the mode icon's fallback colour follows the theme
    updateStats();
    highlightRows();   // the syntax colours are the theme's too
}

void DiffView::updateStats()
{
    if (!showingRows()) {
        m_stats->clear();
        return;
    }
    const ThemeColors &t = Theme::instance().colors();
    m_stats->setText(QStringLiteral("<span style=\"color:%1\">−%2</span>&nbsp;&nbsp;<span style=\"color:%3\">+%4</span>")
                         .arg(t.red.name()).arg(m_removed).arg(t.green.name()).arg(m_added));
}

void DiffView::gotoRow(int row)
{
    const bool inl = m_stack->currentIndex() == 2;
    DiffPane *pane = inl ? m_inline : m_right;
    const int line = inl ? m_rowToInline.value(row, 0) : row;
    const int lineH = qMax(1, pane->fontMetrics().lineSpacing());
    const int visible = qMax(1, pane->viewport()->height() / lineH);
    pane->verticalScrollBar()->setValue(qMax(0, line - visible / 4));
}

bool DiffView::rowInView(int row) const
{
    const bool inl = m_stack->currentIndex() == 2;
    const DiffPane *pane = inl ? m_inline : m_right;
    const int line = inl ? m_rowToInline.value(row, 0) : row;
    const int top = pane->verticalScrollBar()->value();
    const int visible = qMax(1, pane->viewport()->height() / qMax(1, pane->fontMetrics().lineSpacing()));
    return line >= top && line < top + visible;
}

// The current change scrolled out of sight: back to it first. That is the
// only move there is when the file has a single change.
bool DiffView::currentAway() const
{
    return m_current >= 0 && m_current < m_changeStarts.size() && !rowInView(m_changeStarts.at(m_current));
}

void DiffView::nextChange()
{
    if (m_changeStarts.isEmpty())
        return;
    if (!currentAway())
        m_current = qMin(int(m_changeStarts.size()) - 1, m_current + 1);
    gotoRow(m_changeStarts.at(m_current));
    updateNav();
}

void DiffView::prevChange()
{
    if (m_changeStarts.isEmpty())
        return;
    if (!currentAway())
        m_current = qMax(0, m_current - 1);
    gotoRow(m_changeStarts.at(m_current));
    updateNav();
}

void DiffView::updateNav()
{
    const bool any = !m_changeStarts.isEmpty() && showingRows();
    const bool away = any && currentAway();
    m_prev->setEnabled(any && (m_current > 0 || away));
    m_next->setEnabled(any && (m_current < int(m_changeStarts.size()) - 1 || away));
}

// ---------------------------------------------------------------- editing the right side

void DiffView::setEditable(bool editable)
{
    // Ignoring whitespace changes what is shown, not what is edited: the right
    // side is still the file exactly (git takes unchanged-looking lines from
    // the new side), and a changed block's left lines are the old ones.
    m_wantEditable = editable;
    m_editable = editable && showingRows();
    m_right->setReadOnly(!m_editable);
    updateHeader();
}

bool DiffView::isDirty() const
{
    return m_pending || m_buffer != m_original;
}

void DiffView::updateHeader()
{
    const bool dirty = isDirty();
    m_title->setText(dirty ? QStringLiteral("● ") + m_name : m_name);
    m_save->setVisible(dirty);
    m_wsIgnore->setEnabled(!dirty);
    m_wsIgnore->setToolTip(dirty ? tr("Save or discard your edits first") : QString());
    updateImageButton();
}

bool DiffView::isChanged(int row) const
{
    return m_l.at(row).kind != DiffPane::Same || m_r.at(row).kind != DiffPane::Same;
}

// The right pane read back as the file: every line except fillers nobody
// typed into.
QStringList DiffView::editorLines() const
{
    QStringList out;
    for (QTextBlock b = m_right->document()->begin(); b.isValid(); b = b.next()) {
        if (b.userState() == DiffPane::FillerState && b.text().isEmpty())
            continue;
        out << b.text();
    }
    return out;
}

void DiffView::onTyped()
{
    if (m_rendering || !m_editable)
        return;
    if (!m_pending) {
        // First keystroke of a burst: the whole burst is one undo step.
        m_undo << m_buffer;
        m_redo.clear();
        m_pending = true;
    }
    updateHeader();
    m_rediffTimer->start();
}

void DiffView::flushPending()
{
    m_rediffTimer->stop();
    if (!m_pending)
        return;
    m_pending = false;
    const QStringList typed = editorLines();
    if (typed == m_buffer) {
        m_undo.removeLast();   // typed and deleted again: not a step
        updateHeader();
        return;
    }
    applyBuffer(typed);
}

// Re-diffs `buffer` against the left side and redraws, keeping the caret on
// the same line of the file and the view where it was.
void DiffView::applyBuffer(const QStringList &buffer)
{
    const QTextCursor cur = m_right->textCursor();
    const QTextBlock curBlock = cur.block();
    const bool onFiller = curBlock.userState() == DiffPane::FillerState && curBlock.text().isEmpty();
    int fileLine = 0;
    for (QTextBlock b = m_right->document()->begin(); b.isValid() && b != curBlock; b = b.next())
        if (!(b.userState() == DiffPane::FillerState && b.text().isEmpty()))
            ++fileLine;
    const int column = onFiller ? 0 : cur.positionInBlock();
    const int scroll = activeScrollBar()->value();

    m_buffer = buffer;
    bool binary = false;
    rowsFromDiff(rediff ? rediff(buffer) : QByteArray(), buffer, &binary);

    int row = -1, seen = 0;
    for (int i = 0; i < m_r.size() && row < 0; ++i)
        if (m_r.at(i).kind != DiffPane::Empty && seen++ == fileLine)
            row = i;
    if (row < 0)
        row = int(m_r.size()) - 1;
    if (row >= 0) {
        const QTextBlock b = m_right->document()->findBlockByNumber(row);
        QTextCursor c(b);
        c.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, qMin(column, b.length() - 1));
        m_rendering = true;
        m_right->setTextCursor(c);
        m_rendering = false;
    }
    activeScrollBar()->setValue(scroll);
    m_current = -1;
    updateNav();
    updateHeader();
}

void DiffView::take(Take how, int row)
{
    if (!m_editable)
        return;
    flushPending();
    QStringList next;
    if (how == Take::WholeFile && leftFile) {
        next = linesOf(leftFile());   // exact, whitespace and all
    } else if (how == Take::WholeFile) {
        for (const DiffPane::Line &l : m_l)
            if (l.kind != DiffPane::Empty)
                next << l.text;
    } else {
        next = resultOf(how, row);
    }
    if (next == m_buffer)
        return;
    m_undo << m_buffer;
    m_redo.clear();
    applyBuffer(next);
}

void DiffView::undo()
{
    flushPending();
    if (m_undo.isEmpty())
        return;
    m_redo << m_buffer;
    applyBuffer(m_undo.takeLast());
}

void DiffView::redo()
{
    flushPending();
    if (m_redo.isEmpty())
        return;
    m_undo << m_buffer;
    applyBuffer(m_redo.takeLast());
}

bool DiffView::save()
{
    flushPending();
    if (!isDirty())
        return true;
    if (!onSave || !onSave(m_buffer))
        return false;
    m_original = m_buffer;
    updateHeader();
    return true;
}

void DiffView::discard()
{
    m_rediffTimer->stop();
    m_pending = false;
    m_buffer = m_original;
    m_undo.clear();
    m_redo.clear();
    updateHeader();
}

void DiffView::setConflictRows(const QVector<DiffPane::Group> &groups)
{
    m_conflictRows = groups;
    applyConflictRows();
}

// The frames in rows for the two panes, and in lines for the inline view: a
// group there is every inline line that came from its rows.
void DiffView::applyConflictRows()
{
    const bool on = !m_conflictRows.isEmpty();
    m_left->setGroups(m_conflictRows, on);
    m_right->setGroups(m_conflictRows, on);
    QVector<DiffPane::Group> lines;
    for (DiffPane::Group g : m_conflictRows) {
        int first = -1, last = -1;
        for (int i = 0; i < m_inlineToRow.size(); ++i)
            if (m_inlineToRow.at(i) >= g.first && m_inlineToRow.at(i) <= g.last) {
                first = first < 0 ? i : first;
                last = i;
            }
        if (first < 0)
            continue;
        g.first = first;
        g.last = last;
        lines << g;
    }
    m_inline->setGroups(lines, on);
}

bool DiffView::eventFilter(QObject *watched, QEvent *event)
{
    if (onRowClicked
        && (watched == m_left->viewport() || watched == m_right->viewport() || watched == m_inline->viewport())
        && (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick)) {
        auto *me = static_cast<QMouseEvent *>(event);
        DiffPane *pane = watched == m_left->viewport() ? m_left : watched == m_right->viewport() ? m_right : m_inline;
        // A click, not the end of a drag that selected text.
        if (me->button() == Qt::LeftButton && !pane->textCursor().hasSelection()) {
            int row = pane->cursorForPosition(me->position().toPoint()).blockNumber();
            bool right = pane == m_right;
            if (pane == m_inline) {   // back to the row, and which side of it this line is
                right = row < m_inline->lineCount() && m_inline->lineAt(row).kind != DiffPane::Removed;
                row = m_inlineToRow.value(row, -1);
            }
            const bool dbl = event->type() == QEvent::MouseButtonDblClick;
            QTimer::singleShot(0, this, [this, row, right, dbl] {
                if (onRowClicked)
                    onRowClicked(row, right, dbl);
            });
        }
    }
    // The editor would otherwise take these for its own (disabled) undo.
    if ((watched == m_right || watched == m_inline) && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->matches(QKeySequence::Undo)) { undo(); return true; }
        if (ke->matches(QKeySequence::Redo)) { redo(); return true; }
    }
    return QWidget::eventFilter(watched, event);
}

// The right side's lines after taking `how` at `row`. The panes hold the whole
// file (the diff is generated with unlimited context), so rebuilding the right
// side row by row is the complete new file.
QStringList DiffView::resultOf(Take how, int row) const
{
    QStringList out;
    if (how == Take::WholeFile || row < 0 || row >= m_l.size())
        return out;

    // The block is the run of changed rows around `row`.
    int bs = row, be = row + 1;
    if (how != Take::Line) {
        while (bs > 0 && isChanged(bs - 1))
            --bs;
        while (be < m_l.size() && isChanged(be))
            ++be;
    }

    for (int i = 0; i < m_l.size(); ++i) {
        if (i == bs) {
            for (int j = bs; j < be; ++j)
                if (m_l.at(j).kind != DiffPane::Empty)
                    out << m_l.at(j).text;
            if (how == Take::LeftBeforeRight)
                for (int j = bs; j < be; ++j)
                    if (m_r.at(j).kind != DiffPane::Empty)
                        out << m_r.at(j).text;
            i = be - 1;
            continue;
        }
        if (m_r.at(i).kind != DiffPane::Empty)
            out << m_r.at(i).text;
    }
    return out;
}

QStringList DiffView::rightLines() const
{
    QStringList out;
    for (const DiffPane::Line &l : m_r)
        if (l.kind != DiffPane::Empty)
            out << l.text;
    return out;
}

QStringList DiffView::linesOf(const QByteArray &data)
{
    const QString s = QString::fromUtf8(data);
    if (s.isEmpty())
        return {};
    QStringList lines = s.split(u'\n');
    if (s.endsWith(u'\n'))
        lines.removeLast();
    for (QString &l : lines)
        l = stripCr(l);
    return lines;
}

QByteArray DiffView::compose(const QStringList &lines, const QByteArray &original)
{
    const QString eol = original.contains("\r\n") ? QStringLiteral("\r\n") : QStringLiteral("\n");
    QByteArray out = lines.join(eol).toUtf8();
    if (!lines.isEmpty() && original.endsWith('\n'))
        out += eol.toUtf8();
    return out;
}

void DiffView::showContextMenu(DiffPane *pane, const QPoint &pos)
{
    // Settle any typing first, so the row under the pointer means what it shows.
    flushPending();

    QMenu *menu = pane->createStandardContextMenu(pos);
    // Qt's own undo/redo entries drive the editor's history, which is off here.
    for (QAction *a : menu->actions())
        if (a->objectName() == u"edit-undo" || a->objectName() == u"edit-redo")
            menu->removeAction(a);
    QAction *before = menu->actions().isEmpty() ? nullptr : menu->actions().constFirst();

    const bool showing = showingRows();
    const int paneLine = pane->cursorForPosition(pos).blockNumber();
    int row = paneLine;
    if (pane == m_inline)
        row = m_inlineToRow.value(row, -1);   // back to the aligned row it came from
    const bool onChange = showing && row >= 0 && row < m_l.size() && isChanged(row);

    auto add = [&](const QString &text, bool enabled, auto fn) {
        auto *a = new QAction(text, menu);
        a->setEnabled(enabled);
        connect(a, &QAction::triggered, this, fn);
        menu->insertAction(before, a);
        return a;
    };
    if (extendMenu && showing && row >= 0) {
        const int sep = int(menu->actions().size());
        const bool rightSide = pane == m_right
            || (pane == m_inline && paneLine < m_inline->lineCount() && m_inline->lineAt(paneLine).kind != DiffPane::Removed);
        extendMenu(menu, before, row, rightSide);
        if (int(menu->actions().size()) > sep)
            menu->insertSeparator(before);
    }
    if (m_lineAction) {
        const bool dirty = isDirty();
        const QString later = dirty ? tr(" (save your edits first)") : QString();
        auto run = [this](const Selection &s) {
            const auto action = m_lineAction;   // it may replace itself by re-showing the file
            action(s);
        };
        const Selection block = showing ? blockAt(row) : Selection();
        const Selection lines = showing ? pickedLines(pane, paneLine) : Selection();
        const int n = int(lines.left.size() + lines.right.size());
        add(tr("%1 block").arg(m_lineVerb) + later, !block.isEmpty() && !dirty, [run, block] { run(block); });
        const bool several = pane->textCursor().hasSelection() && n > 1;
        add((several ? tr("%1 selected lines").arg(m_lineVerb) : tr("%1 line").arg(m_lineVerb)) + later,
            n > 0 && !dirty, [run, lines] { run(lines); });
        menu->insertSeparator(before);
    }
    // A view with nowhere to save edits (history in the log) has nothing to offer here.
    if (!onSave) {
        menu->exec(pane->viewport()->mapToGlobal(pos));
        delete menu;
        return;
    }
    add(tr("Use left text block"), m_editable && onChange, [this, row] { take(Take::Block, row); });
    add(tr("Use left line"), m_editable && onChange, [this, row] { take(Take::Line, row); });
    add(tr("Use text block from left before right"), m_editable && onChange,
        [this, row] { take(Take::LeftBeforeRight, row); });
    add(tr("Use left whole file"), m_editable && showing, [this] { take(Take::WholeFile, 0); });
    menu->insertSeparator(before);
    add(tr("Undo"), !m_undo.isEmpty(), [this] { undo(); })->setShortcut(QKeySequence::Undo);
    add(tr("Redo"), !m_redo.isEmpty(), [this] { redo(); })->setShortcut(QKeySequence::Redo);
    add(tr("Save"), isDirty(), [this] {
        if (onSaveRequested)
            onSaveRequested();
    })->setShortcut(QKeySequence::Save);
    if (before)
        menu->insertSeparator(before);

    menu->exec(pane->viewport()->mapToGlobal(pos));
    delete menu;
}

// ---------------------------------------------------------------- resolve window support

void DiffView::setPaneCaptions(const QString &left, const QString &right)
{
    m_inlineCaption->setText(left.isEmpty() && right.isEmpty() ? QString()
                             : tr("%1   →   %2").arg(left, right));
    m_inlineCaption->setToolTip(m_inlineCaption->text());
    m_inlineCaption->setVisible(!left.isEmpty() || !right.isEmpty());
    m_leftCaption->setText(left);
    m_rightCaption->setText(right);
    m_leftCaption->setToolTip(left);
    m_rightCaption->setToolTip(right);
    m_leftCaption->setVisible(!left.isEmpty());
    m_rightCaption->setVisible(!right.isEmpty());
}

void DiffView::setSideTints(const QColor &left, const QColor &right)
{
    m_leftTint = left;
    m_rightTint = right;
    applyTheme();
}

void DiffView::setNavShortcutsEnabled(bool enabled)
{
    m_prev->setShortcut(enabled ? QKeySequence(QStringLiteral("Alt+Up")) : QKeySequence());
    m_next->setShortcut(enabled ? QKeySequence(QStringLiteral("Alt+Down")) : QKeySequence());
}

int DiffView::rowForLine(bool right, int lineNumber) const
{
    const QVector<DiffPane::Line> &rows = right ? m_r : m_l;
    for (int i = 0; i < rows.size(); ++i)
        if (rows.at(i).number == lineNumber)
            return i;
    return -1;
}

void DiffView::revealRow(int row)
{
    if (row >= 0)
        gotoRow(row);
}

// ---------------------------------------------------------------- side by side / inline

bool DiffView::showingRows() const
{
    return m_stack->currentIndex() == 0 || m_stack->currentIndex() == 2;
}

bool DiffView::inlineWanted() const
{
    return m_prefInline || (m_narrow && !m_forceSplit);
}

QScrollBar *DiffView::activeScrollBar() const
{
    return (m_stack->currentIndex() == 2 ? m_inline : m_right)->verticalScrollBar();
}

// The inline view drawn from the aligned rows: unchanged lines once, each run
// of changes as its removed lines and then its added ones. Both directions of
// the row <-> line mapping are kept, for scrolling and the context menu.
void DiffView::buildInline()
{
    QVector<DiffPane::Line> lines;
    m_inlineToRow.clear();
    m_rowToInline.assign(m_l.size(), 0);
    for (int i = 0; i < m_l.size();) {
        if (!isChanged(i)) {
            DiffPane::Line l = m_r.at(i);
            l.number = m_l.at(i).number;
            l.number2 = m_r.at(i).number;
            m_rowToInline[i] = int(lines.size());
            lines << l;
            m_inlineToRow << i;
            ++i;
            continue;
        }
        int end = i;
        while (end < m_l.size() && isChanged(end))
            ++end;
        for (int j = i; j < end; ++j)
            m_rowToInline[j] = int(lines.size());
        for (int j = i; j < end; ++j)
            if (m_l.at(j).kind != DiffPane::Empty) {
                DiffPane::Line l = m_l.at(j);   // old number only
                lines << l;
                m_inlineToRow << j;
            }
        for (int j = i; j < end; ++j)
            if (m_r.at(j).kind != DiffPane::Empty) {
                DiffPane::Line l = m_r.at(j);
                l.number2 = l.number;           // new number only
                l.number = -1;
                lines << l;
                m_inlineToRow << j;
            }
        i = end;
    }
    m_inline->setLines(lines);
    applyConflictRows();
    highlightRows();
}

// Syntax colours stop at a line's highlighted change: over the stronger
// background there, a coloured token can all but vanish, and plain text also
// makes the change itself stand out.
static Syntax::Spans outsideChange(const Syntax::Spans &spans, const DiffPane::Line &l)
{
    if (l.hlEnd <= l.hlStart || l.hlStart < 0)
        return spans;
    Syntax::Spans out;
    for (const QTextLayout::FormatRange &s : spans) {
        const int end = s.start + s.length;
        if (s.start < l.hlStart)
            out << QTextLayout::FormatRange{s.start, qMin(end, l.hlStart) - s.start, s.format};
        if (end > l.hlEnd)
            out << QTextLayout::FormatRange{qMax(s.start, l.hlEnd), end - qMax(s.start, l.hlEnd), s.format};
    }
    return out;
}

// Each side's own file is highlighted -- its lines in order, without the
// filler rows -- and the colours laid onto the rows it shows in. Inline, a
// removed line takes the old file's colours and every other line the new's,
// so interleaving the two never confuses a string or a comment.
void DiffView::highlightRows()
{
    QStringList left, right;
    QVector<int> leftAt(m_l.size(), -1), rightAt(m_r.size(), -1);
    for (int i = 0; i < m_l.size(); ++i) {
        if (m_l.at(i).kind != DiffPane::Empty) {
            leftAt[i] = int(left.size());
            left << m_l.at(i).text;
        }
        if (m_r.at(i).kind != DiffPane::Empty) {
            rightAt[i] = int(right.size());
            right << m_r.at(i).text;
        }
    }
    const QVector<Syntax::Spans> ls = Syntax::highlight(m_fileName, left);
    const QVector<Syntax::Spans> rs = Syntax::highlight(m_fileName, right);
    QVector<Syntax::Spans> lp(m_l.size()), rp(m_r.size()), ip;
    for (int i = 0; i < m_l.size(); ++i) {
        lp[i] = outsideChange(ls.value(leftAt.at(i)), m_l.at(i));
        rp[i] = outsideChange(rs.value(rightAt.at(i)), m_r.at(i));
    }
    for (int j = 0; j < m_inline->lineCount(); ++j) {
        const int row = m_inlineToRow.value(j, -1);
        ip << (m_inline->lineAt(j).kind == DiffPane::Removed ? lp.value(row) : rp.value(row));
    }
    const bool rendering = m_rendering;
    m_rendering = true;   // not typing
    m_left->setSyntax(ls.isEmpty() ? QVector<Syntax::Spans>() : lp);
    m_right->setSyntax(rs.isEmpty() ? QVector<Syntax::Spans>() : rp);
    m_inline->setSyntax(ls.isEmpty() && rs.isEmpty() ? QVector<Syntax::Spans>() : ip);
    m_rendering = rendering;
}

// Shows whichever view is wanted, keeping the same part of the file in view.
void DiffView::updateMode()
{
    const bool inl = inlineWanted();
    const bool automatic = inl && !m_prefInline;
    // An icon for the view being shown; in the accent colour when the narrow
    // fallback chose it rather than you.
    m_modeBtn->setText(inl ? QStringLiteral("☰") : QStringLiteral("◫"));
    m_modeBtn->setStyleSheet(automatic ? QStringLiteral("QToolButton { color: %1; }")
                                             .arg(Theme::instance().colors().accent.name())
                                       : QString());
    m_modeBtn->setToolTip(automatic ? tr("Inline, because the pane is too narrow for two sides (read-only).\n"
                                         "Click for side by side anyway.")
                          : inl     ? tr("Inline (read-only). Click for side by side.")
                                    : tr("Side by side. Click for inline."));
    m_image->setStacked(inl);   // images: one above the other instead of inline
    if (!showingRows() || (m_stack->currentIndex() == 2) == inl)
        return;
    const int topLine = activeScrollBar()->value();
    const int topRow = m_stack->currentIndex() == 2 ? m_inlineToRow.value(topLine, 0) : topLine;
    m_stack->setCurrentIndex(inl ? 2 : 0);
    activeScrollBar()->setValue(inl ? m_rowToInline.value(topRow, 0) : topRow);
}

// Narrow means fewer than ~60 characters per side. The band between 60 and
// 64 keeps it from flickering while a divider is dragged across the line.
void DiffView::updateNarrow()
{
    const int charW = qMax(1, m_right->fontMetrics().horizontalAdvance(u'm'));
    const int side = m_stack->width() / 2 - m_right->gutterWidth() - 8;
    const int columns = side / charW;
    const bool narrow = m_narrow ? columns < 64 : columns < 60;
    if (!narrow)
        m_forceSplit = false;
    m_narrow = narrow;
}

void DiffView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateNarrow();
    updateMode();
}

bool DiffView::showWhitespace() const
{
    return m_wsShow->isChecked();
}

void DiffView::setWhitespaceIssues(const QHash<int, QString> &byLine)
{
    m_issues = byLine;
    applyIssues();
}

// Marks the rows on the right (and so the inline view) and counts them up.
void DiffView::applyIssues()
{
    for (DiffPane::Line &l : m_r)
        l.issue = l.kind == DiffPane::Empty ? QString() : m_issues.value(l.number);
    m_right->setKinds(m_r);
    buildInline();
    const int n = int(m_issues.size());
    m_issueNote->setText(n == 1 ? tr("1 whitespace issue") : tr("%1 whitespace issues").arg(n));
    m_issueNote->setVisible(n > 0 && showingRows());
}
