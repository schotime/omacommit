#include "ResolveWindow.h"
#include "ImageCompare.h"
#include "CommitWindow.h"
#include "DiffView.h"
#include "ElidedLabel.h"
#include "FlowLayout.h"
#include "OgWindow.h"
#include "Syntax.h"
#include "PageTabs.h"
#include "Theme.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringDecoder>
#include <QTextBlock>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int PathRole = Qt::UserRole + 1;

QLabel *sectionLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setObjectName(QStringLiteral("section"));
    return l;
}

// A conflict marker line: seven of `ch`, alone or followed by a space and a label.
bool isMarker(const QString &line, QChar ch)
{
    if (line.size() < 7)
        return false;
    for (int i = 0; i < 7; ++i)
        if (line.at(i) != ch)
            return false;
    return line.size() == 7 || (ch != u'=' && line.at(7) == u' ');
}

int countConflicts(const QByteArray &data)
{
    int n = 0;
    for (const QByteArray &line : data.split('\n'))
        if (line.startsWith("<<<<<<<") && (line.size() == 7 || line.at(7) == ' ' || line.at(7) == '\r'))
            ++n;
    return n;
}

bool isText(const QByteArray &data)
{
    return !data.left(8000).contains('\0');
}

bool isEditableText(const QByteArray &data)
{
    QStringDecoder utf8(QStringDecoder::Utf8);
    [[maybe_unused]] const QString decoded = utf8(data);   // decoding is what sets hasError()
    return !utf8.hasError() && !data.startsWith("\xEF\xBB\xBF") && isText(data);
}

// Where `needle` appears as a run of whole lines in `hay`, at or after `from`.
int findBlock(const QStringList &hay, const QStringList &needle, int from)
{
    for (int i = qMax(0, from); i + needle.size() <= hay.size(); ++i) {
        int j = 0;
        while (j < needle.size() && hay.at(i + j) == needle.at(j))
            ++j;
        if (j == needle.size())
            return i;
    }
    return -1;
}

} // namespace

