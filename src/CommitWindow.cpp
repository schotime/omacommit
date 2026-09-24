#include "CommitWindow.h"
#include "DiffView.h"
#include "ElidedLabel.h"
#include "MessageEdit.h"
#include "ResolveWindow.h"
#include "OgWindow.h"
#include "PageTabs.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QStyle>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringDecoder>
#include <QTextCursor>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>
#include <climits>

namespace {
constexpr int IndexRole = Qt::UserRole + 1;
constexpr int MaxHistory = 25;

QLabel *sectionLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setObjectName(QStringLiteral("section"));
    return l;
}
} // namespace

CommitWindow::CommitWindow(const QString &root, QWidget *parent)
    : QWidget(parent), m_repo(root)
{
    setWindowTitle(tr("Commit — %1").arg(QFileInfo(root).fileName()));

    // --- widgets
    m_tabs = new PageTabs(OgWindow::Commit, this);
    m_header = new QLabel;
    m_header->setTextFormat(Qt::RichText);
    m_repoPath = new ElidedLabel(root);
    m_repoPath->setObjectName(QStringLiteral("muted"));

    // Prefer Omarchy's default, then other agents installed on PATH.
    m_agents = Agent::available();
    if (!m_agents.isEmpty())
        m_agent = m_agents.first();
    m_writeBtn = new QToolButton;
    m_writeBtn->setText(tr("✨ Write"));
    m_writeBtn->setVisible(!m_agents.isEmpty());
    if (m_agents.size() > 1) {
        auto *menu = new QMenu(m_writeBtn);
        for (const Agent &a : m_agents) {
            auto *choice = menu->addAction(a.name);
            choice->setCheckable(true);
            connect(choice, &QAction::triggered, this, [this, a] {
                m_agent = a;
                updateWriteButton();
            });
        }
        auto *chooseAgent = new QToolButton;
        chooseAgent->setText(tr("▾"));
        chooseAgent->setObjectName(QStringLiteral("agentPicker"));
        chooseAgent->setFixedWidth(26);
        chooseAgent->setMenu(menu);
        chooseAgent->setPopupMode(QToolButton::InstantPopup);
        m_agentMenuBtn = chooseAgent;
    }

    m_historyBtn = new QToolButton;
    m_historyBtn->setText(tr("Recent ▾"));
    m_historyBtn->setToolTip(tr("Reuse a recent commit message"));
    m_historyBtn->setPopupMode(QToolButton::InstantPopup);
    m_historyMenu = new QMenu(m_historyBtn);
    m_historyBtn->setMenu(m_historyMenu);

    m_message = new MessageEdit;
    m_amend = new QCheckBox(tr("Amend last commit"));
    m_counter = new QLabel;

    m_newBranch = new QLineEdit;
    m_newBranch->setPlaceholderText(tr("New branch (leave empty to commit to the current one)"));
    m_newBranch->setToolTip(tr("Create this branch from the current HEAD and commit to it"));
    m_newBranch->setClearButtonEnabled(true);

    m_selectAll = new QCheckBox(tr("Files"));
    m_selectAll->setTristate(true);
    m_fileCount = new QLabel;
    m_fileCount->setObjectName(QStringLiteral("muted"));
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter files (Ctrl+F)"));
    m_filter->setClearButtonEnabled(true);

    m_files = new QTreeWidget;
    m_files->setColumnCount(2);
    m_files->setHeaderLabels({tr("Path"), tr("Status")});
    // Two sections, Staged and Changes, as header rows over flush file rows.
    m_files->setRootIsDecorated(false);
    m_files->setItemsExpandable(false);
    m_files->setIndentation(0);
    m_files->setUniformRowHeights(true);
    // Several files can be selected (Shift/Ctrl-click); ticking one of them
    // ticks them all. The diff shows the one clicked last.
    m_files->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_files->viewport()->installEventFilter(this);   // checkbox clicks keep the selection
    m_files->header()->setStretchLastSection(false);
    m_files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_files->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("muted"));
    m_pushBtn = new QPushButton(tr("Commit && Push"));
    m_pushBtn->setToolTip(tr("Ctrl+Shift+Enter"));
    m_commitBtn = new QPushButton(tr("Commit"));
    m_commitBtn->setObjectName(QStringLiteral("primary"));
    m_commitBtn->setToolTip(tr("Ctrl+Enter"));

    m_diff = new DiffView;
    m_diff->rediff = [this](const QStringList &lines) { return rediffEdited(lines); };
    m_diff->onSave = [this](const QStringList &lines) { return writeEdited(lines); };
    m_diff->leftFile = [this] { return m_diffBase; };
    m_diff->onSaveRequested = [this] { saveEdited(); };
    m_diff->onOptionsChanged = [this] { showCurrentDiff(); };

    // --- layout
    auto *left = new QWidget;
    auto *l = new QVBoxLayout(left);
    l->setContentsMargins(14, 12, 14, 12);
    l->setSpacing(8);

    // Commit · Log, then what it commits to and where.
    auto *where = new QHBoxLayout;
    where->setSpacing(10);
    where->addWidget(m_header);
    where->addWidget(m_repoPath, 1);
    auto *head = new QVBoxLayout;
    head->setSpacing(4);
    head->addWidget(m_tabs);
    head->addLayout(where);
    l->addLayout(head);
    l->addSpacing(4);

    auto *msgHead = new QHBoxLayout;
    msgHead->addWidget(sectionLabel(tr("Message")));
    msgHead->addStretch();
    msgHead->addWidget(m_writeBtn);
    if (m_agentMenuBtn)
        msgHead->addWidget(m_agentMenuBtn);
    msgHead->addWidget(m_historyBtn);
    l->addLayout(msgHead);
    l->addWidget(m_message, 2);

    auto *msgFoot = new QHBoxLayout;
    msgFoot->addWidget(m_amend);
    msgFoot->addStretch();
    msgFoot->addWidget(m_counter);
    l->addLayout(msgFoot);
    l->addWidget(m_newBranch);
    l->addSpacing(4);

    auto *filesHead = new QHBoxLayout;
    filesHead->addWidget(m_selectAll);
    filesHead->addStretch();
    filesHead->addWidget(m_fileCount);
    l->addLayout(filesHead);
    l->addWidget(m_filter);
    l->addWidget(m_files, 3);

    auto *actions = new QHBoxLayout;
    actions->addWidget(m_status, 1);
    actions->addWidget(m_pushBtn);
    actions->addWidget(m_commitBtn);
    l->addLayout(actions);

    auto *split = m_split = new QSplitter(Qt::Horizontal);
    split->setObjectName(QStringLiteral("pageSplit"));   // kept level with the other pages'
    split->addWidget(left);
    split->addWidget(m_diff);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);
    // Resizing the window resizes the right side; the left pane keeps its width.
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({520, 880});   // provisional; showEvent sizes it for the message

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(split);

    // --- signals
    connect(m_message, &QPlainTextEdit::textChanged, this, &CommitWindow::updateCounts);
    connect(m_amend, &QCheckBox::toggled, this, &CommitWindow::onAmendToggled);
    connect(m_files, &QTreeWidget::currentItemChanged, this, &CommitWindow::showCurrentDiff);
    m_files->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_files, &QWidget::customContextMenuRequested, this, &CommitWindow::showFileMenu);
    connect(m_files, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *it, int column) {
        if (m_updatingChecks || column != 0)
            return;
        if (it->isSelected()) {
            m_updatingChecks = true;
            for (QTreeWidgetItem *s : m_files->selectedItems())
                if (entryOf(s))
                    s->setCheckState(0, it->checkState(0));
            m_updatingChecks = false;
        }
        updateSelectAllState();
        updateCounts();
    });
    connect(m_selectAll, &QCheckBox::clicked, this, &CommitWindow::toggleAll);
    connect(m_filter, &QLineEdit::textChanged, this, &CommitWindow::applyFilter);
    connect(m_commitBtn, &QPushButton::clicked, this, [this] { commit(false); });
    connect(m_pushBtn, &QPushButton::clicked, this, [this] { commit(true); });
    connect(m_historyMenu, &QMenu::aboutToShow, this, &CommitWindow::rebuildHistoryMenu);
    connect(m_writeBtn, &QToolButton::clicked, this, &CommitWindow::writeMessage);
    updateWriteButton();
    connect(&Theme::instance(), &Theme::changed, this, &CommitWindow::applyTheme);

    // --- keyboard
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), this, [this] { commit(false); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Enter")), this, [this] { commit(false); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Return")), this, [this] { commit(true); });
    new QShortcut(QKeySequence(QStringLiteral("F5")), this, [this] { refresh(); });
    for (const char *keys : {"Ctrl+L", "Ctrl+Tab"})
        new QShortcut(QKeySequence(QString::fromLatin1(keys)), this, [this] { OgWindow::go(this, OgWindow::Log); });
    new QShortcut(QKeySequence::Save, this, [this] { saveEdited(); });
    new QShortcut(QKeySequence(QStringLiteral("Esc")), this, [this] { OgWindow::back(this); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+F")), this, [this] {
        m_filter->setFocus();
        m_filter->selectAll();
    });
    new QShortcut(QKeySequence(Qt::Key_Space), m_files, [this] {
        if (auto *it = m_files->currentItem(); it && entryOf(it))
            it->setCheckState(0, it->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);   // the selection follows
    }, Qt::WidgetShortcut);

    // Restore an unsent message from last time.
    const QString draft = QSettings().value(draftKey()).toString();
    if (!draft.isEmpty())
        setMessage(draft);
    // Otherwise, mid-merge (or cherry-pick, or revert), start from the message
    // git prepared. Its # lines are instructions: `commit -F` would keep them.
    if (m_message->toPlainText().trimmed().isEmpty()) {
        QFile prepared(m_repo.gitDir() + QStringLiteral("/MERGE_MSG"));
        if (prepared.open(QIODevice::ReadOnly)) {
            QStringList keep;
            for (const QString &line : QString::fromUtf8(prepared.readAll()).split(u'\n'))
                if (!line.startsWith(u'#'))
                    keep << line;
            setMessage(keep.join(u'\n').trimmed());
        }
    }

    refresh();
    applyTheme();
    m_message->setFocus();
}

