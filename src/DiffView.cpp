#include "DiffView.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
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

    m_left = new DiffPane;
    m_right = new DiffPane;
    m_left->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);   // one scrollbar drives both

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

    connect(m_prev, &QToolButton::clicked, this, [this] { prevChange(); });
    connect(m_next, &QToolButton::clicked, this, [this] { nextChange(); });

    applyTheme();
    showMessage({}, tr("Select a file to see its changes"));
}

void DiffView::showDiff(const QString &title, const QByteArray &diff)
{
    bool binary = false;
    const QVector<Row> rows = parseUnified(diff, &binary);
    if (binary) { showMessage(title, tr("Binary file, no text diff")); return; }
    if (rows.isEmpty()) { showMessage(title, tr("No content changes")); return; }

    QVector<DiffPane::Line> left, right;
    left.reserve(rows.size());
    right.reserve(rows.size());
    m_changeStarts.clear();
    m_removed = m_added = 0;
    bool prevChanged = false;

    for (int i = 0; i < rows.size(); ++i) {
        const Row &r = rows.at(i);
        DiffPane::Line a{r.l, r.ln, r.lk};
        DiffPane::Line b{r.r, r.rn, r.rk};
        if (r.lk == DiffPane::Removed && r.rk == DiffPane::Added)
            inlineRange(r.l, r.r, a.hlStart, a.hlEnd, b.hlStart, b.hlEnd);
        if (r.lk == DiffPane::Removed) ++m_removed;
        if (r.rk == DiffPane::Added) ++m_added;
        const bool changed = r.lk != DiffPane::Same || r.rk != DiffPane::Same;
        if (changed && !prevChanged)
            m_changeStarts << i;
        prevChanged = changed;
        left << a;
        right << b;
    }

    m_title->setText(title);
    m_left->setLines(left);
    m_right->setLines(right);
    m_stack->setCurrentIndex(0);
    updateStats();

    m_current = -1;
    updateNav();
    QTimer::singleShot(0, this, [this] {   // after layout, so the viewport height is known
        if (!m_changeStarts.isEmpty())
            nextChange();
    });
}

void DiffView::showMessage(const QString &title, const QString &message)
{
    m_title->setText(title);
    m_stats->clear();
    m_message->setText(message);
    m_left->setLines({});
    m_right->setLines({});
    m_stack->setCurrentIndex(1);
    m_changeStarts.clear();
    m_current = -1;
    m_removed = m_added = 0;
    updateNav();
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