ResolveWindow::ResolveWindow(const QString &root, const QString &selectPath, QWidget *parent)
    : QWidget(parent), m_repo(root)
{
    setWindowTitle(tr("Resolve — %1").arg(QFileInfo(root).fileName()));
    m_operation = m_repo.operation();
    describeSides();

    // --- left: what is going on, and the files
    auto *tabs = new PageTabs(OgWindow::Resolve, this);
    m_subtitle = new QLabel(m_opText);
    m_subtitle->setObjectName(QStringLiteral("muted"));
    m_subtitle->setWordWrap(true);
    m_subtitle->setTextFormat(Qt::RichText);
    m_subtitle->setVisible(!m_opText.isEmpty());
    auto *repoPath = new ElidedLabel(root);
    repoPath->setObjectName(QStringLiteral("muted"));

    m_files = new QTreeWidget;
    m_files->setColumnCount(2);
    m_files->setHeaderLabels({tr("Path"), tr("State")});
    m_files->setRootIsDecorated(false);
    m_files->setUniformRowHeights(true);
    m_files->setSelectionMode(QAbstractItemView::SingleSelection);
    m_files->header()->setStretchLastSection(false);
    m_files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_files->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    m_hintLabel = new QLabel(m_hint);
    m_hintLabel->setObjectName(QStringLiteral("muted"));
    m_hintLabel->setWordWrap(true);
    m_hintLabel->setVisible(!m_hint.isEmpty());
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("muted"));
    m_commitBtn = new QPushButton(m_operation == u"cherry-pick" ? tr("Commit cherry-pick…")
                                  : m_operation == u"revert"    ? tr("Commit revert…")
                                                                : tr("Commit merge…"));
    m_commitBtn->setObjectName(QStringLiteral("primary"));
    m_commitBtn->setToolTip(tr("Open the commit dialog once every file is resolved"));
    m_commitBtn->setVisible(m_canCommit);
    // A rebase goes on commit by commit: git rebase --continue from here.
    m_continueBtn = new QPushButton(tr("Continue rebase"));
    m_continueBtn->setObjectName(QStringLiteral("primary"));
    m_continueBtn->setToolTip(tr("Record the resolution and replay the next commit (git rebase --continue)"));
    m_continueBtn->setVisible(m_operation == u"rebase");

    auto *left = new QWidget;
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(14, 12, 14, 12);
    ll->setSpacing(8);
    auto *head = new QVBoxLayout;
    head->setSpacing(4);
    head->addWidget(tabs);
    head->addWidget(m_subtitle);
    head->addWidget(repoPath);
    ll->addLayout(head);
    ll->addSpacing(4);
    ll->addWidget(sectionLabel(tr("Conflicted files")));
    ll->addWidget(m_files, 1);
    ll->addWidget(m_hintLabel);
    auto *foot = new QHBoxLayout;
    foot->addWidget(m_status, 1);
    foot->addWidget(m_commitBtn);
    foot->addWidget(m_continueBtn);
    ll->addLayout(foot);

    // --- right, page 0: the two sides over the merged file
    m_top = new DiffView;
    m_top->setNavShortcutsEnabled(false);
    // The conflicts can be picked right where the two sides are shown: a click
    // makes one current, a double-click takes that side, and right-click
    // offers every way.
    m_top->extendMenu = [this](QMenu *menu, QAction *before, int row, bool right) {
        extendSidesMenu(menu, before, row, right);
    };
    m_top->onRowClicked = [this](int row, bool right, bool doubleClick) {
        const int k = m_originalOf.indexOf(originalAtRow(row));
        if (k < 0)
            return;
        if (!doubleClick)
            gotoConflict(k, false);
        else if (right == true)   // the right pane is always yours
            pick(m_mineIsIncoming ? Pick::Incoming : Pick::Current, k);
        else
            pick(m_mineIsIncoming ? Pick::Current : Pick::Incoming, k);
    };
    // Whitespace settings: the sides are diffed again; the merged file only
    // changes how it is drawn, so edits in it are never reset.
    m_top->onOptionsChanged = [this] {
        m_merged->setShowWhitespace(m_top->showWhitespace());
        if (m_stack->currentIndex() == 0)
            showSides();
    };

    m_mergedTitle = new QLabel;
    m_mergedTitle->setObjectName(QStringLiteral("section"));
    m_counter = new QLabel;
    m_counter->setObjectName(QStringLiteral("muted"));
    m_prevBtn = new QToolButton;
    m_prevBtn->setText(QStringLiteral("↑"));
    m_prevBtn->setToolTip(tr("Previous conflict (Alt+Up)"));
    m_nextBtn = new QToolButton;
    m_nextBtn->setText(QStringLiteral("↓"));
    m_nextBtn->setToolTip(tr("Next conflict (Alt+Down)"));

    m_useInc = new QPushButton(tr("Use %1").arg(m_incShort));
    m_useCur = new QPushButton(tr("Use %1").arg(m_curShort));
    m_useIncCur = new QPushButton(tr("%1, then %2").arg(m_incShort, m_curShort));
    m_useCurInc = new QPushButton(tr("%1, then %2").arg(m_curShort, m_incShort));
    m_useInc->setToolTip(tr("Replace this conflict with the %1 side (%2)").arg(m_incShort, m_incLong));
    m_useCur->setToolTip(tr("Replace this conflict with the %1 side (%2)").arg(m_curShort, m_curLong));
    m_useIncCur->setToolTip(tr("Keep both: %1's lines, then %2's").arg(m_incShort, m_curShort));
    m_useCurInc->setToolTip(tr("Keep both: %1's lines, then %2's").arg(m_curShort, m_incShort));
    m_wholeBtn = new QToolButton;
    m_wholeBtn->setText(tr("Whole file ▾"));
    m_wholeBtn->setPopupMode(QToolButton::InstantPopup);
    auto *wholeMenu = new QMenu(m_wholeBtn);
    // Everything reads left to right in pane order: the other side, then yours.
    for (bool incoming : {!m_mineIsIncoming, m_mineIsIncoming})
        wholeMenu->addAction(tr("Use %1 for the whole file").arg(incoming ? m_incShort : m_curShort), this,
                             [this, incoming] { useWholeSide(incoming); });
    m_wholeBtn->setMenu(wholeMenu);
    m_saveBtn = new QPushButton(tr("Save"));
    m_saveBtn->setToolTip(tr("Write the merged file (Ctrl+S)"));
    m_resolvedBtn = new QPushButton(tr("Mark resolved"));
    m_resolvedBtn->setObjectName(QStringLiteral("primary"));
    m_resolvedBtn->setToolTip(tr("Save and stage the file, telling git its conflict is settled"));

    m_merged = new DiffPane;
    m_merged->setReadOnly(false);
    m_merged->setShowWhitespace(m_top->showWhitespace());

    auto *mergedPanel = new QWidget;
    auto *ml = new QVBoxLayout(mergedPanel);
    ml->setContentsMargins(10, 8, 10, 0);
    ml->setSpacing(6);
    auto *mh = new QHBoxLayout;
    mh->addWidget(m_mergedTitle, 1);
    mh->addWidget(m_counter);
    mh->addSpacing(8);
    mh->addWidget(m_prevBtn);
    mh->addWidget(m_nextBtn);
    ml->addLayout(mh);
    // The pick buttons wrap onto a second row when the pane is narrow, rather
    // than setting its minimum width -- one long row held the window's divider
    // in place. Save and Mark resolved stay together on the right.
    auto *picks = new QWidget;
    auto *pickFlow = new FlowLayout(picks);
    m_pickFor = new QLabel;   // which conflict the buttons act on
    m_pickFor->setObjectName(QStringLiteral("section"));
    pickFlow->addWidget(m_pickFor);
    if (m_mineIsIncoming) {   // upstream on the left, yours on the right
        pickFlow->addWidget(m_useCur);
        pickFlow->addWidget(m_useInc);
        pickFlow->addWidget(m_useCurInc);
        pickFlow->addWidget(m_useIncCur);
    } else {
        pickFlow->addWidget(m_useInc);
        pickFlow->addWidget(m_useCur);
        pickFlow->addWidget(m_useIncCur);
        pickFlow->addWidget(m_useCurInc);
    }
    pickFlow->addWidget(m_wholeBtn);
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    picks->setSizePolicy(sp);
    auto *tools = new QHBoxLayout;
    tools->addWidget(picks, 1);
    tools->addWidget(m_saveBtn, 0, Qt::AlignTop);
    tools->addWidget(m_resolvedBtn, 0, Qt::AlignTop);
    ml->addLayout(tools);
    ml->addWidget(m_merged, 1);

    auto *textPage = new QSplitter(Qt::Vertical);
    textPage->addWidget(m_top);
    textPage->addWidget(mergedPanel);
    textPage->setChildrenCollapsible(false);
    textPage->setHandleWidth(1);
    textPage->setSizes({420, 460});

    // --- right, page 1: conflicts that can only be settled for the whole file
    m_wholeText = new QLabel;
    m_wholeText->setWordWrap(true);
    m_wholeText->setAlignment(Qt::AlignCenter);
    m_wholeA = new QPushButton;
    m_wholeB = new QPushButton;
    m_wholeImages = new ImageCompare;
    m_wholeImages->hide();
    auto *wholePage = new QWidget;
    auto *wl = new QVBoxLayout(wholePage);
    wl->addStretch();
    wl->addWidget(m_wholeImages, 8);
    wl->addWidget(m_wholeText);
    auto *wb = new QHBoxLayout;
    wb->addStretch();
    wb->addWidget(m_wholeA);
    wb->addWidget(m_wholeB);
    wb->addStretch();
    wl->addSpacing(12);
    wl->addLayout(wb);
    wl->addStretch();

    // --- right, page 2: a message
    m_message = new QLabel;
    m_message->setObjectName(QStringLiteral("muted"));
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);

    m_stack = new QStackedWidget;
    m_stack->addWidget(textPage);
    m_stack->addWidget(wholePage);
    m_stack->addWidget(m_message);

    auto *split = new QSplitter(Qt::Horizontal);
    split->setObjectName(QStringLiteral("pageSplit"));   // kept level with the other pages'
    split->addWidget(left);
    split->addWidget(m_stack);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);
    // Resizing the window resizes the right side; the left pane keeps its width.
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({350, 1050});   // a quarter: the list only needs file names
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(split);

    // --- signals
    connect(m_files, &QTreeWidget::currentItemChanged, this, &ResolveWindow::openSelected);
    connect(m_merged, &QPlainTextEdit::textChanged, this, &ResolveWindow::parseMerged);
    connect(m_merged, &QPlainTextEdit::cursorPositionChanged, this, &ResolveWindow::updateActions);
    connect(m_useInc, &QPushButton::clicked, this, [this] { pick(Pick::Incoming); });
    connect(m_useCur, &QPushButton::clicked, this, [this] { pick(Pick::Current); });
    connect(m_useIncCur, &QPushButton::clicked, this, [this] { pick(Pick::IncomingThenCurrent); });
    connect(m_useCurInc, &QPushButton::clicked, this, [this] { pick(Pick::CurrentThenIncoming); });
    connect(m_saveBtn, &QPushButton::clicked, this, [this] { save(); });
    connect(m_resolvedBtn, &QPushButton::clicked, this, &ResolveWindow::markResolved);
    connect(m_commitBtn, &QPushButton::clicked, this, &ResolveWindow::commit);
    connect(m_continueBtn, &QPushButton::clicked, this, &ResolveWindow::continueRebase);
    auto step = [this](int dir) {
        const int line = m_merged->textCursor().blockNumber();
        int k = -1;
        if (dir > 0) {
            for (int i = 0; i < m_conflicts.size() && k < 0; ++i)
                if (m_conflicts.at(i).start > line)
                    k = i;
        } else {
            for (int i = int(m_conflicts.size()) - 1; i >= 0 && k < 0; --i)
                if (m_conflicts.at(i).end < line)
                    k = i;
        }
        // Nothing further that way (a single conflict, say): back to the one
        // being worked on, in case it was scrolled out of sight.
        if (k < 0)
            k = targetConflict();
        if (k >= 0)
            gotoConflict(k);
    };
    connect(m_nextBtn, &QToolButton::clicked, this, [step] { step(1); });
    connect(m_prevBtn, &QToolButton::clicked, this, [step] { step(-1); });
    connect(&Theme::instance(), &Theme::changed, this, &ResolveWindow::applyTheme);

    // --- keyboard
    new QShortcut(QKeySequence(QStringLiteral("Alt+Down")), this, [step] { step(1); });
    new QShortcut(QKeySequence(QStringLiteral("Alt+Up")), this, [step] { step(-1); });
    new QShortcut(QKeySequence::Save, this, [this] { save(); });
    new QShortcut(QKeySequence(QStringLiteral("Esc")), this, [this] { OgWindow::back(this); });
    for (const char *keys : {"Ctrl+L", "Ctrl+Tab"})
        new QShortcut(QKeySequence(QString::fromLatin1(keys)), this, [this] { OgWindow::go(this, OgWindow::Log); });

    applyTheme();
    QString select = selectPath;
    if (!select.isEmpty() && QFileInfo(select).isAbsolute())
        select = QDir(root).relativeFilePath(select);
    refreshList(select);
}