QString CommitWindow::draftKey() const
{
    return QStringLiteral("drafts/") + QString::fromLatin1(m_repo.root().toUtf8().toHex());
}

namespace {
// A row is known by its section and path: a half-staged file has one in each.
QString rowKey(const FileEntry &e)
{
    return (e.staged ? QStringLiteral("S:") : QStringLiteral("W:")) + e.path;
}
} // namespace

QVector<QTreeWidgetItem *> CommitWindow::fileItems() const
{
    QVector<QTreeWidgetItem *> out;
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        QTreeWidgetItem *section = m_files->topLevelItem(i);
        for (int j = 0; j < section->childCount(); ++j)
            out << section->child(j);
    }
    return out;
}

const FileEntry *CommitWindow::entryOf(const QTreeWidgetItem *it) const
{
    const int i = it ? it->data(0, IndexRole).toInt() : -1;
    return it && it->parent() && i >= 0 && i < m_entries.size() ? &m_entries.at(i) : nullptr;
}

void CommitWindow::refresh()
{
    QSet<QString> known, checked;
    QString currentKey, currentPath;
    for (QTreeWidgetItem *it : fileItems()) {
        const QString key = rowKey(*entryOf(it));
        known.insert(key);
        if (it->checkState(0) == Qt::Checked)
            checked.insert(key);
    }
    if (const FileEntry *cur = entryOf(m_files->currentItem())) {
        currentKey = rowKey(*cur);
        currentPath = cur->path;
    }

    // Staged: the index against what the commit builds on -- HEAD, or when
    // amending its parent, so the last commit's changes are listed there too.
    // Changes: the working tree against the index.
    const bool amend = m_amend->isChecked();
    m_entries = m_repo.stagedFiles(diffBase());
    m_indexChanged.clear();
    if (amend) {
        for (const FileEntry &e : m_repo.stagedFiles(QStringLiteral("HEAD")))
            m_indexChanged.insert(e.path);
        // The last commit's files first, in its order: they are what amending is about.
        QHash<QString, int> order;
        const QVector<FileEntry> last = m_repo.changedFiles(diffBase(), QStringLiteral("HEAD"));
        for (int i = 0; i < last.size(); ++i)
            order.insert(last.at(i).path, i);
        std::stable_sort(m_entries.begin(), m_entries.end(), [&](const FileEntry &a, const FileEntry &b) {
            return order.value(a.path, INT_MAX) < order.value(b.path, INT_MAX);
        });
    }
    m_entries += m_repo.unstagedFiles();
    m_tabs->setConflicts(std::any_of(m_entries.begin(), m_entries.end(), [](const FileEntry &e) { return e.conflicted(); }));
    // Files you have staged part of. (While amending, being in the last commit
    // doesn't count: its newer changes are still meant to go in by default.)
    QSet<QString> stagedPaths;
    for (const FileEntry &e : m_entries)
        if (e.staged && (!amend || m_indexChanged.contains(e.path)))
            stagedPaths.insert(e.path);

    const QString branch = m_repo.branch();
    m_header->setText(branch.isEmpty() ? tr("on <i>detached HEAD</i>") : tr("to %1").arg(Theme::strong(branch)));
    if (m_repo.isMerging())
        m_header->setText(m_header->text() + tr(" <i>(merge)</i>"));

    m_updatingChecks = true;
    {
        QSignalBlocker block(m_files);
        m_files->clear();
    }
    QTreeWidgetItem *sections[2] = {nullptr, nullptr};   // staged, changes
    auto sectionFor = [&](bool staged) {
        QTreeWidgetItem *&s = sections[staged ? 0 : 1];
        if (!s) {
            s = new QTreeWidgetItem;
            s->setText(0, staged ? (amend ? tr("Staged, with the last commit") : tr("Staged")) : tr("Changes"));
            s->setData(0, IndexRole, -1);
            s->setFlags(Qt::ItemIsEnabled);   // a heading: not selectable, not ticked
            QFont f = s->font(0);
            f.setWeight(QFont::DemiBold);
            s->setFont(0, f);
            m_files->insertTopLevelItem(staged ? 0 : m_files->topLevelItemCount(), s);
        }
        return s;
    };
    QTreeWidgetItem *toSelect = nullptr, *samePath = nullptr;
    for (int i = 0; i < m_entries.size(); ++i) {
        const FileEntry &e = m_entries.at(i);
        auto *it = new QTreeWidgetItem;
        it->setText(0, e.oldPath.isEmpty() ? e.path : e.oldPath + QStringLiteral(" → ") + e.path);
        it->setToolTip(0, it->text(0));
        it->setText(1, e.statusText());
        it->setData(0, IndexRole, i);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        // Ticked unless untracked (TortoiseGit's default) -- or when part of the
        // file is staged: then its staged part is what's meant, unless you tick
        // the rest too.
        // Staging part of a file changes what its change row should default to.
        const QString key = rowKey(e);
        const bool newlyStaged = !e.staged && stagedPaths.contains(e.path)
                              && !known.contains(QStringLiteral("S:") + e.path) && !amend;
        const bool on = known.contains(key) && !newlyStaged
                            ? checked.contains(key)
                            : e.staged || (!e.untracked() && !stagedPaths.contains(e.path));
        it->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
        sectionFor(e.staged)->addChild(it);
        if (key == currentKey)
            toSelect = it;
        else if (e.path == currentPath && !samePath)
            samePath = it;   // staged or unstaged away: follow the file to its other section
    }
    for (QTreeWidgetItem *s : sections)
        if (s)
            s->setText(1, s->childCount() == 1 ? tr("1 file") : tr("%1 files").arg(s->childCount()));
    m_files->expandAll();
    m_updatingChecks = false;

    applyTheme();
    applyFilter();
    updateSelectAllState();
    updateCounts();

    if (!toSelect)
        toSelect = samePath;
    if (!toSelect && !fileItems().isEmpty())
        toSelect = fileItems().constFirst();
    if (toSelect)
        m_files->setCurrentItem(toSelect);
    else
        m_diff->showMessage({}, tr("Working tree clean"));
}

