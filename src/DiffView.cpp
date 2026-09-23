#include "DiffView.h"
#include "Theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextLayout>
#include <QTimer>
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
    updateGutterWidth();
    viewport()->update();
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
        maxNo = qMax(maxNo, l.number);
    const int digits = qMax(3, int(QString::number(maxNo).size()));
    return fontMetrics().horizontalAdvance(u'9') * digits + 18;
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
            switch (ln.kind) {
            case Removed: p.fillRect(full, m_c.removed); break;
            case Added:   p.fillRect(full, m_c.added); break;
            case Empty:   p.fillRect(full, QBrush(m_c.empty, Qt::BDiagPattern)); break;
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
        }
    }
    QPlainTextEdit::paintEvent(e);
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
        if (ln.number > 0) {
            p.setPen(ln.kind == Same ? m_c.gutterFg : m_c.fg);
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

    // Re-diff once typing pauses, not on every keystroke.
    m_rediffTimer = new QTimer(this);
    m_rediffTimer->setSingleShot(true);
    m_rediffTimer->setInterval(300);
    connect(m_rediffTimer, &QTimer::timeout, this, [this] { flushPending(); });
    connect(m_right, &QPlainTextEdit::textChanged, this, [this] { onTyped(); });

    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(m_left);
    split->addWidget(m_right);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);

    m_message = new QLabel;
    m_message->setObjectName(QStringLiteral("muted"));
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);

    m_stack = new QStackedWidget;
    m_stack->addWidget(split);
    m_stack->addWidget(m_message);

    auto *head = new QHBoxLayout;
    head->setContentsMargins(10, 6, 8, 6);
    head->addWidget(m_title, 1);
    head->addWidget(m_stats);
    head->addSpacing(10);
    head->addWidget(m_save);
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
    sync(m_left->horizontalScrollBar(), m_right->horizontalScrollBar());

    connect(m_save, &QToolButton::clicked, this, [this] {
        if (onSaveRequested)
            onSaveRequested();
    });
    connect(m_prev, &QToolButton::clicked, this, [this] { prevChange(); });
    connect(m_next, &QToolButton::clicked, this, [this] { nextChange(); });

    for (DiffPane *pane : {m_left, m_right}) {
        pane->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(pane, &QWidget::customContextMenuRequested, this,
                [this, pane](const QPoint &pos) { showContextMenu(pane, pos); });
    }

    applyTheme();
    showMessage({}, tr("Select a file to see its changes"));
}

void DiffView::showDiff(const QString &title, const QByteArray &diff, bool editable)
{
    // Re-showing the same file (after a refresh or a save) keeps the scroll
    // position instead of jumping back to the first change.
    const bool sameFile = m_stack->currentIndex() == 0 && title == m_name;
    const int keepScroll = m_right->verticalScrollBar()->value();

    bool binary = false;
    if (!rowsFromDiff(diff, {}, &binary)) {
        showMessage(title, binary ? tr("Binary file, no text diff") : tr("No content changes"));
        return;
    }
    m_name = title;
    m_stack->setCurrentIndex(0);
    m_buffer = m_original = rightLines();
    m_undo.clear();
    m_redo.clear();
    m_pending = false;
    m_rediffTimer->stop();
    setEditable(editable);

    m_current = -1;
    updateNav();
    QTimer::singleShot(0, this, [this, sameFile, keepScroll] {   // after layout, so the viewport height is known
        if (sameFile)
            m_right->verticalScrollBar()->setValue(keepScroll);
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
    m_buffer.clear();
    m_original.clear();
    m_undo.clear();
    m_redo.clear();
    m_pending = false;
    m_rediffTimer->stop();
    m_rendering = true;
    m_left->setLines({});
    m_right->setLines({});
    m_rendering = false;
    m_stack->setCurrentIndex(1);
    m_changeStarts.clear();
    m_current = -1;
    m_removed = m_added = 0;
    setEditable(false);
    updateNav();
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
    m_rendering = true;
    m_left->setLines(m_l);
    m_right->setLines(m_r);
    m_rendering = false;
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
    c.removed = Theme::mix(t.background, t.red, soft);
    c.removedStrong = Theme::mix(t.background, t.red, strong);
    c.added = Theme::mix(t.background, t.green, soft);
    c.addedStrong = Theme::mix(t.background, t.green, strong);
    c.empty = t.border;
    m_left->setColors(c);
    m_right->setColors(c);
    updateStats();
}

void DiffView::updateStats()
{
    if (m_stack->currentIndex() != 0) {
        m_stats->clear();
        return;
    }
    const ThemeColors &t = Theme::instance().colors();
    m_stats->setText(QStringLiteral("<span style=\"color:%1\">−%2</span>&nbsp;&nbsp;<span style=\"color:%3\">+%4</span>")
                         .arg(t.red.name()).arg(m_removed).arg(t.green.name()).arg(m_added));
}

void DiffView::gotoRow(int row)
{
    const int lineH = qMax(1, m_right->fontMetrics().lineSpacing());
    const int visible = qMax(1, m_right->viewport()->height() / lineH);
    m_right->verticalScrollBar()->setValue(qMax(0, row - visible / 4));
}

void DiffView::nextChange()
{
    if (m_changeStarts.isEmpty())
        return;
    m_current = qMin(int(m_changeStarts.size()) - 1, m_current + 1);
    gotoRow(m_changeStarts.at(m_current));
    updateNav();
}

void DiffView::prevChange()
{
    if (m_changeStarts.isEmpty())
        return;
    m_current = qMax(0, m_current - 1);
    gotoRow(m_changeStarts.at(m_current));
    updateNav();
}

void DiffView::updateNav()
{
    m_prev->setEnabled(m_current > 0);
    m_next->setEnabled(!m_changeStarts.isEmpty() && m_current < int(m_changeStarts.size()) - 1);
}

// ---------------------------------------------------------------- editing the right side

void DiffView::setEditable(bool editable)
{
    m_editable = editable && m_stack->currentIndex() == 0;
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
    const int scroll = m_right->verticalScrollBar()->value();

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
    m_right->verticalScrollBar()->setValue(scroll);
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
    if (how == Take::WholeFile) {
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

bool DiffView::eventFilter(QObject *watched, QEvent *event)
{
    // The editor would otherwise take these for its own (disabled) undo.
    if (watched == m_right && event->type() == QEvent::KeyPress) {
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

    const bool showing = m_stack->currentIndex() == 0;
    const int row = pane->cursorForPosition(pos).blockNumber();
    const bool onChange = showing && row >= 0 && row < m_l.size() && isChanged(row);

    auto add = [&](const QString &text, bool enabled, auto fn) {
        auto *a = new QAction(text, menu);
        a->setEnabled(enabled);
        connect(a, &QAction::triggered, this, fn);
        menu->insertAction(before, a);
        return a;
    };
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