// Names for the two sides. Git's "ours" is HEAD and "theirs" the other side --
// which during a rebase means ours is the upstream and theirs is your own
// commit. The UI only ever uses these names, never ours/theirs.
void ResolveWindow::describeSides()
{
    auto out = [this](const QStringList &args) { return QString::fromUtf8(m_repo.run(args).out).trimmed(); };
    auto nameOf = [&](const QString &rev) {
        QString n = out({QStringLiteral("name-rev"), QStringLiteral("--name-only"), rev});
        if (n.isEmpty() || n == u"undefined")
            n = out({QStringLiteral("rev-parse"), QStringLiteral("--short"), rev});
        return n.remove(QStringLiteral("remotes/"));
    };
    auto describe = [&](const QString &rev) { return out({QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%h %s"), rev}); };
    const QString branch = m_repo.branch();
    const QString head = branch.isEmpty() ? tr("HEAD") : tr("HEAD (%1)").arg(branch);

    m_curShort = tr("mine");
    m_curLong = tr("Mine — %1").arg(head);
    if (m_operation == u"merge") {
        const QString other = nameOf(QStringLiteral("MERGE_HEAD"));
        m_incShort = tr("theirs");
        m_incLong = tr("Theirs — %1").arg(other);
        m_canCommit = true;
        m_opText = tr("Merging %1 into %2").arg(Theme::strong(other), Theme::strong(branch.isEmpty() ? tr("HEAD") : branch));
    } else if (m_operation == u"rebase") {
        QString onto, ontoSha;
        for (const char *dir : {"/rebase-merge/onto", "/rebase-apply/onto"}) {
            QFile f(m_repo.gitDir() + QLatin1String(dir));
            if (f.open(QIODevice::ReadOnly)) {
                ontoSha = QString::fromLatin1(f.readAll()).trimmed();
                onto = nameOf(ontoSha);
                break;
            }
        }
        const QString replaying = describe(QStringLiteral("REBASE_HEAD"));
        // HEAD is upstream plus whichever of your commits have been replayed already.
        const int replayed = ontoSha.isEmpty() ? 0
                           : out({QStringLiteral("rev-list"), QStringLiteral("--count"), ontoSha + QStringLiteral("..HEAD")}).toInt();
        const QString base = onto.isEmpty() ? tr("HEAD") : onto;
        m_mineIsIncoming = true;
        m_curShort = tr("upstream");
        m_curLong = replayed == 0 ? tr("Upstream — %1, which you are rebasing onto").arg(base)
                  : replayed == 1 ? tr("Upstream — %1 + 1 of your commits, already replayed").arg(base)
                                  : tr("Upstream — %1 + %2 of your commits, already replayed").arg(base).arg(replayed);
        m_incShort = tr("mine");
        m_incLong = replaying.isEmpty() ? tr("Mine — the commit being replayed") : tr("Mine — replaying %1").arg(replaying);
        m_hint = tr("Resolve every file, then continue the rebase to the next commit.");
        m_opText = tr("Rebasing onto %1").arg(Theme::strong(onto.isEmpty() ? tr("HEAD") : onto));
    } else if (m_operation == u"cherry-pick") {
        const QString picked = describe(QStringLiteral("CHERRY_PICK_HEAD"));
        m_incShort = tr("picked");
        m_incLong = tr("Picked — %1").arg(picked);
        m_canCommit = true;
        m_opText = tr("Cherry-picking %1").arg(Theme::strong(picked));
    } else if (m_operation == u"revert") {
        const QString reverted = describe(QStringLiteral("REVERT_HEAD"));
        m_incShort = tr("reverted");
        m_incLong = tr("Revert of %1").arg(reverted);
        m_canCommit = true;
        m_opText = tr("Reverting %1").arg(Theme::strong(reverted));
    } else {
        m_curShort = tr("current");
        m_curLong = tr("Current — %1").arg(head);
        m_incShort = tr("incoming");
        m_incLong = tr("Incoming changes");
        m_hint = tr("When every file is resolved, stage and commit as usual.");
    }
}

void ResolveWindow::refreshList(const QString &select)
{
    const QVector<UnmergedFile> unmerged = m_repo.unmerged();
    QSet<QString> open;
    for (const UnmergedFile &u : unmerged) {
        open.insert(u.path);
        if (!m_listed.contains(u.path))
            m_listed << u.path;
    }
    // Listed but no longer unmerged: settled, here or elsewhere.
    for (const QString &p : m_listed)
        if (!open.contains(p))
            m_resolved.insert(p);

    const ThemeColors &t = Theme::instance().colors();
    QTreeWidgetItem *toSelect = nullptr;
    {
        QSignalBlocker block(m_files);
        m_files->clear();
        for (const QString &p : m_listed) {
            auto *it = new QTreeWidgetItem;
            it->setText(0, p);
            it->setToolTip(0, p);
            it->setData(0, PathRole, p);
            if (m_resolved.contains(p)) {
                it->setText(1, tr("✓ Resolved"));
                it->setForeground(1, t.green);
            } else {
                const UnmergedFile u = *std::find_if(unmerged.begin(), unmerged.end(),
                                                     [&](const UnmergedFile &x) { return x.path == p; });
                QString state;
                if (u.has(2) && u.has(3)) {
                    // A count only for text: a binary file has no markers to
                    // count, and "none left" would wrongly read as done.
                    QFile f(QDir(m_repo.root()).filePath(p));
                    const QByteArray data = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
                    state = bothState(u.has(1), isText(data) && !data.isEmpty() ? countConflicts(data) : -1);
                } else if (u.has(2)) {
                    state = u.has(1) ? tr("Deleted in %1").arg(m_incShort) : tr("Added in %1 only").arg(m_curShort);
                } else if (u.has(3)) {
                    state = u.has(1) ? tr("Deleted in %1").arg(m_curShort) : tr("Added in %1 only").arg(m_incShort);
                } else {
                    state = tr("Deleted in both");
                }
                it->setText(1, state);
                it->setForeground(1, t.red);
            }
            m_files->addTopLevelItem(it);
            if (p == select)
                toSelect = it;
        }
    }
    const bool allDone = unmerged.isEmpty();
    m_commitBtn->setEnabled(allDone);
    m_continueBtn->setEnabled(allDone && !m_continuing);
    if (allDone && !m_listed.isEmpty())
        m_status->setText(tr("All conflicts resolved."));

    if (!toSelect)
        for (int i = 0; i < m_files->topLevelItemCount() && !toSelect; ++i)
            if (!m_resolved.contains(m_files->topLevelItem(i)->data(0, PathRole).toString()))
                toSelect = m_files->topLevelItem(i);
    if (!toSelect && m_files->topLevelItemCount() > 0)
        toSelect = m_files->topLevelItem(0);
    if (toSelect)
        m_files->setCurrentItem(toSelect);
    openSelected();
}

QString ResolveWindow::bothState(bool hasBase, int left)
{
    const QString s = hasBase ? tr("Both modified") : tr("Both added");
    if (left < 0)
        return s;
    return s + QStringLiteral(" · ") + (left == 0 ? tr("none left") : tr("%1 left").arg(left));
}

// The open file's row counts the conflicts still in the merged pane as they
// are resolved, not only what is on disk.
void ResolveWindow::updateOpenFileState()
{
    if (m_stack->currentIndex() != 0)
        return;
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        auto *it = m_files->topLevelItem(i);
        if (it->data(0, PathRole).toString() != m_path || m_resolved.contains(m_path))
            continue;
        QString state = bothState(m_openHasBase, int(m_conflicts.size()));
        if (m_merged->document()->isModified())
            state += QStringLiteral(" ●");   // not saved yet
        it->setText(1, state);
    }
}

void ResolveWindow::selectNextUnresolved()
{
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        auto *it = m_files->topLevelItem(i);
        if (!m_resolved.contains(it->data(0, PathRole).toString())) {
            m_files->setCurrentItem(it);
            return;
        }
    }
}

// Asks what to do with unsaved edits to the merged file. False: don't go ahead.
bool ResolveWindow::resolveUnsaved(bool allowCancel)
{
    if (!m_merged->document()->isModified() || m_stack->currentIndex() != 0)
        return true;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Unsaved changes"));
    box.setText(tr("Save your changes to %1?").arg(m_path));
    auto buttons = QMessageBox::Save | QMessageBox::Discard;
    if (allowCancel)
        buttons |= QMessageBox::Cancel;
    box.setStandardButtons(buttons);
    box.button(QMessageBox::Discard)->setText(tr("Discard"));   // not "Close without Saving"
    box.setDefaultButton(QMessageBox::Save);
    for (;;) {
        const int choice = box.exec();
        if (choice == QMessageBox::Discard) {
            m_merged->document()->setModified(false);
            return true;
        }
        if (choice == QMessageBox::Save) {
            if (save())
                return true;
            if (allowCancel)
                return false;
            continue;
        }
        return false;
    }
}

void ResolveWindow::openSelected()
{
    auto *it = m_files->currentItem();
    const QString path = it ? it->data(0, PathRole).toString() : QString();
    if (path == m_path && m_merged->document()->isModified() && m_stack->currentIndex() == 0)
        return;   // re-selected by a refresh: keep the edits
    if (path != m_path && !resolveUnsaved()) {
        QSignalBlocker block(m_files);
        for (int i = 0; i < m_files->topLevelItemCount(); ++i)
            if (m_files->topLevelItem(i)->data(0, PathRole).toString() == m_path)
                m_files->setCurrentItem(m_files->topLevelItem(i));
        return;
    }

    m_path = path;
    m_conflicts.clear();
    if (path.isEmpty()) {
        m_message->setText(m_operation.isEmpty() ? tr("There are no conflicts to resolve.")
                                                 : tr("There are no conflicted files."));
        m_stack->setCurrentIndex(2);
    } else if (m_resolved.contains(path)) {
        m_message->setText(tr("%1 is resolved and staged.").arg(path));
        m_stack->setCurrentIndex(2);
    } else {
        const QVector<UnmergedFile> unmerged = m_repo.unmerged();
        const auto u = std::find_if(unmerged.begin(), unmerged.end(), [&](const UnmergedFile &x) { return x.path == path; });
        if (u == unmerged.end()) {
            m_resolved.insert(path);
            refreshList(path);
            return;
        }
        if (u->has(2) && u->has(3))
            openText(*u);
        else
            openWholeFile(*u);
    }
    updateActions();
}

void ResolveWindow::openText(const UnmergedFile &u)
{
    auto blob = [this](const QString &sha) {
        return m_repo.run({QStringLiteral("cat-file"), QStringLiteral("blob"), sha}).out;
    };
    const QByteArray cur = blob(u.stage[2]);
    const QByteArray inc = blob(u.stage[3]);
    QFile f(QDir(m_repo.root()).filePath(u.path));
    const QByteArray merged = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    if (!isText(cur) || !isText(inc) || !f.exists() || !isEditableText(merged)) {
        openWholeFile(u);
        return;
    }

    // The two sides, aligned against each other: incoming left, current right.
    if (!m_tmp)
        m_tmp = std::make_unique<QTemporaryDir>();
    const QString incPath = m_incPath = m_tmp->filePath(QStringLiteral("incoming"));
    const QString curPath = m_curPath = m_tmp->filePath(QStringLiteral("current"));
    for (const auto &[file, data] : {std::pair{incPath, inc}, std::pair{curPath, cur}}) {
        QFile out(file);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            out.write(data);
    }
    m_incLines = DiffView::linesOf(inc);
    m_curLines = DiffView::linesOf(cur);
    m_openHasBase = u.has(1);
    showSides();

    // The merged file starts as git left it -- markers and all, or whatever
    // has been done to it since.
    m_loaded = merged;
    {
        QSignalBlocker block(m_merged);
        m_merged->setPlainText(DiffView::linesOf(merged).join(u'\n'));
    }
    m_merged->document()->setModified(false);
    m_merged->document()->clearUndoRedoStacks();
    m_stack->setCurrentIndex(0);
    m_originals.clear();
    parseMerged();
    buildOriginals();
    m_current = -1;
    if (!m_conflicts.isEmpty())
        gotoConflict(0);
}

// The two sides, aligned against each other: yours on the right.
void ResolveWindow::showSides()
{
    // An SVG can be looked at as a picture too.
    const QString left = m_mineIsIncoming ? m_curPath : m_incPath;
    const QString right = m_mineIsIncoming ? m_incPath : m_curPath;
    const ImageFetch images = [left, right] {
        ImageSides s;
        QFile a(left), b(right);
        s.hasBefore = a.open(QIODevice::ReadOnly);
        s.before = a.readAll();
        s.hasAfter = b.open(QIODevice::ReadOnly);
        s.after = b.readAll();
        return s;
    };
    m_top->setFileName(m_path);   // the language for syntax colours
    // Captions first: the picture view labels its sides with them.
    if (m_mineIsIncoming)
        m_top->setPaneCaptions(m_curLong, m_incLong);
    else
        m_top->setPaneCaptions(m_incLong, m_curLong);
    m_top->showDiff(m_path, m_repo.diffFiles(left, right), false, images);
    placeOriginals();
}

void ResolveWindow::openWholeFile(const UnmergedFile &u)
{
    m_originals.clear();
    m_originalOf.clear();
    m_top->setConflictRows({});
    const QString path = u.path;
    auto arm = [this](QPushButton *b, const QString &text, std::function<void()> fn) {
        QObject::disconnect(b, &QPushButton::clicked, nullptr, nullptr);
        b->setText(text);
        b->setVisible(!text.isEmpty());
        if (fn)
            connect(b, &QPushButton::clicked, this, fn);
    };
    const QStringList add{QStringLiteral("add"), QStringLiteral("--"), path};
    auto keep = [this, path, add](bool incoming) {
        finishWith({QStringLiteral("checkout"), incoming ? QStringLiteral("--theirs") : QStringLiteral("--ours"),
                    QStringLiteral("--"), path}, add);
    };
    auto remove = [this, path] {
        if (QMessageBox::question(this, tr("Delete %1").arg(path), tr("Resolve by deleting %1?").arg(path),
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
            finishWith({QStringLiteral("rm"), QStringLiteral("-q"), QStringLiteral("--"), path});
    };

    // An image: show both sides to choose between, yours on the right.
    {
        auto side = [this, &u](int stage, const QString &caption) {
            const QByteArray data = u.has(stage)
                ? m_repo.run({QStringLiteral("cat-file"), QStringLiteral("blob"), u.stage[stage]}).out
                : QByteArray();
            return ImageCompare::load(caption, data, u.has(stage));
        };
        const ImageCompare::Side inc = side(3, m_incLong), cur = side(2, m_curLong);
        m_wholeImages->setSides(m_mineIsIncoming ? cur : inc, m_mineIsIncoming ? inc : cur);
        m_wholeImages->setVisible(m_wholeImages->hasImage());
    }

    if (u.has(2) && u.has(3)) {
        m_wholeText->setText(tr("%1 can't be merged line by line (it is binary, or not UTF-8 text).\n"
                                "Choose one side for the whole file.").arg(path));
        const bool leftIsIncoming = !m_mineIsIncoming;
        arm(m_wholeA, tr("Use %1").arg(leftIsIncoming ? m_incShort : m_curShort), [keep, leftIsIncoming] { keep(leftIsIncoming); });
        arm(m_wholeB, tr("Use %1").arg(leftIsIncoming ? m_curShort : m_incShort), [keep, leftIsIncoming] { keep(!leftIsIncoming); });
    } else if (u.has(2)) {
        m_wholeText->setText(u.has(1) ? tr("%1 was changed in %2 but deleted in %3.").arg(path, m_curShort, m_incShort)
                                      : tr("%1 was added in %2 only.").arg(path, m_curShort));
        arm(m_wholeA, tr("Keep %1").arg(m_curShort), [keep] { keep(false); });
        arm(m_wholeB, tr("Delete it"), remove);
    } else if (u.has(3)) {
        m_wholeText->setText(u.has(1) ? tr("%1 was changed in %2 but deleted in %3.").arg(path, m_incShort, m_curShort)
                                      : tr("%1 was added in %2 only.").arg(path, m_incShort));
        arm(m_wholeA, tr("Keep %1").arg(m_incShort), [keep] { keep(true); });
        arm(m_wholeB, tr("Delete it"), remove);
    } else {
        m_wholeText->setText(tr("%1 was deleted in both.").arg(path));
        arm(m_wholeA, tr("Delete it"), remove);
        arm(m_wholeB, QString(), nullptr);
    }
    m_stack->setCurrentIndex(1);
}

QStringList ResolveWindow::mergedLines() const
{
    const QString text = m_merged->toPlainText();
    return text.isEmpty() ? QStringList() : text.split(u'\n');
}

// Finds the conflict marker blocks -- they are what "unresolved" means -- and
// colours the merged pane to match the panes above: the current side's
// section, the incoming side's, git's common-ancestor section if it wrote one.
void ResolveWindow::parseMerged()
{
    if (m_highlighting)
        return;   // only the colours changed
    const QStringList lines = mergedLines();
    QVector<DiffPane::Line> rows(lines.size());
    m_conflicts.clear();
    Conflict c;
    enum { Outside, InCurrent, InBase, InIncoming } state = Outside;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &l = lines.at(i);
        DiffPane::Line &row = rows[i];
        row.number = i + 1;
        if (state == Outside && isMarker(l, u'<')) {
            c = Conflict{i, -1, -1, -1};
            state = InCurrent;
            row.kind = DiffPane::Marker;
        } else if (state == InCurrent && isMarker(l, u'|')) {
            c.base = i;
            state = InBase;
            row.kind = DiffPane::Marker;
        } else if ((state == InCurrent || state == InBase) && isMarker(l, u'=')) {
            c.sep = i;
            state = InIncoming;
            row.kind = DiffPane::Marker;
        } else if (state == InIncoming && isMarker(l, u'>')) {
            c.end = i;
            m_conflicts << c;
            state = Outside;
            row.kind = DiffPane::Marker;
        } else {
            // Coloured by whose work it is, matching the panes above: during a
            // rebase git's HEAD section is upstream's, not yours.
            const auto yours = DiffPane::Mine, other = DiffPane::Theirs;
            row.kind = state == InCurrent ? (m_mineIsIncoming ? other : yours)
                     : state == InBase    ? DiffPane::Base
                     : state == InIncoming ? (m_mineIsIncoming ? yours : other)
                                           : DiffPane::Same;
        }
    }
    m_merged->setKinds(rows);
    // The merged file in colour too. Laying the colours on counts as a change
    // to the document, which would bring us straight back here.
    if (!m_highlighting) {
        m_highlighting = true;
        QVector<Syntax::Spans> spans = Syntax::highlight(m_path, lines);
        for (int i = 0; i < spans.size() && i < rows.size(); ++i)
            if (rows.at(i).kind == DiffPane::Marker)
                spans[i].clear();   // git's marker lines aren't code
        m_merged->setSyntax(spans);
        m_highlighting = false;
    }
    matchOriginals();
    updateOpenFileState();
    updateActions();
}

QStringList ResolveWindow::section(const Conflict &c, bool incoming) const
{
    const QStringList lines = mergedLines();
    const int from = incoming ? c.sep + 1 : c.start + 1;
    const int to = incoming ? c.end : (c.base >= 0 ? c.base : c.sep);
    return lines.mid(from, qMax(0, to - from));
}

// The conflict the actions apply to: the one the caret is in, else the one
// last moved to, else the next one below the caret.
int ResolveWindow::targetConflict() const
{
    const int line = m_merged->textCursor().blockNumber();
    for (int i = 0; i < m_conflicts.size(); ++i)
        if (line >= m_conflicts.at(i).start && line <= m_conflicts.at(i).end)
            return i;
    if (m_current >= 0 && m_current < m_conflicts.size())
        return m_current;
    for (int i = 0; i < m_conflicts.size(); ++i)
        if (m_conflicts.at(i).start > line)
            return i;
    return m_conflicts.isEmpty() ? -1 : int(m_conflicts.size()) - 1;
}

void ResolveWindow::gotoConflict(int k, bool revealAbove)
{
    if (k < 0 || k >= m_conflicts.size())
        return;
    m_current = k;
    const QTextBlock b = m_merged->document()->findBlockByNumber(m_conflicts.at(k).start);
    m_merged->setTextCursor(QTextCursor(b));
    m_merged->centerCursor();

    // Follow in the panes above: find each conflict's sides in the stage
    // files, in order, so repeated text resolves to the right place.
    int atCur = 0, atInc = 0, row = -1;
    for (int i = 0; i <= k; ++i) {
        const QStringList cur = section(m_conflicts.at(i), false);
        const QStringList inc = section(m_conflicts.at(i), true);
        const int a = cur.isEmpty() ? -1 : findBlock(m_curLines, cur, atCur);
        const int b2 = inc.isEmpty() ? -1 : findBlock(m_incLines, inc, atInc);
        if (a >= 0)
            atCur = a + int(cur.size());
        if (b2 >= 0)
            atInc = b2 + int(inc.size());
        if (i == k)
            row = a >= 0 ? m_top->rowForLine(!m_mineIsIncoming, a + 1)
                : b2 >= 0 ? m_top->rowForLine(m_mineIsIncoming, b2 + 1) : -1;
    }
    if (revealAbove)
        m_top->revealRow(row);
    updateActions();
}

void ResolveWindow::pick(Pick how, int k)
{
    if (k < 0)
        k = targetConflict();
    if (k < 0 || k >= m_conflicts.size())
        return;
    const Conflict c = m_conflicts.at(k);
    const QStringList cur = section(c, false);
    const QStringList inc = section(c, true);
    QStringList with;
    switch (how) {
    case Pick::Current: with = cur; break;
    case Pick::Incoming: with = inc; break;
    case Pick::CurrentThenIncoming: with = cur + inc; break;
    case Pick::IncomingThenCurrent: with = inc + cur; break;
    }

    // Replace the whole marker block through the editor, so it is one undo step.
    QTextDocument *doc = m_merged->document();
    const QTextBlock first = doc->findBlockByNumber(c.start);
    const QTextBlock last = doc->findBlockByNumber(c.end);
    QTextCursor tc(doc);
    if (!with.isEmpty()) {
        tc.setPosition(first.position());
        tc.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    } else if (last.next().isValid()) {   // nothing to keep: take the lines out entirely
        tc.setPosition(first.position());
        tc.setPosition(last.next().position(), QTextCursor::KeepAnchor);
    } else if (first.previous().isValid()) {
        tc.setPosition(first.previous().position() + first.previous().length() - 1);
        tc.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    } else {
        tc.select(QTextCursor::Document);
    }
    tc.beginEditBlock();
    tc.insertText(with.join(u'\n'));
    tc.endEditBlock();

    // On to the next one, which now has this one's index.
    m_current = -1;
    if (!m_conflicts.isEmpty())
        gotoConflict(qMin(k, int(m_conflicts.size()) - 1));
}

void ResolveWindow::useWholeSide(bool incoming)
{
    QTextCursor tc(m_merged->document());
    tc.select(QTextCursor::Document);
    tc.beginEditBlock();
    tc.insertText((incoming ? m_incLines : m_curLines).join(u'\n'));
    tc.endEditBlock();
}

bool ResolveWindow::save()
{
    if (m_stack->currentIndex() != 0)
        return true;
    // Decided by content, not the editor's modified flag: Mark resolved stages
    // what is on disk, so it must be exactly what the pane shows.
    if (DiffView::compose(mergedLines(), m_loaded) == m_loaded) {
        m_merged->document()->setModified(false);
        return true;
    }
    const QString file = QDir(m_repo.root()).filePath(m_path);
    QFile in(file);
    const QByteArray onDisk = in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
    in.close();
    if (onDisk != m_loaded) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("%1 changed on disk").arg(m_path));
        box.setText(tr("%1 has changed on disk since it was opened here.").arg(m_path));
        box.setInformativeText(tr("Saving will overwrite that change."));
        box.setStandardButtons(QMessageBox::Save | QMessageBox::Cancel);
        box.button(QMessageBox::Save)->setText(tr("Overwrite"));
        box.setDefaultButton(QMessageBox::Cancel);
        if (box.exec() != QMessageBox::Save)
            return false;
    }
    const QByteArray data = DiffView::compose(mergedLines(), m_loaded);
    QFile out(file);   // truncate in place: keeps permissions and symlinks
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(data) != data.size()) {
        QMessageBox::warning(this, tr("Could not save %1").arg(m_path), out.errorString());
        return false;
    }
    m_loaded = data;
    m_merged->document()->setModified(false);
    m_status->setText(tr("Saved %1").arg(m_path));
    updateActions();
    return true;
}