namespace {
bool isUtf8(const QByteArray &data)
{
    QStringDecoder utf8(QStringDecoder::Utf8);
    [[maybe_unused]] const QString decoded = utf8(data);   // decoding is what sets hasError()
    return !utf8.hasError();
}
} // namespace

void CommitWindow::showCurrentDiff()
{
    QTreeWidgetItem *it = m_files->currentItem();
    const FileEntry *entry = entryOf(it);

    if (m_diff->isDirty()) {
        if (entry && entry->path == m_diffPath && entry->staged == m_diffStaged)
            return;   // a refresh re-selected the file being edited: keep the edits
        // Moving to another file. If the edited one is still listed, Cancel
        // can go back to it; if it has dropped out, only Save or Discard can.
        QTreeWidgetItem *editedItem = nullptr;
        for (QTreeWidgetItem *f : fileItems())
            if (entryOf(f)->path == m_diffPath && entryOf(f)->staged == m_diffStaged)
                editedItem = f;
        if (!resolveUnsavedEdits(editedItem != nullptr)) {
            QSignalBlocker block(m_files);
            m_files->setCurrentItem(editedItem);
            return;
        }
    }

    // Whatever is still loading for an earlier selection is dropped when it arrives.
    const int request = ++m_diffRequest;
    const bool sameFile = entry && entry->path == m_diffPath && entry->staged == m_diffStaged;
    m_diffLoaded.clear();
    m_diffBase.clear();
    m_wsBase.clear();
    m_shownIndex.clear();
    if (!entry) {
        m_diffPath.clear();
        m_diff->setPaneCaptions({}, {});
        m_diff->showMessage({}, tr("Select a file to see its changes"));
        return;
    }
    const FileEntry e = *entry;
    m_diffPath = e.path;
    m_diff->setFileName(e.path);   // the language for syntax colours
    m_diffStaged = e.staged;

    // Decided here, where the list is; the git work happens on a worker thread
    // so a slow git (Windows, a big repository) doesn't freeze the window.
    const QString title = it->text(0);
    const QString base = diffBase();
    const bool amend = m_amend->isChecked();
    const bool mayEdit = canEditInDiff(e);
    // For a file with staged changes the unstaged diff's left side is what's
    // staged, not the last commit -- worth saying, as the staged part doesn't
    // show there.
    bool partlyStaged = false;
    for (const FileEntry &s : m_entries)
        if (s.staged && s.path == e.path && (!amend || m_indexChanged.contains(e.path)))
            partlyStaged = true;
    if (!sameFile)   // re-showing the same file keeps it on screen, and its scroll position
        m_diff->showMessage(title, tr("Loading diff…"));

    struct Loaded {
        WhitespaceRules rules;
        QByteArray index, before, onDisk, diff;
        bool inIndex = false, hasBefore = false, onDiskOk = false, diffCr = false;
        QHash<int, QString> issues;
    };
    const GitRepo repo = m_repo;
    QPointer<CommitWindow> self(this);
    QThreadPool::globalInstance()->start([self, repo, request, e, base, title, amend, mayEdit, partlyStaged] {
        Loaded d;
        d.rules = repo.whitespaceRules(e.path);
        d.index = repo.indexBlob(e.path, &d.inIndex);
        if (e.staged) {
            // What is staged, against what the commit builds on.
            const GitResult b = repo.run({QStringLiteral("cat-file"), QStringLiteral("blob"),
                                          base + QLatin1Char(':') + (e.oldPath.isEmpty() ? e.path : e.oldPath)});
            d.hasBefore = b.ok();
            d.before = b.out;
            d.diff = repo.diffStaged(e, base);
            d.issues = repo.whitespaceIssues(d.rules, d.before, d.index);
        } else {
            // What is not staged yet: the working tree against the index (for
            // an untracked file, against nothing).
            QFile f(QDir(repo.root()).filePath(e.path));
            d.onDiskOk = f.open(QIODevice::ReadOnly);
            d.onDisk = d.onDiskOk ? f.readAll() : QByteArray();
            d.diff = repo.diffUnstaged(e);
            // Whether git's diff kept the CRs (it drops them when core.autocrlf
            // normalises the file). Re-diffs of edits must see the file the same
            // way, or one keystroke would turn every line of a CRLF file into a change.
            for (const QByteArray &line : d.diff.split('\n'))
                if ((line.startsWith('+') || line.startsWith(' ')) && !line.startsWith("+++") && line.endsWith('\r')) {
                    d.diffCr = true;
                    break;
                }
            d.issues = repo.whitespaceIssues(d.rules, d.index, d.onDisk);
        }
        QMetaObject::invokeMethod(qApp, [self, request, e, title, amend, mayEdit, partlyStaged, d] {
            if (!self || request != self->m_diffRequest)
                return;   // gone, or another file was picked meanwhile
            CommitWindow *w = self;
            w->m_wsRules = d.rules;
            w->m_shownIndex = d.index;
            if (e.staged) {
                // Read-only: the index is changed a block or a line at a time, not by typing.
                w->m_wsBase = d.hasBefore ? d.before : QByteArray();
                const ImageFetch images = [d] {
                    ImageSides s;
                    s.hasBefore = d.hasBefore;
                    s.before = d.before;
                    s.hasAfter = d.inIndex;
                    s.after = d.index;
                    return s;
                };
                w->m_diff->setPaneCaptions(amend ? tr("Parent of the last commit") : tr("Last commit"), tr("Staged"));
                w->m_diff->showDiff(title + tr("  (staged)"), d.diff, false, images);
                w->m_diff->setWhitespaceIssues(d.issues);
                if (d.inIndex && isUtf8(d.index) && isUtf8(w->m_wsBase))
                    w->m_diff->setLineActions(tr("Unstage"), [w](const DiffView::Selection &s) { w->unstageLines(s); });
                return;
            }
            // Lines are written back as UTF-8, which would mangle anything else.
            const bool editable = mayEdit && d.onDiskOk && isUtf8(d.onDisk) && !d.onDisk.startsWith("\xEF\xBB\xBF");
            w->m_diffLoaded = editable ? d.onDisk : QByteArray();
            w->m_diffBase = d.index;   // what the left side is, and what edits are re-diffed against
            w->m_diffCr = d.diffCr;
            w->m_wsBase = d.index;
            const ImageFetch images = [d] {
                ImageSides s;
                s.hasBefore = d.inIndex;
                s.before = d.index;
                s.hasAfter = d.onDiskOk;
                s.after = d.onDisk;
                return s;
            };
            w->m_diff->setPaneCaptions(e.untracked() ? tr("Not in git yet") : partlyStaged ? tr("Staged") : tr("Last commit"),
                                       tr("Working tree"));
            w->m_diff->showDiff(partlyStaged ? title + tr("  (unstaged changes: against what's staged)") : title,
                                d.diff, editable, images);
            // Git may show the file through filters (autocrlf, textconv, ...). If
            // what the pane shows is not what is on disk, saving it would rewrite
            // the file as something else, so leave it read-only -- and don't
            // stage from it.
            const bool faithful = w->m_diff->rightLines() == DiffView::linesOf(w->m_diffLoaded);
            if (editable && !faithful)
                w->m_diff->setEditable(false);
            w->m_diff->setWhitespaceIssues(d.issues);   // on the lines staging this would add
            if (editable && faithful && isUtf8(d.index))
                w->m_diff->setLineActions(tr("Stage"), [w](const DiffView::Selection &s) { w->stageLines(s); });
        }, Qt::QueuedConnection);
    });
}

