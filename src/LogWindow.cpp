#include "LogWindow.h"
#include "CommitWindow.h"
#include "DiffView.h"
#include "ElidedLabel.h"
#include "ResolveWindow.h"
#include "Theme.h"

#include <QCheckBox>
#include <QDateTime>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int RowRole = Qt::UserRole + 1;
constexpr int BatchSize = 500;

QLabel *sectionLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setObjectName(QStringLiteral("section"));
    return l;
}

QString statusText(QChar s)
{
    switch (s.toLatin1()) {
    case 'A': return QStringLiteral("Added");
    case 'D': return QStringLiteral("Deleted");
    case 'R': return QStringLiteral("Renamed");
    case 'C': return QStringLiteral("Copied");
    case 'T': return QStringLiteral("Type changed");
    default:  return QStringLiteral("Modified");
    }
}

// Paints the first column: graph lanes, the commit's dot, then its branch and
// tag names as badges, then the subject.
class GraphDelegate : public QStyledItemDelegate {
public:
    explicit GraphDelegate(LogWindow *w) : QStyledItemDelegate(w), m_w(w) {}

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem o = option;
        initStyleOption(&o, index);
        o.text.clear();
        const QWidget *view = o.widget;
        view->style()->drawControl(QStyle::CE_ItemViewItem, &o, p, view);   // background, selection

        const int row = index.data(RowRole).toInt();
        if (row < 0 || row >= m_w->graph().size())
            return;
        const GraphRow &g = m_w->graph().at(row);
        const LogCommit &c = m_w->commits().at(row);
        const ThemeColors &t = Theme::instance().colors();

        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = o.rect;
        const int lane = qMax(12, qRound(o.fontMetrics.height() * 0.95));
        const qreal top = r.top(), bottom = r.bottom() + 1, mid = (top + bottom) / 2.0;
        auto x = [&](int i) { return r.left() + 6 + lane * i + lane / 2.0; };

        auto stroke = [&](qreal x1, qreal y1, qreal x2, qreal y2, const QColor &col) {
            QPainterPath path(QPointF(x1, y1));
            if (qFuzzyCompare(x1, x2)) {
                path.lineTo(x2, y2);
            } else {
                const qreal my = (y1 + y2) / 2.0;
                path.cubicTo(x1, my, x2, my, x2, y2);
            }
            p->setPen(QPen(col, 2, Qt::SolidLine, Qt::RoundCap));
            p->setBrush(Qt::NoBrush);
            p->drawPath(path);
        };
        for (const auto &s : g.top)
            stroke(x(s.from), top, x(s.to), mid, laneColor(s.from, t));
        for (const auto &s : g.bottom)
            stroke(x(s.from), mid, x(s.to), bottom, laneColor(s.to, t));

        // The dot: hollow for merges, ringed for the commit HEAD is on.
        const QVector<RefLabel> refs = m_w->refs().value(c.hash);
        const bool isHead = std::any_of(refs.begin(), refs.end(), [](const RefLabel &l) { return l.current; });
        const QColor dot = laneColor(g.column, t);
        const qreal rad = lane * 0.26;
        if (isHead) {
            p->setPen(QPen(t.foreground, 2));
            p->setBrush(Qt::NoBrush);
            p->drawEllipse(QPointF(x(g.column), mid), rad + 3, rad + 3);
        }
        p->setPen(QPen(dot, 2));
        p->setBrush(g.merge ? QBrush(t.background) : QBrush(dot));
        p->drawEllipse(QPointF(x(g.column), mid), rad, rad);

        // Badges.
        qreal tx = r.left() + 6 + lane * g.width + 6;
        QFont badgeFont = o.font;
        badgeFont.setPointSizeF(badgeFont.pointSizeF() * 0.88);
        const QFontMetrics bfm(badgeFont);
        for (const RefLabel &ref : refs) {
            QFont f = badgeFont;
            f.setBold(ref.current);
            const QFontMetrics fm(f);
            const qreal w = fm.horizontalAdvance(ref.name) + 12;
            if (tx + w > r.right() - 40)
                break;
            const QColor kind = refColor(ref, t);
            const QRectF box(tx, r.top() + 3, w, r.height() - 6);
            p->setPen(QPen(kind, 1));
            p->setBrush(Theme::mix(t.background, kind, ref.current ? 0.45 : 0.22));
            p->drawRoundedRect(box, 4, 4);
            p->setFont(f);
            p->setPen(t.foreground);
            p->drawText(box, Qt::AlignCenter, ref.name);
            tx += w + 4;
        }