void ResolveWindow::markResolved()
{
    if (m_stack->currentIndex() != 0 || !save())
        return;
    if (!m_conflicts.isEmpty()) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("Conflicts left"));
        box.setText(m_conflicts.size() == 1 ? tr("%1 still has an unresolved conflict.").arg(m_path)
                                            : tr("%1 still has %2 unresolved conflicts.").arg(m_path).arg(m_conflicts.size()));
        box.setInformativeText(tr("Marking it resolved stages it with the conflict markers in it."));
        box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
        box.button(QMessageBox::Ok)->setText(tr("Mark resolved anyway"));
        box.setDefaultButton(QMessageBox::Cancel);
        if (box.exec() != QMessageBox::Ok)
            return;
    }
    finishWith({QStringLiteral("add"), QStringLiteral("--"), m_path});
}

// Runs the git commands that settle the open file, then moves on.
void ResolveWindow::finishWith(const QStringList &gitArgs, const QStringList &thenArgs)
{
    GitResult r = m_repo.run(gitArgs);
    if (r.ok() && !thenArgs.isEmpty())
        r = m_repo.run(thenArgs);
    if (!r.ok()) {
        QMessageBox::warning(this, tr("Could not resolve %1").arg(m_path), QString::fromUtf8(r.err));
        refreshList(m_path);
        return;
    }
    m_merged->document()->setModified(false);
    m_resolved.insert(m_path);
    m_status->setText(tr("Resolved %1").arg(m_path));
    const QString done = m_path;
    refreshList(done);
    selectNextUnresolved();
}