void CommitWindow::flagWhitespace(const QByteArray &text)
{
    m_diff->setWhitespaceIssues(m_repo.whitespaceIssues(m_wsRules, m_wsBase, text));
}

QString CommitWindow::diffBase() const
{
    return m_repo.commitBase(m_amend->isChecked());
}

// The working-tree side of modified and untracked files: those are a file on
// disk against a single "before" (the index's copy, or nothing). Staged rows
// show the index, deleted files have nothing to type into, and conflicts are
// for og resolve.
bool CommitWindow::canEditInDiff(const FileEntry &e) const
{
    if (e.staged || e.conflicted())
        return false;
    if (!e.untracked() && e.worktree != u'M')
        return false;
    return QFileInfo(QDir(m_repo.root()).filePath(e.path)).isFile();
}

// The edited buffer against HEAD's copy, through the same git diff as
// everything else so the alignment matches.
QByteArray CommitWindow::rediffEdited(const QStringList &lines)
{
    if (!m_tmp)
        m_tmp = std::make_unique<QTemporaryDir>();
    const QString head = m_tmp->filePath(QStringLiteral("head"));
    const QString edited = m_tmp->filePath(QStringLiteral("edited"));
    QFile a(head), b(edited);
    if (!a.open(QIODevice::WriteOnly | QIODevice::Truncate) || !b.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    const QString eol = m_diffCr ? QStringLiteral("\r\n") : QStringLiteral("\n");
    QByteArray text = lines.join(eol).toUtf8();
    if (!lines.isEmpty() && m_diffLoaded.endsWith('\n'))
        text += eol.toUtf8();
    a.write(m_diffBase);
    b.write(text);
    a.close();
    b.close();
    // The marks follow the edits, checked against the bytes as they would be saved.
    flagWhitespace(DiffView::compose(lines, m_diffLoaded));
    return m_repo.diffFiles(head, edited);
}

bool CommitWindow::writeEdited(const QStringList &lines)
{
    const QString file = QDir(m_repo.root()).filePath(m_diffPath);
    QFile in(file);
    const QByteArray onDisk = in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
    in.close();
    if (onDisk != m_diffLoaded) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("%1 changed on disk").arg(m_diffPath));
        box.setText(tr("%1 has changed on disk since it was opened here.").arg(m_diffPath));
        box.setInformativeText(tr("Saving will overwrite that change with your edits."));
        box.setStandardButtons(QMessageBox::Save | QMessageBox::Cancel);
        box.button(QMessageBox::Save)->setText(tr("Overwrite"));
        box.setDefaultButton(QMessageBox::Cancel);
        if (box.exec() != QMessageBox::Save)
            return false;
    }

    const QByteArray data = DiffView::compose(lines, m_diffLoaded);
    // Truncate in place rather than replace, so permissions and symlinks survive.
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(data) != data.size()) {
        showError(tr("Could not save %1").arg(m_diffPath), out.errorString());
        return false;
    }
    m_diffLoaded = data;
    m_status->setText(tr("Saved %1").arg(m_diffPath));
    return true;
}

void CommitWindow::saveEdited()
{
    if (m_diff->isDirty() && m_diff->save())
        refresh();   // the file's status may have changed, or it may now be clean
}

void CommitWindow::showFileMenu(const QPoint &pos)
{
    const FileEntry *entry = entryOf(m_files->itemAt(pos));
    if (!entry)
        return;
    const FileEntry e = *entry;

    QMenu menu(this);
    QAction *resolve = nullptr, *stage = nullptr, *unstage = nullptr, *discard = nullptr, *revert = nullptr;
    if (e.conflicted()) {
        resolve = menu.addAction(tr("Resolve…"));
        resolve->setEnabled(!m_busy);
    }
    if (e.staged) {
        unstage = menu.addAction(tr("Unstage"));
        unstage->setEnabled(!m_busy && canUnstage(e));
        if (!canUnstage(e))
            unstage->setText(tr("Unstage (it is only in the last commit)"));
        revert = menu.addAction(tr("Revert to the last commit…"));
        revert->setEnabled(!m_busy && canRevert(e));
    } else {
        stage = menu.addAction(tr("Stage"));
        stage->setEnabled(!m_busy && !e.conflicted());
        discard = menu.addAction(tr("Discard unstaged changes…"));
        discard->setEnabled(!m_busy && !e.untracked() && !e.conflicted());
    }
    QAction *chosen = menu.exec(m_files->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == stage) {
        stageFile(e);
    } else if (chosen == unstage) {
        unstageFile(e);
    } else if (chosen == discard) {
        discardUnstaged(e);
    } else if (chosen == revert) {
        revertFile(e);
    } else if (chosen == resolve) {
        // In this window's place; the list is refreshed on coming back.
        OgWindow::go(this, OgWindow::Resolve, e.path);
    }
}

// Staging and unstaging change what the list and the diff show. Unsaved edits
// to the file were measured against the old index, so they are settled first.
void CommitWindow::afterIndexChange(const GitResult &r, const QString &failure, const QString &done)
{
    if (!r.ok()) {
        showError(failure, QString::fromUtf8(r.err));
    } else {
        m_status->setText(done);
    }
    refresh();
}

void CommitWindow::stageFile(const FileEntry &e)
{
    if (m_busy || (m_diffPath == e.path && !resolveUnsavedEdits()))
        return;
    afterIndexChange(m_repo.run({QStringLiteral("add"), QStringLiteral("-A"), QStringLiteral("--"), e.path}),
                     tr("Could not stage %1").arg(e.path), tr("Staged %1").arg(e.path));
}

// While amending, a staged row can be one the last commit made and the index
// doesn't change further; there is nothing to unstage (untick it to drop it).
bool CommitWindow::canUnstage(const FileEntry &e) const
{
    return e.staged && (!m_amend->isChecked() || m_indexChanged.contains(e.path));
}