        // Subject.
        p->setFont(o.font);
        p->setPen(o.state & QStyle::State_Selected ? o.palette.color(QPalette::HighlightedText)
                                                   : o.palette.color(QPalette::Text));
        const QRectF textRect(tx + 2, r.top(), r.right() - tx - 4, r.height());
        p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                    o.fontMetrics.elidedText(c.subject, Qt::ElideRight, int(textRect.width())));
        p->restore();
    }

    static QColor laneColor(int lane, const ThemeColors &t)
    {
        return t.lanes.isEmpty() ? t.accent : t.lanes.at(lane % t.lanes.size());
    }

    static QColor refColor(const RefLabel &ref, const ThemeColors &t)
    {
        switch (ref.kind) {
        case RefLabel::Head:   return t.red;
        case RefLabel::Branch: return ref.current ? t.accent : t.refBranch;
        case RefLabel::Remote: return t.refRemote;
        case RefLabel::Tag:    return t.refTag;
        }
        return t.muted;
    }

private:
    LogWindow *m_w;
};

} // namespace

LogWindow::LogWindow(const QString &root, QWidget *parent) : QWidget(parent), m_repo(root)
{
    setWindowTitle(tr("Log — %1").arg(QFileInfo(root).fileName()));

    // --- widgets
    m_header = new QLabel;
    m_header->setObjectName(QStringLiteral("title"));
    m_header->setTextFormat(Qt::RichText);
    m_repoPath = new ElidedLabel(root);
    m_repoPath->setObjectName(QStringLiteral("muted"));
    m_allBranches = new QCheckBox(tr("All branches"));
    m_allBranches->setChecked(true);
    m_allBranches->setToolTip(tr("Show every local and remote branch and tag, not only the current branch"));

    m_commits = new QTreeWidget;
    m_commits->setColumnCount(4);
    m_commits->setHeaderLabels({tr("Graph"), tr("Author"), tr("Date"), tr("Commit")});
    m_commits->setRootIsDecorated(false);
    m_commits->setUniformRowHeights(true);
    m_commits->setSelectionMode(QAbstractItemView::SingleSelection);
    m_commits->setItemDelegateForColumn(0, new GraphDelegate(this));
    m_commits->viewport()->installEventFilter(this);   // hide columns as it narrows
    m_commits->header()->setStretchLastSection(false);
    // Sized by fitColumns rather than stretched, so the table can be wider
    // than the pane and scroll sideways to the other columns.
    m_commits->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_commits->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_commits->header(), &QHeaderView::sectionResized, this, [this](int section, int, int size) {
        if (section == 0 && !m_fitting)
            m_subjectDragged = size;
    });
    m_commits->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_commits->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_commits->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    // Column widths come from the themed font; see sizeColumns().

    m_details = new QPlainTextEdit;
    m_details->setReadOnly(true);
    m_details->setLineWrapMode(QPlainTextEdit::WidgetWidth);

    m_fileCount = new QLabel;
    m_fileCount->setObjectName(QStringLiteral("muted"));
    m_files = new QTreeWidget;
    m_files->setColumnCount(2);
    m_files->setHeaderLabels({tr("Path"), tr("Status")});
    m_files->setRootIsDecorated(false);
    m_files->setUniformRowHeights(true);
    m_files->setSelectionMode(QAbstractItemView::SingleSelection);
    m_files->header()->setStretchLastSection(false);
    m_files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_files->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    m_diff = new DiffView;
    m_diff->onOptionsChanged = [this] { showFileDiff(); };

    // --- layout: commits over details over files, diff on the right
    auto *commitsPane = new QWidget;
    auto *cl = new QVBoxLayout(commitsPane);
    cl->setContentsMargins(14, 12, 14, 6);
    cl->setSpacing(8);
    auto *head = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(m_header);
    titles->addWidget(m_repoPath);
    head->addLayout(titles, 1);
    head->addWidget(m_allBranches, 0, Qt::AlignBottom);
    cl->addLayout(head);
    cl->addWidget(m_commits, 1);

    auto *detailsPane = new QWidget;
    auto *dl = new QVBoxLayout(detailsPane);
    dl->setContentsMargins(14, 6, 14, 6);
    dl->setSpacing(6);
    dl->addWidget(sectionLabel(tr("Commit")));
    dl->addWidget(m_details, 1);

    auto *filesPane = new QWidget;
    auto *fl = new QVBoxLayout(filesPane);
    fl->setContentsMargins(14, 6, 14, 12);
    fl->setSpacing(6);
    auto *filesHead = new QHBoxLayout;
    filesHead->addWidget(sectionLabel(tr("Files")));
    filesHead->addStretch();
    filesHead->addWidget(m_fileCount);
    fl->addLayout(filesHead);
    fl->addWidget(m_files, 1);

    auto *leftSplit = new QSplitter(Qt::Vertical);
    leftSplit->addWidget(commitsPane);
    leftSplit->addWidget(detailsPane);
    leftSplit->addWidget(filesPane);
    leftSplit->setChildrenCollapsible(false);
    leftSplit->setHandleWidth(1);
    leftSplit->setStretchFactor(0, 5);
    leftSplit->setStretchFactor(1, 2);
    leftSplit->setStretchFactor(2, 3);
    leftSplit->setSizes({460, 150, 250});

    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(leftSplit);
    split->addWidget(m_diff);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setSizes({700, 700});   // half: the commit table is what you read here

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(split);

    // --- signals
    connect(m_commits, &QTreeWidget::currentItemChanged, this, &LogWindow::showCommit);
    m_commits->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_commits, &QWidget::customContextMenuRequested, this, &LogWindow::showCommitMenu);
    connect(m_files, &QTreeWidget::currentItemChanged, this, &LogWindow::showFileDiff);
    connect(m_allBranches, &QCheckBox::toggled, this, &LogWindow::reload);
    // Fetch the next batch as the list nears its end.
    connect(m_commits->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        const QScrollBar *sb = m_commits->verticalScrollBar();
        if (v >= sb->maximum() - sb->pageStep())
            loadMore();
    });
    connect(&Theme::instance(), &Theme::changed, this, &LogWindow::applyTheme);

    // --- keyboard
    new QShortcut(QKeySequence(QStringLiteral("F5")), this, [this] { reload(); });
    new QShortcut(QKeySequence(QStringLiteral("Esc")), this, [this] { close(); });

    reload();
    applyTheme();
    m_commits->setFocus();
}