void ResolveWindow::commit()
{
    if (!resolveUnsaved())
        return;
    OgWindow::go(this, OgWindow::Commit);
}

// git rebase --continue: records this stop's resolution and replays the next
// commit. It either finishes, stops at the next commit's conflicts -- the
// window then moves on to those -- or stops for something else, which is
// left to the terminal.
void ResolveWindow::continueRebase()
{
    if (m_continuing || !resolveUnsaved())
        return;
    m_continuing = new QProcess(this);
    m_repo.configure(*m_continuing);
    QProcessEnvironment env = m_continuing->processEnvironment();
    env.insert(QStringLiteral("GIT_EDITOR"), QStringLiteral("true"));   // keep the commit's own message
    m_continuing->setProcessEnvironment(env);
    connect(m_continuing, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        const QString output = QString::fromUtf8(m_continuing->readAllStandardOutput() + m_continuing->readAllStandardError());
        m_continuing->deleteLater();
        m_continuing = nullptr;
        m_operation = m_repo.operation();
        if (m_operation != u"rebase") {
            m_opText = tr("The rebase is finished.");
            m_subtitle->setText(m_opText);
            m_hintLabel->hide();
            m_continueBtn->hide();
            m_status->setText(tr("Rebase finished"));
            m_message->setText(tr("The rebase is finished."));
            m_stack->setCurrentIndex(2);
            return;
        }
        if (!m_repo.unmerged().isEmpty()) {
            // The next commit's conflicts: a fresh list, and sides named for it.
            describeSides();
            m_subtitle->setText(m_opText);
            m_listed.clear();
            m_resolved.clear();
            m_path.clear();
            m_status->setText(tr("Continued; the next commit has conflicts"));
            refreshList();
            return;
        }
        m_status->setText(tr("The rebase stopped"));
        refreshList();
        QMessageBox::information(this, tr("The rebase stopped"),
                                 tr("git stopped for something other than a conflict; carry on in a terminal.\n\n")
                                     + output.right(1500));
    });
    m_status->setText(tr("Continuing the rebase…"));
    m_continueBtn->setEnabled(false);
    m_continuing->start(QStringLiteral("git"), {QStringLiteral("rebase"), QStringLiteral("--continue")});
    m_continuing->closeWriteChannel();
}