void CommitWindow::unstageFile(const FileEntry &e)
{
    if (m_busy || !canUnstage(e))
        return;
    QStringList paths{e.path};
    if (!e.oldPath.isEmpty())
        paths << e.oldPath;   // a rename: its old name comes back too
    QStringList args = m_repo.hasHead()
        ? QStringList{QStringLiteral("restore"), QStringLiteral("--staged"), QStringLiteral("--")}
        : QStringList{QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-q"), QStringLiteral("--")};
    afterIndexChange(m_repo.run(args << paths), tr("Could not unstage %1").arg(e.path),
                     tr("Unstaged %1").arg(e.path));
}

// The picked lines are rows of the diff on screen, which was made from the
// index as it was then. If something else has changed it since, they no
// longer line up: show it afresh rather than write a garbled file.
bool CommitWindow::indexMovedOn(const QByteArray &index)
{
    if (index == m_shownIndex)
        return false;
    m_status->setText(tr("%1 changed in the index meanwhile — showing it again; try once more").arg(m_diffPath));
    refresh();
    return true;
}

// Stage a block or lines: the index's copy takes them from the working tree.
void CommitWindow::stageLines(const DiffView::Selection &picked)
{
    if (m_busy || m_diffStaged || m_diff->isDirty())
        return;
    bool inIndex = false;
    const QByteArray index = m_repo.indexBlob(m_diffPath, &inIndex);
    if (indexMovedOn(index))
        return;
    const QStringList lines = m_diff->linesApplied(picked, true, DiffView::linesOf(index));
    // Line endings follow the index's copy; a file new to it, the one on disk.
    const QByteArray data = DiffView::compose(lines, inIndex ? index : m_diffLoaded);
    afterIndexChange(m_repo.setIndexContent(m_diffPath, data), tr("Could not stage the change"),
                     tr("Staged part of %1").arg(m_diffPath));
}

// Unstage a block or lines: the index's copy goes back to the base's version of them.
void CommitWindow::unstageLines(const DiffView::Selection &picked)
{
    if (m_busy || !m_diffStaged)
        return;
    const FileEntry *e = entryOf(m_files->currentItem());
    const QByteArray index = m_repo.indexBlob(m_diffPath);
    if (indexMovedOn(index))
        return;
    const QStringList lines = m_diff->linesApplied(picked, false, DiffView::linesOf(index));
    GitResult r;
    if (lines.isEmpty() && e && e->index == u'A')
        // All of a new file unstaged: it is no longer added, rather than added empty.
        r = m_repo.run({QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-q"), QStringLiteral("--"),
                        m_diffPath});
    else
        r = m_repo.setIndexContent(m_diffPath, DiffView::compose(lines, index));
    afterIndexChange(r, tr("Could not unstage the change"), tr("Unstaged part of %1").arg(m_diffPath));
}

// Back to the last commit, from a staged row. While amending, a row only in
// the commit being amended has nothing pending to revert.
bool CommitWindow::canRevert(const FileEntry &e) const
{
    return e.staged && canUnstage(e);
}

// TortoiseGit's revert: back to HEAD, staged and unstaged changes alike. A file
// that is new to the index -- added, or the new name of a rename or copy -- is
// only taken out of the index, and stays on disk as untracked.
void CommitWindow::revertFile(const FileEntry &e)
{
    if (!canRevert(e) || m_busy)
        return;

    const bool editing = m_diffPath == e.path && m_diff->isDirty();
    // A file new to the index -- added, or the new name of a rename or copy.
    const bool newInIndex = e.index == u'A' || e.index == u'R' || e.index == u'C' || !m_repo.hasHead();
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Revert %1").arg(e.path));
    box.setText(tr("Revert %1 to the last commit?").arg(e.path));
    QString what = newInIndex ? tr("It will no longer be added; the file stays on disk as untracked.")
                              : tr("Its uncommitted changes will be lost.");
    if (editing)
        what += QLatin1Char(' ') + tr("Your unsaved edits in the diff will be lost.");
    box.setInformativeText(what);
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(tr("Revert"));
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Ok)
        return;

    QStringList fromHead, unstage;
    (newInIndex ? unstage : fromHead) << e.path;
    if (!e.oldPath.isEmpty() && e.index == u'R')
        fromHead << e.oldPath;   // the old name comes back

    GitResult r;
    r.exitCode = 0;
    if (!fromHead.isEmpty()) {
        QStringList args{QStringLiteral("restore"), QStringLiteral("--source=HEAD"), QStringLiteral("--staged"),
                         QStringLiteral("--worktree"), QStringLiteral("--")};
        r = m_repo.run(args << fromHead);
    }
    if (r.ok() && !unstage.isEmpty()) {
        QStringList args{QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("--quiet"), QStringLiteral("--")};
        r = m_repo.run(args << unstage);
    }
    if (!r.ok()) {
        showError(tr("Could not revert %1").arg(e.path), QString::fromUtf8(r.err));
        refresh();   // part of it may have gone through
        return;
    }
    if (editing)
        m_diff->discard();   // the file they applied to is gone
    m_status->setText(tr("Reverted %1").arg(e.path));
    refresh();
}

// Discard unstaged changes: the working tree back to the index's copy, so what
// is staged stays staged.
void CommitWindow::discardUnstaged(const FileEntry &e)
{
    if (m_busy || e.staged || e.untracked() || e.conflicted())
        return;
    const bool editing = m_diffPath == e.path && !m_diffStaged && m_diff->isDirty();
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Discard changes to %1").arg(e.path));
    box.setText(tr("Discard the unstaged changes to %1?").arg(e.path));
    QString what = tr("The file goes back to what is staged (or to the last commit, if nothing is). "
                      "Its other changes will be lost.");
    if (editing)
        what += QLatin1Char(' ') + tr("Your unsaved edits in the diff will be lost.");
    box.setInformativeText(what);
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(tr("Discard"));
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Ok)
        return;
    const GitResult r = m_repo.run({QStringLiteral("restore"), QStringLiteral("--"), e.path});
    if (r.ok() && editing)
        m_diff->discard();
    afterIndexChange(r, tr("Could not discard changes to %1").arg(e.path), tr("Discarded changes to %1").arg(e.path));
}

// Asks what to do with unsaved diff edits. Returns false when the caller
// should not go ahead.
bool CommitWindow::resolveUnsavedEdits(bool allowCancel)
{
    if (!m_diff->isDirty())
        return true;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Unsaved changes"));
    box.setText(tr("Save your edits to %1?").arg(m_diffPath));
    auto buttons = QMessageBox::Save | QMessageBox::Discard;
    if (allowCancel)
        buttons |= QMessageBox::Cancel;
    box.setStandardButtons(buttons);
    // The platform theme may call it "Close without Saving", which is wrong
    // when this is asked on the way to another file or a commit.
    box.button(QMessageBox::Discard)->setText(tr("Discard"));
    box.setDefaultButton(QMessageBox::Save);
    for (;;) {
        const int choice = box.exec();
        if (choice == QMessageBox::Discard) {
            m_diff->discard();
            return true;
        }
        if (choice == QMessageBox::Save) {
            if (m_diff->save())
                return true;
            if (allowCancel)
                return false;
            continue;   // could not save and cannot stay: ask again rather than lose them
        }
        return false;
    }
}

QColor CommitWindow::statusColor(const FileEntry &e) const
{
    const ThemeColors &c = Theme::instance().colors();
    const QString s = e.statusText();
    if (s == QLatin1String("Untracked")) return c.muted;
    if (s == QLatin1String("Added")) return c.green;
    if (s == QLatin1String("Deleted") || s == QLatin1String("Conflicted")) return c.red;
    if (s == QLatin1String("Renamed") || s == QLatin1String("Copied")) return c.accent;
    return c.yellow;
}

void CommitWindow::applyTheme()
{
    m_diff->applyTheme();
    m_message->applyTheme();
    m_updatingChecks = true;
    for (QTreeWidgetItem *it : fileItems())
        it->setForeground(1, statusColor(*entryOf(it)));
    for (int i = 0; i < m_files->topLevelItemCount(); ++i)
        m_files->topLevelItem(i)->setForeground(1, Theme::instance().colors().muted);
    m_updatingChecks = false;
    updateCounts();
}

void CommitWindow::applyFilter()
{
    const QString f = m_filter->text().trimmed();
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        QTreeWidgetItem *section = m_files->topLevelItem(i);
        bool any = false;
        for (int j = 0; j < section->childCount(); ++j) {
            QTreeWidgetItem *it = section->child(j);
            it->setHidden(!f.isEmpty() && !it->text(0).contains(f, Qt::CaseInsensitive));
            any = any || !it->isHidden();
        }
        section->setHidden(!any);   // a heading over nothing is noise
    }
    updateSelectAllState();   // a filter matching nothing leaves it nothing to toggle
}

void CommitWindow::toggleAll()
{
    bool anyUnchecked = false;
    for (QTreeWidgetItem *it : fileItems())
        if (!it->isHidden() && it->checkState(0) != Qt::Checked)
            anyUnchecked = true;
    m_updatingChecks = true;
    for (QTreeWidgetItem *it : fileItems())
        if (!it->isHidden())
            it->setCheckState(0, anyUnchecked ? Qt::Checked : Qt::Unchecked);
    m_updatingChecks = false;
    updateSelectAllState();
    updateCounts();
}