void LogWindow::reload()
{
    const QString branch = m_repo.branch();
    m_header->setText(m_allBranches->isChecked() ? tr("History of all branches")
                      : branch.isEmpty()         ? tr("History of <i>detached HEAD</i>")
                                                 : tr("History of %1").arg(Theme::strong(branch)));

    const QString keep = m_shownCommit;
    m_refs = m_repo.refsByCommit();
    m_log.clear();
    m_graph.clear();
    m_layout.reset();
    m_exhausted = false;
    m_shownCommit.clear();
    {
        QSignalBlocker block(m_commits);
        m_commits->clear();
    }
    loadMore();

    if (m_log.isEmpty()) {
        m_details->setPlainText(tr("No commits yet."));
        m_files->clear();
        m_fileCount->clear();
        m_diff->showMessage({}, tr("No commits yet"));
        return;
    }
    // Stay on the commit that was showing if it is still in the list.
    QTreeWidgetItem *select = m_commits->topLevelItem(0);
    for (int i = 0; i < m_log.size() && !keep.isEmpty(); ++i)
        if (m_log.at(i).hash == keep) {
            select = m_commits->topLevelItem(i);
            break;
        }
    m_commits->setCurrentItem(select);
    m_commits->scrollToItem(select);
}

void LogWindow::loadMore()
{
    if (m_exhausted || m_loading)
        return;
    m_loading = true;
    const QVector<LogCommit> batch = m_repo.log(int(m_log.size()), BatchSize, m_allBranches->isChecked());
    if (batch.size() < BatchSize)
        m_exhausted = true;

    QList<QTreeWidgetItem *> items;
    items.reserve(batch.size());
    for (const LogCommit &c : batch) {
        const int row = int(m_log.size());
        m_log.push_back(c);
        m_graph.push_back(m_layout.add(c.hash, c.parents));
        auto *it = new QTreeWidgetItem;
        it->setData(0, RowRole, row);
        it->setToolTip(0, tr("%1\n\n%2 · %3 · %4")
                              .arg(c.subject, c.author,
                                   QDateTime::fromSecsSinceEpoch(c.time).toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                                   c.hash.left(8)));
        it->setText(1, c.author);
        it->setToolTip(1, c.email);
        it->setText(2, QDateTime::fromSecsSinceEpoch(c.time).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        it->setText(3, c.hash.left(8));
        it->setToolTip(3, c.hash);
        items << it;
    }
    m_commits->addTopLevelItems(items);
    m_loading = false;
}

// A commit is shown against its first parent, as TortoiseGit does for merges;
// a root commit against the empty tree.
QString LogWindow::baseOf(const LogCommit &c) const
{
    return c.parents.isEmpty() ? m_repo.emptyTree() : c.parents.constFirst();
}

void LogWindow::showCommit()
{
    auto *it = m_commits->currentItem();
    if (!it)
        return;
    const LogCommit &c = m_log.at(it->data(0, RowRole).toInt());
    if (c.hash == m_shownCommit)
        return;
    m_shownCommit = c.hash;

    QStringList parents;
    for (const QString &p : c.parents)
        parents << p.left(8);
    QString details = m_repo.commitMessage(c.hash);
    details += QStringLiteral("\n\n") + tr("Commit   %1").arg(c.hash);
    details += QStringLiteral("\n") + tr("Author   %1 <%2>").arg(c.author, c.email);
    details += QStringLiteral("\n") + tr("Date     %1")
                                          .arg(QDateTime::fromSecsSinceEpoch(c.time).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    if (!parents.isEmpty())
        details += QStringLiteral("\n") + tr("Parents  %1").arg(parents.join(QStringLiteral(", ")));
    m_details->setPlainText(details);

    m_commitFiles = m_repo.changedFiles(baseOf(c), c.hash);
    {
        QSignalBlocker block(m_files);
        m_files->clear();
        for (int i = 0; i < m_commitFiles.size(); ++i) {
            const FileEntry &f = m_commitFiles.at(i);
            auto *fi = new QTreeWidgetItem;
            fi->setText(0, f.oldPath.isEmpty() ? f.path : f.oldPath + QStringLiteral(" → ") + f.path);
            fi->setToolTip(0, fi->text(0));
            fi->setText(1, statusText(f.commitStatus));
            fi->setForeground(1, statusColor(f.commitStatus));
            fi->setData(0, RowRole, i);
            m_files->addTopLevelItem(fi);
        }
    }
    m_fileCount->setText(m_commitFiles.size() == 1 ? tr("1 file") : tr("%1 files").arg(m_commitFiles.size()));
    if (m_files->topLevelItemCount() > 0)
        m_files->setCurrentItem(m_files->topLevelItem(0));
    else
        m_diff->showMessage({}, c.parents.size() > 1 ? tr("Merge with no changes against its first parent")
                                                     : tr("No file changes"));
}

void LogWindow::showFileDiff()
{
    auto *fi = m_files->currentItem();
    auto *ci = m_commits->currentItem();
    if (!fi || !ci)
        return;
    const LogCommit &c = m_log.at(ci->data(0, RowRole).toInt());
    const FileEntry &f = m_commitFiles.at(fi->data(0, RowRole).toInt());
    m_diff->showDiff(fi->text(0), m_repo.diffBetween(baseOf(c), c.hash, f), false);
}

void LogWindow::showCommitMenu(const QPoint &pos)
{
    auto *it = m_commits->itemAt(pos);
    if (!it)
        return;
    const LogCommit c = m_log.at(it->data(0, RowRole).toInt());
    QMenu menu(this);
    QAction *revert = menu.addAction(tr("Revert changes by this commit…"));
    revert->setEnabled(m_repo.operation().isEmpty());
    if (!revert->isEnabled())
        revert->setToolTip(tr("Finish the %1 in progress first").arg(m_repo.operation()));
    if (menu.exec(m_commits->viewport()->mapToGlobal(pos)) == revert)
        revertCommit(c);
}

// TortoiseGit's "Revert changes by this commit": the commit's changes are
// undone in the working tree, not committed, so they can be reviewed first.
// git leaves its prepared "Revert ..." message, which og commit starts from.
void LogWindow::revertCommit(const LogCommit &c)
{
    const bool merge = c.parents.size() > 1;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Revert %1").arg(c.hash.left(8)));
    box.setTextFormat(Qt::PlainText);   // the subject is shown as written
    box.setText(tr("Undo the changes made by %1 “%2”?").arg(c.hash.left(8), c.subject));
    box.setInformativeText(
        (merge ? tr("It is a merge: what it brought in, compared with its first parent, is undone.\n\n") : QString())
        + tr("The changes are undone in your working tree and nothing is committed, so you can review "
             "them and commit when ready."));
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(tr("Revert"));
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Ok)
        return;

    QStringList args{QStringLiteral("revert"), QStringLiteral("--no-commit")};
    if (merge)
        args << QStringLiteral("-m") << QStringLiteral("1");
    const GitResult r = m_repo.run(args << c.hash);
    const QString output = QString::fromUtf8(r.out + r.err).trimmed();

    auto open = [this](QWidget *w) {
        w->setAttribute(Qt::WA_DeleteOnClose);
        w->resize(size());
        w->show();
    };
    if (r.ok()) {
        QMessageBox done(this);
        done.setIcon(QMessageBox::Information);
        done.setWindowTitle(tr("Reverted"));
        done.setText(tr("The changes made by %1 are undone in your working tree.").arg(c.hash.left(8)));
        done.setInformativeText(tr("Commit them to record the revert; the message git prepared is filled in."));
        QPushButton *commitNow = done.addButton(tr("Open commit dialog"), QMessageBox::AcceptRole);
        done.addButton(tr("Later"), QMessageBox::RejectRole);
        done.exec();
        if (done.clickedButton() == commitNow)
            open(new CommitWindow(m_repo.root()));
    } else if (!m_repo.unmerged().isEmpty()) {
        QMessageBox clash(this);
        clash.setIcon(QMessageBox::Warning);
        clash.setWindowTitle(tr("Revert has conflicts"));
        clash.setText(tr("Undoing %1 conflicts with later changes.").arg(c.hash.left(8)));
        clash.setInformativeText(tr("Resolve the conflicts, then commit the revert."));
        QPushButton *resolve = clash.addButton(tr("Resolve…"), QMessageBox::AcceptRole);
        clash.addButton(tr("Later"), QMessageBox::RejectRole);
        clash.exec();
        if (clash.clickedButton() == resolve)
            open(new ResolveWindow(m_repo.root()));
    } else {
        QMessageBox::warning(this, tr("Could not revert %1").arg(c.hash.left(8)), output.right(1500));
    }
}

QColor LogWindow::statusColor(QChar status) const
{
    const ThemeColors &c = Theme::instance().colors();
    switch (status.toLatin1()) {
    case 'A': return c.green;
    case 'D': return c.red;
    case 'R':
    case 'C': return c.accent;
    default:  return c.yellow;
    }
}

// The date and hash are fixed-length text, so their columns are sized to fit
// it. Measured after polishing, because the theme's font arrives through the
// stylesheet, and again on theme changes, which can switch it.
void LogWindow::sizeColumns()
{
    m_commits->ensurePolished();
    const QFontMetrics fm(m_commits->font());
    const int pad = fm.horizontalAdvance(QStringLiteral("MM"));
    int hexWidth = 0;   // the UI font is proportional: size for the widest hex digit
    for (QChar ch : QStringLiteral("0123456789abcdef"))
        hexWidth = qMax(hexWidth, fm.horizontalAdvance(ch));
    m_colWidth[1] = fm.horizontalAdvance(QStringLiteral("Firstname Lastname")) + pad;
    m_colWidth[2] = fm.horizontalAdvance(QStringLiteral("2026-09-23 22:22")) + pad;
    m_colWidth[3] = hexWidth * 8 + pad;
    for (int col = 1; col <= 3; ++col)
        m_commits->setColumnWidth(col, m_colWidth[col]);
    // Room for a readable subject -- about 36 characters -- plus a few lanes of graph.
    m_subjectMin = fm.averageCharWidth() * 36 + fm.height() * 6;
    fitColumns();
}

// The Graph/subject column -- what the log is read for -- gets a generous
// width: whatever the pane has left after the other columns, but never less
// than its floor. When that doesn't fit, the table scrolls sideways to reach
// Author, Date and Commit rather than squeezing the subject. A width set by
// dragging the column is kept.
void LogWindow::fitColumns()
{
    if (!m_colWidth[1])
        return;
    int others = 0;
    for (int col = 1; col <= 3; ++col)
        others += m_colWidth[col];
    const int visible = m_commits->viewport()->width();
    const int fill = visible - others;
    m_fitting = true;   // never wider than the pane itself: the subject should always be readable without scrolling
    m_commits->setColumnWidth(0, m_subjectDragged ? m_subjectDragged : qMax(qMin(m_subjectMin, visible), fill));
    m_fitting = false;
}

bool LogWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_commits->viewport() && event->type() == QEvent::Resize)
        fitColumns();
    return QWidget::eventFilter(watched, event);
}

void LogWindow::applyTheme()
{
    sizeColumns();
    m_diff->applyTheme();
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        auto *fi = m_files->topLevelItem(i);
        fi->setForeground(1, statusColor(m_commitFiles.at(fi->data(0, RowRole).toInt()).commitStatus));
    }
    m_commits->viewport()->update();
}