void ResolveWindow::updateActions()
{
    const bool text = m_stack->currentIndex() == 0;
    const int n = int(m_conflicts.size());
    const int k = text ? targetConflict() : -1;
    const bool dirty = text && m_merged->document()->isModified();
    m_mergedTitle->setText((dirty ? QStringLiteral("● ") : QString()) + tr("Merged — saved to %1").arg(m_path));
    // Numbered as the file was opened, like the frames above: resolving one
    // doesn't renumber the rest.
    const int number = k >= 0 && k < m_originalOf.size() && m_originalOf.at(k) >= 0 ? m_originalOf.at(k) + 1 : k + 1;
    const int total = qMax(int(m_originals.size()), n);
    m_counter->setText(!text ? QString()
                       : n == 0 ? tr("No conflicts left")
                       : n == total ? tr("Conflict %1 of %2").arg(number).arg(total)
                                    : tr("Conflict %1 of %2 · %3 left").arg(number).arg(total).arg(n));
    m_pickFor->setText(text && n > 0 ? tr("Conflict %1:").arg(number) : QString());
    m_pickFor->setVisible(text && n > 0);
    updateMarks();
    for (QWidget *w : {static_cast<QWidget *>(m_useInc), static_cast<QWidget *>(m_useCur),
                       static_cast<QWidget *>(m_useIncCur), static_cast<QWidget *>(m_useCurInc)})
        w->setEnabled(text && n > 0);
    m_prevBtn->setEnabled(text && n > 0);
    m_nextBtn->setEnabled(text && n > 0);
    m_saveBtn->setEnabled(dirty);
}