void CommitWindow::updateSelectAllState()
{
    const QVector<QTreeWidgetItem *> rows = fileItems();
    int checked = 0;
    bool anyShown = false;
    for (QTreeWidgetItem *it : rows) {
        if (it->checkState(0) == Qt::Checked)
            ++checked;
        anyShown = anyShown || !it->isHidden();
    }
    QSignalBlocker block(m_selectAll);
    m_selectAll->setCheckState(checked == 0 ? Qt::Unchecked
                               : checked == rows.size() ? Qt::Checked
                                                        : Qt::PartiallyChecked);
    // It toggles the rows on show, so with none showing -- nothing changed,
    // or a filter matching nothing -- there is nothing for it to do.
    m_selectAll->setEnabled(!m_busy && anyShown);
}

// Returns false when the commit must not go ahead. The branch is created as a
// separate step rather than folded into the commit, so everything downstream --
// the scratch index, the merge path, push -u -- works the same on a new branch as on an
// existing one.
bool CommitWindow::prepareBranch()
{
    const QString name = m_newBranch->text().trimmed();
    if (name.isEmpty())
        return true;

    if (!m_repo.isValidBranchName(name)) {
        showError(tr("Invalid branch name"),
                  tr("\u201c%1\u201d is not a valid Git branch name.").arg(name));
        m_newBranch->setFocus();
        return false;
    }
    if (m_repo.branchExists(name)) {
        showError(tr("Branch already exists"),
                  tr("A branch named \u201c%1\u201d already exists. Choose another name, or clear "
                     "the field to commit to the current branch.").arg(name));
        m_newBranch->setFocus();
        return false;
    }
    const GitResult r = m_repo.createBranch(name);
    if (!r.ok()) {
        showError(tr("Could not create branch"), QString::fromUtf8(r.err));
        m_newBranch->setFocus();
        return false;
    }
    return true;
}

void CommitWindow::updateCounts()
{
    const ThemeColors &c = Theme::instance().colors();
    const QString msg = m_message->toPlainText();
    const int subject = msg.section(u'\n', 0, 0).size();
    m_counter->setText(QStringLiteral("%1/50").arg(subject));
    m_counter->setStyleSheet(QStringLiteral("color:%1").arg(
        (subject > 72 ? c.red : subject > 50 ? c.yellow : c.muted).name()));

    const QVector<QTreeWidgetItem *> rows = fileItems();
    int checked = 0;
    for (QTreeWidgetItem *it : rows)
        if (it->checkState(0) == Qt::Checked)
            ++checked;
    m_fileCount->setText(tr("%1 of %2 selected").arg(checked).arg(rows.size()));

    const bool can = !m_busy && !m_writer && !msg.trimmed().isEmpty() && (checked > 0 || m_amend->isChecked());
    m_commitBtn->setEnabled(can);
    m_pushBtn->setEnabled(can);
}

void CommitWindow::onAmendToggled(bool on)
{
    // The diff's left side switches between HEAD and its parent, which unsaved
    // edits were measured against; settle them before it moves.
    if (m_diff->isDirty() && !resolveUnsavedEdits()) {
        QSignalBlocker block(m_amend);
        m_amend->setChecked(!on);
        return;
    }
    if (on) {
        m_lastMessage = m_repo.lastCommitMessage();
        if (m_message->toPlainText().trimmed().isEmpty())
            setMessage(m_lastMessage);
    } else if (m_message->toPlainText().trimmed() == m_lastMessage) {
        m_message->clear();
    }
    // Amending rewrites the commit HEAD already points at, which is not
    // something you can also redirect onto a branch that does not exist yet.
    m_newBranch->setEnabled(!on);
    m_newBranch->setToolTip(on ? tr("Not available while amending")
                               : tr("Create this branch from the current HEAD and commit to it"));
    if (on)
        m_newBranch->clear();
    refresh();   // the last commit's files join or leave the list
    updateCounts();
}

void CommitWindow::commit(bool push)
{
    if (m_busy || !m_commitBtn->isEnabled())
        return;
    // The commit takes files from disk, so unsaved diff edits would be left out.
    if (m_diff->isDirty()) {
        if (!resolveUnsavedEdits())
            return;
        refresh();
    }

    const QString msg = m_message->toPlainText().trimmed();
    const bool amend = m_amend->isChecked();
    // A ticked staged row commits what is staged; a ticked change row commits
    // the file as it is on disk. Anything unticked stays as the base has it
    // (HEAD, or its parent when amending -- which drops it from the commit).
    QStringList fromIndex, fromWorktree;
    for (QTreeWidgetItem *it : fileItems()) {
        if (it->checkState(0) != Qt::Checked)
            continue;
        const FileEntry &e = *entryOf(it);
        if (e.staged) {
            fromIndex << e.path;
            if (!e.oldPath.isEmpty())
                fromIndex << e.oldPath;
        } else {
            fromWorktree << e.path;
        }
    }
    if (msg.isEmpty() || (fromIndex.isEmpty() && fromWorktree.isEmpty() && !amend))
        return;

    if (!prepareBranch())
        return;

    const bool merging = m_repo.isMerging();
    setBusy(true, tr("Committing…"));

    // Normally the commit is built in a scratch index, so what is staged but
    // unticked stays staged afterwards. Mid-merge git wants the whole index
    // committed: the ticked changes are staged into it and it all goes.
    QString indexFile;
    if (merging) {
        if (!fromWorktree.isEmpty()) {
            QStringList args{QStringLiteral("add"), QStringLiteral("-A"), QStringLiteral("--")};
            const GitResult r = m_repo.run(args << fromWorktree);
            if (!r.ok()) {
                setBusy(false, tr("Nothing committed"));
                showError(tr("Could not stage files"), QString::fromUtf8(r.err));
                return;
            }
        }
    } else {
        indexFile = m_repo.scratchIndexPath();
        if (indexFile.isEmpty()) {
            setBusy(false, tr("Nothing committed"));
            showError(tr("Could not commit"), tr("Could not locate the repository's git directory."));
            return;
        }
        QFile::remove(indexFile);
        const GitResult r = m_repo.prepareCommitIndex(indexFile, diffBase(), fromIndex, fromWorktree);
        if (!r.ok()) {
            QFile::remove(indexFile);
            setBusy(false, tr("Nothing committed"));
            showError(tr("Could not commit"), QString::fromUtf8(r.err));
            return;
        }
    }

    saveToHistory(msg);

    // Async so long-running hooks (linters, tests) don't freeze the window.
    // Afterwards the real index takes what was committed from the working
    // tree; what was committed from the index is already there.
    const QStringList touched = merging ? QStringList() : fromWorktree;
    auto *proc = new QProcess(this);
    m_repo.configure(*proc, indexFile);
    connect(proc, &QProcess::finished, this, [this, proc, push, indexFile, touched](int code, QProcess::ExitStatus st) {
        const QString output = QString::fromUtf8(proc->readAllStandardOutput() + proc->readAllStandardError());
        proc->deleteLater();
        if (!indexFile.isEmpty())
            QFile::remove(indexFile);
        if (st != QProcess::NormalExit || code != 0) {
            setBusy(false, tr("Commit failed"));
            showError(tr("Commit failed"), output);
            refresh();   // a new branch may have been created before the failure
            return;
        }
        if (!touched.isEmpty()) {
            QStringList args{QStringLiteral("reset"), QStringLiteral("-q"), QStringLiteral("--")};
            m_repo.run(args << touched);
        }
        const QString hash = QString::fromUtf8(
            m_repo.run({QStringLiteral("rev-parse"), QStringLiteral("--short"), QStringLiteral("HEAD")}).out).trimmed();
        QSettings().remove(draftKey());
        {
            QSignalBlocker block(m_amend);
            m_amend->setChecked(false);
        }
        m_message->clear();
        m_newBranch->clear();
        if (push) {
            setBusy(true, tr("Committed %1, pushing…").arg(hash));
            startPush();
        } else {
            setBusy(false, tr("Committed %1").arg(hash));
            finishAfterSuccess();
        }
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, indexFile](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart)
            return;
        proc->deleteLater();
        if (!indexFile.isEmpty())
            QFile::remove(indexFile);
        setBusy(false, tr("Commit failed"));
        showError(tr("Commit failed"), tr("Could not start git."));
    });
    QStringList args{QStringLiteral("commit"), QStringLiteral("-F"), QStringLiteral("-")};
    if (amend)
        args << QStringLiteral("--amend");
    proc->start(QStringLiteral("git"), args);
    proc->write(msg.toUtf8());
    proc->closeWriteChannel();
}

