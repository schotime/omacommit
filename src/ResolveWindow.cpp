#include "ResolveWindow.h"
#include "CommitWindow.h"
#include "DiffView.h"
#include "ElidedLabel.h"
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
    m_title = new QLabel(tr("Resolve conflicts"));
    m_title->setObjectName(QStringLiteral("title"));
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
    head->setSpacing(2);
    head->addWidget(m_title);
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
    auto *tools = new QHBoxLayout;
    if (m_mineIsIncoming) {   // upstream on the left, yours on the right
        tools->addWidget(m_useCur);
        tools->addWidget(m_useInc);
        tools->addWidget(m_useCurInc);
        tools->addWidget(m_useIncCur);
    } else {
        tools->addWidget(m_useInc);
        tools->addWidget(m_useCur);
        tools->addWidget(m_useIncCur);
        tools->addWidget(m_useCurInc);
    }
    tools->addWidget(m_wholeBtn);
    tools->addStretch();
    tools->addWidget(m_saveBtn);
    tools->addWidget(m_resolvedBtn);
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
    auto *wholePage = new QWidget;
    auto *wl = new QVBoxLayout(wholePage);
    wl->addStretch();
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
    split->addWidget(left);
    split->addWidget(m_stack);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
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
    new QShortcut(QKeySequence(QStringLiteral("Esc")), this, [this] { close(); });

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
    parseMerged();
    m_current = -1;
    if (!m_conflicts.isEmpty())
        gotoConflict(0);
}

// The two sides, aligned against each other: yours on the right.
void ResolveWindow::showSides()
{
    if (m_mineIsIncoming) {
        m_top->showDiff(m_path, m_repo.diffFiles(m_curPath, m_incPath), false);
        m_top->setPaneCaptions(m_curLong, m_incLong);
    } else {
        m_top->showDiff(m_path, m_repo.diffFiles(m_incPath, m_curPath), false);
        m_top->setPaneCaptions(m_incLong, m_curLong);
    }
}

void ResolveWindow::openWholeFile(const UnmergedFile &u)
{
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

void ResolveWindow::gotoConflict(int k)
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
    m_top->revealRow(row);
    updateActions();
}

void ResolveWindow::pick(Pick how)
{
    const int k = targetConflict();
    if (k < 0)
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
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *cw = qobject_cast<CommitWindow *>(w); cw && cw->isVisible()) {
            cw->raise();
            cw->activateWindow();
            close();
            return;
        }
    auto *cw = new CommitWindow(m_repo.root());
    cw->setAttribute(Qt::WA_DeleteOnClose);
    cw->resize(size());
    cw->show();
    close();
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
    m_counter->setText(!text ? QString() : n == 0 ? tr("No conflicts left") : tr("Conflict %1 of %2").arg(k + 1).arg(n));
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
    m_merged->setColors(c);
}

void ResolveWindow::closeEvent(QCloseEvent *e)
{
    if (!resolveUnsaved()) {
        e->ignore();
        return;
    }
    e->accept();
}