void ResolveWindow::applyTheme()
{
    const ThemeColors &t = Theme::instance().colors();
    // Yours is the accent colour and the other side a colour picked to stand
    // apart from it (Theme::load), in all three panes.
    m_top->setSideTints(t.other, t.accent);
    const qreal soft = t.light ? 0.16 : 0.20;
    DiffColors c;
    c.bg = t.background;
    c.fg = t.foreground;
    c.gutterBg = t.background;
    c.gutterFg = t.muted;
    c.border = t.border;
    c.empty = t.border;
    c.mine = Theme::mix(t.background, t.accent, soft);
    c.theirs = Theme::mix(t.background, t.other, soft);
    c.base = Theme::mix(t.background, t.muted, 0.12);
    c.marker = Theme::mix(t.background, t.red, 0.35);
    c.frame = t.accent;
    c.frameIdle = t.muted;
    m_merged->setColors(c);
}

void ResolveWindow::buildOriginals()
{
    m_originals.clear();
    for (const Conflict &c : m_conflicts)
        m_originals << Original{section(c, false), section(c, true)};
    placeOriginals();
    matchOriginals();
    updateActions();
}

// Where each conflict's two sections are in the stage files, in order (so
// repeated text lands in the right place), and so which rows of the panes.
void ResolveWindow::placeOriginals()
{
    int atCur = 0, atInc = 0;
    const bool curRight = !m_mineIsIncoming;
    for (Original &o : m_originals) {
        o.firstRow = o.lastRow = -1;
        auto take = [&](const QStringList &lines, const QStringList &sec, int &at, bool right) {
            const int from = sec.isEmpty() ? -1 : findBlock(lines, sec, at);
            if (from < 0)
                return;
            at = from + int(sec.size());
            for (int l = from; l < at; ++l) {
                const int row = m_top->rowForLine(right, l + 1);
                if (row < 0)
                    continue;
                o.firstRow = o.firstRow < 0 ? row : qMin(o.firstRow, row);
                o.lastRow = qMax(o.lastRow, row);
            }
        };
        take(m_curLines, o.cur, atCur, curRight);
        take(m_incLines, o.inc, atInc, !curRight);
    }
    updateMarks();
}