void CommitWindow::startPush()
{
    QStringList args{QStringLiteral("push")};
    if (m_repo.upstream().isEmpty()) {
        // First push of a new branch: set upstream like `git push -u origin <branch>`.
        const QString branch = m_repo.branch();
        const QStringList remotes = m_repo.remotes();
        if (!branch.isEmpty() && !remotes.isEmpty()) {
            const QString remote = remotes.contains(QStringLiteral("origin")) ? QStringLiteral("origin") : remotes.first();
            args << QStringLiteral("-u") << remote << branch;
        }
    }

    auto *proc = new QProcess(this);
    m_repo.configure(*proc);
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus st) {
        const QString output = QString::fromUtf8(proc->readAllStandardOutput() + proc->readAllStandardError());
        proc->deleteLater();
        if (st != QProcess::NormalExit || code != 0) {
            setBusy(false, tr("Committed, but push failed"));
            showError(tr("Push failed"), output);
            refresh();
            return;
        }
        setBusy(false, tr("Committed and pushed"));
        finishAfterSuccess();
    });
    proc->start(QStringLiteral("git"), args);
    proc->closeWriteChannel();
}

void CommitWindow::finishAfterSuccess()
{
    refresh();
    if (m_entries.isEmpty())
        QTimer::singleShot(700, this, [this] { OgWindow::back(this); });   // nothing left: get out of the way
}

void CommitWindow::setBusy(bool busy, const QString &message)
{
    if (busy != m_busy) {
        if (busy)
            QApplication::setOverrideCursor(Qt::BusyCursor);
        else
            QApplication::restoreOverrideCursor();
    }
    m_busy = busy;
    m_message->setEnabled(!busy);
    m_files->setEnabled(!busy);
    m_amend->setEnabled(!busy);
    m_newBranch->setEnabled(!busy && !m_amend->isChecked());
    updateSelectAllState();
    m_historyBtn->setEnabled(!busy);
    updateWriteButton();
    if (!message.isNull())
        m_status->setText(message);
    if (!busy)
        m_message->setFocus();
    updateCounts();
}

void CommitWindow::showError(const QString &title, const QString &details)
{
    QString text = details.trimmed();
    if (text.isEmpty())
        text = tr("git did not report a reason.");
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(title);
    box.setText(title);
    box.setInformativeText(text.left(1500));
    if (text.size() > 1500)
        box.setDetailedText(text);
    box.exec();
}

// Text put in the message for you -- a saved draft, git's prepared message,
// the last message when amending, a recent one -- leaves the cursor at its
// end, where you would carry on typing.
void CommitWindow::setMessage(const QString &text)
{
    m_message->setPlainText(text);
    m_message->moveCursor(QTextCursor::End);
}

void CommitWindow::updateWriteButton()
{
    if (m_writer) {
        m_writeBtn->setText(tr("■ Stop"));
        m_writeBtn->setToolTip(tr("Stop %1").arg(m_agent.name));
        m_writeBtn->setEnabled(true);
        if (m_agentMenuBtn)
            m_agentMenuBtn->setEnabled(false);
        return;
    }
    m_writeBtn->setText(tr("✨ Write · %1").arg(m_agent.name));
    m_writeBtn->setEnabled(!m_busy && m_agent.usable());
    if (m_agentMenuBtn) {
        m_agentMenuBtn->setEnabled(!m_busy);
        m_agentMenuBtn->setToolTip(tr("Agent: %1 — choose another").arg(m_agent.name));
        for (QAction *choice : m_agentMenuBtn->menu()->actions())
            choice->setChecked(choice->text() == m_agent.name);
    }
    if (m_agent.usable())
        m_writeBtn->setToolTip(tr("Write a commit message for the checked changes with %1.\n"
                                  "Sends their diff to it; nothing is sent until you click.").arg(m_agent.name));
    else
        m_writeBtn->setToolTip(tr("Install Claude Code, Codex or OpenCode on PATH to write messages."));
}

// What the agent is asked: the checked changes as they will be committed --
// against the parent when amending -- the recent subjects for the house
// style, and the draft if there is one. Empty when nothing is checked.
QString CommitWindow::messagePrompt() const
{
    // What each ticked row commits: a change row the file on disk, a staged
    // row what is staged -- unless the file's change row is ticked too.
    QStringList worktree, staged, untracked;
    for (QTreeWidgetItem *it : fileItems()) {
        if (it->checkState(0) != Qt::Checked)
            continue;
        const FileEntry &e = *entryOf(it);
        if (e.untracked())
            untracked << e.path;
        else if (!e.staged)
            worktree << e.path;
    }
    for (QTreeWidgetItem *it : fileItems()) {
        const FileEntry &e = *entryOf(it);
        if (e.staged && it->checkState(0) == Qt::Checked && !worktree.contains(e.path)) {
            staged << e.path;
            if (!e.oldPath.isEmpty())
                staged << e.oldPath;
        }
    }
    auto baseDiff = [this](bool cached, const QStringList &paths) {
        QStringList args{QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                         QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"), QStringLiteral("-M")};
        if (cached)
            args << QStringLiteral("--cached");
        args << diffBase() << QStringLiteral("--") << paths;
        return QString::fromUtf8(m_repo.run(args).out);
    };
    QString diff;
    if (!staged.isEmpty())
        diff += baseDiff(true, staged);
    if (!worktree.isEmpty())
        diff += baseDiff(false, worktree);
    for (const QString &p : untracked)
        diff += QString::fromUtf8(m_repo.run({QStringLiteral("diff"), QStringLiteral("--no-color"),
                                              QStringLiteral("--no-index"), QStringLiteral("--"),
                                              QStringLiteral("/dev/null"), p}).out);
    if (diff.trimmed().isEmpty())
        return {};
    constexpr int limit = 60000;   // enough to describe a change; a huge diff only slows the reply
    if (diff.size() > limit)
        diff = diff.left(limit) + tr("\n[… diff truncated: %1 more characters]\n").arg(diff.size() - limit);

    QString prompt = QStringLiteral(
        "Write a git commit message for the changes below.\n\n"
        "- First line: a summary of at most 50 characters, in the imperative mood (\"Add\", \"Fix\"), "
        "with no trailing period.\n"
        "- If the change needs explaining, add a blank line and a body wrapped at 72 columns that says "
        "what changed and why.\n"
        "- Follow the style of the recent commit messages if they show one.\n"
        "- Reply with only the commit message: no code fences, no quotes, no preamble.\n"
        "- Everything you need is here; do not run commands or read files.\n");
    const QString draft = m_message->toPlainText().trimmed();
    if (m_amend->isChecked() && !m_lastMessage.isEmpty())
        prompt += QStringLiteral("\nThis amends the previous commit, whose message was:\n") + m_lastMessage + u'\n';
    if (!draft.isEmpty() && draft != m_lastMessage)
        prompt += QStringLiteral("\nThe author's draft, whose intent to keep:\n") + draft + u'\n';
    if (m_repo.hasHead()) {
        const QString recent = QString::fromUtf8(
            m_repo.run({QStringLiteral("log"), QStringLiteral("-12"), QStringLiteral("--format=- %s")}).out).trimmed();
        if (!recent.isEmpty())
            prompt += QStringLiteral("\nRecent commit messages in this repository:\n") + recent + u'\n';
    }
    return prompt + QStringLiteral("\nThe changes:\n\n") + diff;
}

void CommitWindow::writeMessage()
{
    if (m_writer) {   // Stop
        m_writeStopped = true;
        m_writer->kill();
        return;
    }
    if (!m_agent.usable() || m_busy)
        return;
    const QString prompt = messagePrompt();
    if (prompt.isEmpty()) {
        m_status->setText(tr("Check the files the message should describe"));
        return;
    }
    if (!m_tmp)
        m_tmp = std::make_unique<QTemporaryDir>();
    const QString replyFile = m_tmp->filePath(QStringLiteral("reply"));
    QFile::remove(replyFile);
    if (m_agent.id == u"opencode") {
        QFile promptFile(replyFile);
        if (!promptFile.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            promptFile.write(prompt.toUtf8()) != prompt.toUtf8().size()) {
            showError(tr("Could not prepare the prompt for OpenCode"), promptFile.errorString());
            return;
        }
        promptFile.close();
    }

    m_writeStopped = false;
    m_writer = new QProcess(this);
    m_writer->setWorkingDirectory(m_repo.root());
    if (m_agent.id == u"opencode") {
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("OPENCODE_PERMISSION"), QStringLiteral("{\"*\":\"deny\"}"));
        env.insert(QStringLiteral("OPENCODE_DISABLE_DEFAULT_PLUGINS"), QStringLiteral("true"));
        m_writer->setProcessEnvironment(env);
    }
    auto *limit = new QTimer(m_writer);   // a stuck agent shouldn't hold the dialog forever
    limit->setSingleShot(true);
    connect(limit, &QTimer::timeout, m_writer, &QProcess::kill);
    limit->start(180000);

    auto done = [this, replyFile](const QString &error) {
        const bool stopped = m_writeStopped;
        QString reply = m_agent.replyInFile()
            ? [&] { QFile f(replyFile); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString(); }()
            : QString::fromUtf8(m_writer->readAllStandardOutput());
        if (m_agent.id == u"opencode") {
            QString text;
            for (const QByteArray &line : reply.toUtf8().split('\n')) {
                const QJsonDocument event = QJsonDocument::fromJson(line);
                if (!event.isObject() || event.object().value(QStringLiteral("type")).toString() != u"text")
                    continue;
                const QJsonObject part = event.object().value(QStringLiteral("part")).toObject();
                text += part.value(QStringLiteral("text")).toString();
            }
            reply = text;
        }
        const QString err = QString::fromUtf8(m_writer->readAllStandardError()).trimmed();
        m_writer->deleteLater();
        m_writer = nullptr;
        m_message->setReadOnly(false);

        // Tidy what models sometimes add anyway: code fences, surrounding blank lines.
        reply = reply.trimmed();
        if (reply.startsWith(QLatin1String("```")))
            reply = reply.section(u'\n', 1);
        if (reply.endsWith(QLatin1String("```")))
            reply = reply.section(u'\n', 0, -2);
        reply = reply.trimmed();

        if (stopped) {
            m_status->setText(tr("Stopped"));
        } else if (error.isEmpty() && !reply.isEmpty()) {
            QTextCursor c(m_message->document());   // one undo step: Ctrl+Z brings the old text back
            c.beginEditBlock();
            c.select(QTextCursor::Document);
            c.insertText(reply);
            c.endEditBlock();
            m_message->setFocus();
            m_status->setText(tr("Written by %1 — check it before committing").arg(m_agent.name));
        } else {
            // Whatever it printed is the likeliest explanation (not logged in, no network, ...).
            const QString said = !err.isEmpty() ? err : reply;
            m_status->setText(tr("No message written"));
            showError(tr("%1 couldn't write a message").arg(m_agent.name),
                      (error.isEmpty() ? tr("It returned nothing.") : error)
                          + (said.isEmpty() ? QString() : QStringLiteral("\n\n") + said.right(1500)));
        }
        updateWriteButton();
        updateCounts();
    };
    // Only a clean exit counts: a failing agent may still print something --
    // an error message must never end up as the commit message.
    connect(m_writer, &QProcess::finished, this, [this, done](int code, QProcess::ExitStatus st) {
        done(m_writeStopped || (st == QProcess::NormalExit && code == 0) ? QString()
             : st != QProcess::NormalExit ? tr("It stopped after running too long, or crashed.")
                                          : tr("It exited with code %1.").arg(code));
    });
    connect(m_writer, &QProcess::errorOccurred, this, [this, done](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            done(tr("Could not start %1.").arg(m_agent.program()));
    });

    m_message->setReadOnly(true);
    m_status->setText(tr("Writing a message with %1…").arg(m_agent.name));
    m_writer->start(m_agent.program(), m_agent.arguments(replyFile));
    if (m_agent.id != u"opencode")
        m_writer->write(prompt.toUtf8());
    m_writer->closeWriteChannel();
    updateWriteButton();
    updateCounts();
}

void CommitWindow::rebuildHistoryMenu()
{
    m_historyMenu->clear();
    const QStringList history = QSettings().value(QStringLiteral("history")).toStringList();
    if (history.isEmpty()) {
        m_historyMenu->addAction(tr("No recent messages"))->setEnabled(false);
        return;
    }
    for (const QString &msg : history) {
        QString label = msg.section(u'\n', 0, 0);
        if (label.size() > 70)
            label = label.left(69) + QStringLiteral("…");
        auto *act = m_historyMenu->addAction(label.replace(u'&', QStringLiteral("&&")));
        act->setToolTip(msg);
        connect(act, &QAction::triggered, this, [this, msg] { setMessage(msg); });
    }
}

void CommitWindow::saveToHistory(const QString &message)
{
    QSettings s;
    QStringList history = s.value(QStringLiteral("history")).toStringList();
    history.removeAll(message);
    history.prepend(message);
    while (history.size() > MaxHistory)
        history.removeLast();
    s.setValue(QStringLiteral("history"), history);
}

// The left side is made wide enough for a 72-column message -- the guide the
// message box draws -- but kept between 30% and 45% of the window, so the diff
// always keeps most of it. Worked out once, when the font and width are known.
// A click on the checkbox of one of several selected files would, left to the
// view, also narrow the selection to that file. Tick them all and keep it.
bool CommitWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_files->viewport()
        && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick)) {
        auto *me = static_cast<QMouseEvent *>(event);
        QTreeWidgetItem *it = m_files->itemAt(me->position().toPoint());
        const int box = m_files->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, m_files) + 10;
        const bool onBox = it && entryOf(it) && me->button() == Qt::LeftButton && me->modifiers() == Qt::NoModifier
                        && me->position().x() < m_files->visualItemRect(it).left() + box;
        if (onBox && it->isSelected() && m_files->selectedItems().size() > 1) {
            if (event->type() == QEvent::MouseButtonRelease)
                it->setCheckState(0, it->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
            return true;   // the selection stays as it is
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CommitWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (m_sized) {
        refresh();   // back from the log or resolve, which may have changed things
        return;
    }
    m_sized = true;
    m_message->ensurePolished();
    const int total = m_split->width();
    const int text = m_message->fontMetrics().horizontalAdvance(QString(72, QLatin1Char('m')));
    const int want = text + 2 * int(m_message->document()->documentMargin()) + 2 * m_message->frameWidth()
                   + m_message->verticalScrollBar()->sizeHint().width() + 28;   // + the panel's margins
    const int left = qBound(int(total * 0.30), want, int(total * 0.45));
    m_split->setSizes({left, total - left});
}

void CommitWindow::closeEvent(QCloseEvent *e)
{
    if (m_busy || !resolveUnsavedEdits()) {   // don't abandon a running commit/push, or unsaved edits
        e->ignore();
        return;
    }
    QSettings s;
    const QString msg = m_message->toPlainText();
    if (msg.trimmed().isEmpty() || (m_amend->isChecked() && msg.trimmed() == m_lastMessage))
        s.remove(draftKey());
    else
        s.setValue(draftKey(), msg);
    e->accept();
}