// Which original each conflict still in the file is: the same sections, in
// order; one edited by hand keeps the next free number.
void ResolveWindow::matchOriginals()
{
    m_originalOf.clear();
    int next = 0;
    for (const Conflict &c : m_conflicts) {
        const QStringList cur = section(c, false), inc = section(c, true);
        int found = -1;
        for (int o = next; o < m_originals.size() && found < 0; ++o)
            if (m_originals.at(o).cur == cur && m_originals.at(o).inc == inc)
                found = o;
        if (found < 0 && next < m_originals.size())
            found = next;
        m_originalOf << found;
        if (found >= 0)
            next = found + 1;
    }
}

int ResolveWindow::originalAtRow(int row) const
{
    for (int o = 0; o < m_originals.size(); ++o)
        if (row >= m_originals.at(o).firstRow && row <= m_originals.at(o).lastRow)
            return o;
    return -1;
}

void ResolveWindow::updateMarks()
{
    const bool text = m_stack && m_stack->currentIndex() == 0;
    const int k = text ? targetConflict() : -1;
    const int current = k >= 0 && k < m_originalOf.size() ? m_originalOf.at(k) : -1;
    QVector<DiffPane::Group> above, below;
    if (text) {
        for (int o = 0; o < m_originals.size(); ++o) {
            const Original &x = m_originals.at(o);
            if (x.firstRow >= 0)
                above << DiffPane::Group{x.firstRow, x.lastRow, o + 1, o == current, !m_originalOf.contains(o)};
        }
        for (int i = 0; i < m_conflicts.size(); ++i)
            below << DiffPane::Group{m_conflicts.at(i).start, m_conflicts.at(i).end,
                                     m_originalOf.value(i, i) + 1, i == k, false};
    }
    m_top->setConflictRows(above);
    m_merged->setGroups(below, false);
}

// Right-click on a conflict in the panes above: every way to settle it, the
// side clicked first.
void ResolveWindow::extendSidesMenu(QMenu *menu, QAction *before, int row, bool right)
{
    const int o = originalAtRow(row);
    if (o < 0)
        return;
    auto *title = new QAction(tr("Conflict %1").arg(o + 1), menu);
    title->setEnabled(false);
    menu->insertAction(before, title);
    const int k = m_originalOf.indexOf(o);
    if (k < 0) {
        auto *a = new QAction(tr("Resolved (Ctrl+Z in the merged file undoes it)"), menu);
        a->setEnabled(false);
        menu->insertAction(before, a);
        return;
    }
    // The right pane is always yours.
    const QString mine = m_mineIsIncoming ? m_incShort : m_curShort;
    const QString theirs = m_mineIsIncoming ? m_curShort : m_incShort;
    const Pick useMine = m_mineIsIncoming ? Pick::Incoming : Pick::Current;
    const Pick useTheirs = m_mineIsIncoming ? Pick::Current : Pick::Incoming;
    const Pick mineThenTheirs = m_mineIsIncoming ? Pick::IncomingThenCurrent : Pick::CurrentThenIncoming;
    const Pick theirsThenMine = m_mineIsIncoming ? Pick::CurrentThenIncoming : Pick::IncomingThenCurrent;
    QVector<QPair<QString, Pick>> items{
        {tr("Use %1").arg(mine), useMine},
        {tr("Use %1").arg(theirs), useTheirs},
        {tr("%1, then %2").arg(mine, theirs), mineThenTheirs},
        {tr("%1, then %2").arg(theirs, mine), theirsThenMine},
    };
    if (!right) {   // clicked on their side: theirs first
        std::swap(items[0], items[1]);
        std::swap(items[2], items[3]);
    }
    for (const auto &[label, how] : items) {
        auto *a = new QAction(label, menu);
        connect(a, &QAction::triggered, this, [this, how, k] { pick(how, k); });
        menu->insertAction(before, a);
    }
}

void ResolveWindow::select(const QString &path)
{
    if (path != m_path && !resolveUnsaved())
        return;
    refreshList(QFileInfo(path).isAbsolute() ? QDir(m_repo.root()).relativeFilePath(path) : path);
}

// Back from another page of the window: the conflicts may have moved on.
void ResolveWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (m_shownBefore)
        refreshList(m_path);
    m_shownBefore = true;
}

void ResolveWindow::closeEvent(QCloseEvent *e)
{
    if (!resolveUnsaved()) {
        e->ignore();
        return;
    }
    e->accept();
}
