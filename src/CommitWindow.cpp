#include "CommitWindow.h"
#include "DiffView.h"
#include "ElidedLabel.h"
#include "MessageEdit.h"
#include "ResolveWindow.h"
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
    m_header = new QLabel;
    m_header->setObjectName(QStringLiteral("title"));
    m_header->setTextFormat(Qt::RichText);
    m_repoPath = new ElidedLabel(root);
    m_repoPath->setObjectName(QStringLiteral("muted"));

    // Writes a message with Omarchy's default coding agent (Claude Code or Codex).
    m_agent = Agent::detect();
    m_writeBtn = new QToolButton;
    m_writeBtn->setText(tr("✨ Write"));
    m_writeBtn->setVisible(!m_agent.id.isEmpty());

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

    m_selectAll = new QCheckBox(tr("Changes"));
    m_selectAll->setTristate(true);
    m_fileCount = new QLabel;
    m_fileCount->setObjectName(QStringLiteral("muted"));
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter files (Ctrl+F)"));
    m_filter->setClearButtonEnabled(true);

    m_files = new QTreeWidget;
    m_files->setColumnCount(2);
    m_files->setHeaderLabels({tr("Path"), tr("Status")});
    m_files->setRootIsDecorated(false);
    m_files->setUniformRowHeights(true);
    m_files->setSelectionMode(QAbstractItemView::SingleSelection);
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

    auto *head = new QVBoxLayout;
    head->setSpacing(2);
    head->addWidget(m_header);
    head->addWidget(m_repoPath);
    l->addLayout(head);
    l->addSpacing(4);

    auto *msgHead = new QHBoxLayout;
    msgHead->addWidget(sectionLabel(tr("Message")));
    msgHead->addStretch();
    msgHead->addWidget(m_writeBtn);
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
    split->addWidget(left);
    split->addWidget(m_diff);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
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
    connect(m_files, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *, int column) {
        if (!m_updatingChecks && column == 0) {
            updateSelectAllState();
            updateCounts();
        }
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
    new QShortcut(QKeySequence::Save, this, [this] { saveEdited(); });
    new QShortcut(QKeySequence(QStringLiteral("Esc")), this, [this] { close(); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+F")), this, [this] {
        m_filter->setFocus();
        m_filter->selectAll();
    });
    new QShortcut(QKeySequence(Qt::Key_Space), m_files, [this] {
        if (auto *it = m_files->currentItem())
            it->setCheckState(0, it->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
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

void CommitWindow::refresh()
{
    QSet<QString> known, checked;
    QString currentPath;
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        auto *it = m_files->topLevelItem(i);
        const QString p = m_entries.at(it->data(0, IndexRole).toInt()).path;
        known.insert(p);
        if (it->checkState(0) == Qt::Checked)
            checked.insert(p);
    }
    if (auto *cur = m_files->currentItem())
        currentPath = m_entries.at(cur->data(0, IndexRole).toInt()).path;

    m_entries = m_repo.status();

    if (m_amend->isChecked()) {
        // Amending replaces the last commit, so what it already contains belongs
        // in the list too: unchecking one of those rows drops it from the commit.
        QHash<QString, int> byPath;
        for (int i = 0; i < m_entries.size(); ++i)
            byPath.insert(m_entries.at(i).path, i);
        // They come first, in the commit's order, then the changes not in it yet.
        QVector<FileEntry> ordered;
        QSet<int> taken;
        const QVector<FileEntry> committed = m_repo.lastCommitFiles();
        for (const FileEntry &e : committed) {
            const auto it = byPath.constFind(e.path);
            if (it != byPath.constEnd()) {
                FileEntry merged = m_entries.at(it.value());
                merged.inLastCommit = true;
                merged.commitStatus = e.commitStatus;
                ordered.push_back(merged);
                taken.insert(it.value());
            }
            else
                ordered.push_back(e);
        }
        for (int i = 0; i < m_entries.size(); ++i)
            if (!taken.contains(i))
                ordered.push_back(m_entries.at(i));
        m_entries = ordered;
    }

    const QString branch = m_repo.branch();
    m_header->setText(branch.isEmpty() ? tr("Commit on <i>detached HEAD</i>")
                                       : tr("Commit to %1").arg(Theme::strong(branch)));
    if (m_repo.isMerging())
        m_header->setText(m_header->text() + tr(" <i>(merge)</i>"));

    m_updatingChecks = true;
    {
        QSignalBlocker block(m_files);
        m_files->clear();
    }
    QTreeWidgetItem *toSelect = nullptr;
    for (int i = 0; i < m_entries.size(); ++i) {
        const FileEntry &e = m_entries.at(i);
        auto *it = new QTreeWidgetItem;
        it->setText(0, e.oldPath.isEmpty() ? e.path : e.oldPath + QStringLiteral(" → ") + e.path);
        it->setToolTip(0, it->text(0));
        it->setText(1, e.statusText());
        it->setData(0, IndexRole, i);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        // TortoiseGit default: tracked changes on, untracked files off.
        const bool on = known.contains(e.path) ? checked.contains(e.path) : !e.untracked();
        it->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
        m_files->addTopLevelItem(it);
        if (e.path == currentPath)
            toSelect = it;
    }
    m_updatingChecks = false;

    applyTheme();
    applyFilter();
    updateSelectAllState();
    updateCounts();

    if (!toSelect && m_files->topLevelItemCount() > 0)
        toSelect = m_files->topLevelItem(0);
    if (toSelect)
        m_files->setCurrentItem(toSelect);
    else
        m_diff->showMessage({}, tr("Working tree clean"));
}

void CommitWindow::showCurrentDiff()
{
    auto *it = m_files->currentItem();
    const QString path = it ? m_entries.at(it->data(0, IndexRole).toInt()).path : QString();

    if (m_diff->isDirty()) {
        if (path == m_diffPath)
            return;   // a refresh re-selected the file being edited: keep the edits
        // Moving to another file. If the edited one is still listed, Cancel
        // can go back to it; if it has dropped out, only Save or Discard can.
        QTreeWidgetItem *editedItem = nullptr;
        for (int i = 0; i < m_files->topLevelItemCount() && !editedItem; ++i)
            if (m_entries.at(m_files->topLevelItem(i)->data(0, IndexRole).toInt()).path == m_diffPath)
                editedItem = m_files->topLevelItem(i);
        if (!resolveUnsavedEdits(editedItem != nullptr)) {
            QSignalBlocker block(m_files);
            m_files->setCurrentItem(editedItem);
            return;
        }
    }

    m_diffPath = path;
    m_diffLoaded.clear();
    m_diffBase.clear();
    if (!it) {
        m_diff->showMessage({}, tr("Select a file to see its changes"));
        return;
    }
    const FileEntry &e = m_entries.at(it->data(0, IndexRole).toInt());
    const QString base = diffBase();
    bool editable = canEditInDiff(e);
    if (editable) {
        QFile f(QDir(m_repo.root()).filePath(e.path));
        editable = f.open(QIODevice::ReadOnly);
        if (editable)
            m_diffLoaded = f.readAll();
        // Lines are written back as UTF-8, which would mangle anything else.
        QStringDecoder utf8(QStringDecoder::Utf8);
        [[maybe_unused]] const QString decoded = utf8(m_diffLoaded);   // decoding is what sets hasError()
        if (utf8.hasError() || m_diffLoaded.startsWith("\xEF\xBB\xBF"))
            editable = false;
    }
    if (editable) {
        const GitResult left = m_repo.run({QStringLiteral("cat-file"), QStringLiteral("blob"),
                                           base + QLatin1Char(':') + e.path});
        editable = left.ok();
        m_diffBase = left.out;
    }
    const QByteArray diff = m_repo.diff(e, base);
    // Whether git's diff kept the CRs (it drops them when core.autocrlf
    // normalises the file). Re-diffs of edits must see the file the same way,
    // or one keystroke would turn every line of a CRLF file into a change.
    m_diffCr = false;
    for (const QByteArray &line : diff.split('\n'))
        if ((line.startsWith('+') || line.startsWith(' ')) && !line.startsWith("+++") && line.endsWith('\r')) {
            m_diffCr = true;
            break;
        }
    // For an image: the base's version against the one on disk.
    const QString before = base + QLatin1Char(':') + (e.oldPath.isEmpty() ? e.path : e.oldPath);
    const QString onDiskPath = QDir(m_repo.root()).filePath(e.path);
    const ImageFetch images = [this, before, onDiskPath] {
        ImageSides s;
        const GitResult b = m_repo.run({QStringLiteral("cat-file"), QStringLiteral("blob"), before});
        s.hasBefore = b.ok();
        s.before = b.out;
        QFile f(onDiskPath);
        s.hasAfter = f.open(QIODevice::ReadOnly);
        if (s.hasAfter)
            s.after = f.readAll();
        return s;
    };
    m_diff->showDiff(it->text(0), diff, editable, images);
    // Git may show the file through filters (autocrlf, textconv, ...). If what
    // the pane shows is not what is on disk, saving it would rewrite the file
    // as something else, so leave it read-only.
    if (editable && m_diff->rightLines() != DiffView::linesOf(m_diffLoaded))
        m_diff->setEditable(false);

    // Whitespace problems on the lines this commit would add -- measured
    // against the same base as the diff (the parent when amending).
    m_wsRules = m_repo.whitespaceRules(e.path);
    m_wsBase.clear();
    if (!e.untracked() && e.index != u'A' && m_repo.hasHead()) {
        const GitResult b = m_repo.run({QStringLiteral("cat-file"), QStringLiteral("blob"),
                                        base + QLatin1Char(':') + (e.oldPath.isEmpty() ? e.path : e.oldPath)});
        if (b.ok())
            m_wsBase = b.out;
    }
    QByteArray onDisk = m_diffLoaded;
    if (onDisk.isEmpty()) {
        QFile f(QDir(m_repo.root()).filePath(e.path));
        if (f.open(QIODevice::ReadOnly))
            onDisk = f.readAll();
    }
    flagWhitespace(onDisk);
}

void CommitWindow::flagWhitespace(const QByteArray &text)
{
    m_diff->setWhitespaceIssues(m_repo.whitespaceIssues(m_wsRules, m_wsBase, text));
}

// What the left side of the diff is. Amending replaces HEAD, so the commit
// that results is measured against HEAD's parent; otherwise against HEAD.
QString CommitWindow::diffBase() const
{
    if (m_amend->isChecked() && m_repo.hasHead())
        return m_repo.headParent();
    return QStringLiteral("HEAD");
}

// Only plain modifications: for new, deleted, renamed or conflicted files the
// left side is missing or is not "the same file before". When amending, a file
// the commit changed counts too, as long as the commit modified it rather than
// adding, deleting or renaming it.
bool CommitWindow::canEditInDiff(const FileEntry &e) const
{
    auto plain = [](QChar c) { return c == u' ' || c == u'M'; };
    if (!plain(e.index) || !plain(e.worktree))
        return false;
    if (e.inLastCommit ? e.commitStatus != u'M' : !e.worktreeChange())
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
    auto *it = m_files->itemAt(pos);
    if (!it)
        return;
    const FileEntry e = m_entries.at(it->data(0, IndexRole).toInt());

    QMenu menu(this);
    QAction *resolve = nullptr;
    if (e.statusText() == QLatin1String("Conflicted")) {
        resolve = menu.addAction(tr("Resolve…"));
        resolve->setEnabled(!m_busy);
    }
    QAction *revert = menu.addAction(tr("Revert…"));
    revert->setEnabled(!m_busy && canRevert(e));
    QAction *chosen = menu.exec(m_files->viewport()->mapToGlobal(pos));
    if (chosen == revert) {
        revertFile(e);
    } else if (chosen && chosen == resolve) {
        // Its own window; the list is refreshed when it closes.
        auto *w = new ResolveWindow(m_repo.root(), e.path);
        w->setAttribute(Qt::WA_DeleteOnClose);
        connect(w, &QObject::destroyed, this, [this] { refresh(); });
        w->resize(1500, 900);
        w->show();
    }
}

// Untracked files have no committed version to go back to, and a row that is
// only in the commit being amended has nothing pending.
bool CommitWindow::canRevert(const FileEntry &e) const
{
    return e.worktreeChange() && !e.untracked();
}

// TortoiseGit's revert: back to HEAD, staged and unstaged changes alike. A file
// that is new to the index -- added, or the new name of a rename or copy -- is
// only taken out of the index, and stays on disk as untracked.
void CommitWindow::revertFile(const FileEntry &e)
{
    if (!canRevert(e) || m_busy)
        return;

    const bool editing = m_diffPath == e.path && m_diff->isDirty();
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
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        auto *it = m_files->topLevelItem(i);
        it->setForeground(1, statusColor(m_entries.at(it->data(0, IndexRole).toInt())));
    }
    m_updatingChecks = false;
    updateCounts();
}

void CommitWindow::applyFilter()
{
    const QString f = m_filter->text().trimmed();
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        auto *it = m_files->topLevelItem(i);
        it->setHidden(!f.isEmpty() && !it->text(0).contains(f, Qt::CaseInsensitive));
    }
    updateSelectAllState();   // a filter matching nothing leaves it nothing to toggle
}

void CommitWindow::toggleAll()
{
    bool anyUnchecked = false;
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        auto *it = m_files->topLevelItem(i);
        if (!it->isHidden() && it->checkState(0) != Qt::Checked)
            anyUnchecked = true;
    }
    m_updatingChecks = true;
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        auto *it = m_files->topLevelItem(i);
        if (!it->isHidden())
            it->setCheckState(0, anyUnchecked ? Qt::Checked : Qt::Unchecked);
    }
    m_updatingChecks = false;
    updateSelectAllState();
    updateCounts();
}

void CommitWindow::updateSelectAllState()
{
    int checked = 0;
    const int total = m_files->topLevelItemCount();
    for (int i = 0; i < total; ++i)
        if (m_files->topLevelItem(i)->checkState(0) == Qt::Checked)
            ++checked;
    QSignalBlocker block(m_selectAll);
    m_selectAll->setCheckState(checked == 0 ? Qt::Unchecked
                               : checked == total ? Qt::Checked
                                                  : Qt::PartiallyChecked);
    // It toggles the rows on show, so with none showing -- nothing changed,
    // or a filter matching nothing -- there is nothing for it to do.
    bool anyShown = false;
    for (int i = 0; i < total && !anyShown; ++i)
        anyShown = !m_files->topLevelItem(i)->isHidden();
    m_selectAll->setEnabled(!m_busy && anyShown);
}

// Returns false when the commit must not go ahead. The branch is created as a
// separate step rather than folded into the commit, so everything downstream --
// --only, the merge path, push -u -- works the same on a new branch as on an
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

    int checked = 0;
    const int total = m_files->topLevelItemCount();
    for (int i = 0; i < total; ++i)
        if (m_files->topLevelItem(i)->checkState(0) == Qt::Checked)
            ++checked;
    m_fileCount->setText(tr("%1 of %2 selected").arg(checked).arg(total));

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
    QStringList paths, untracked, dropped;
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        auto *it = m_files->topLevelItem(i);
        const FileEntry &e = m_entries.at(it->data(0, IndexRole).toInt());
        if (it->checkState(0) != Qt::Checked) {
            // Unchecking something the commit already has means taking it out.
            if (e.inLastCommit) {
                dropped << e.path;
                if (!e.oldPath.isEmpty())
                    dropped << e.oldPath;
            }
            continue;
        }
        if (!e.worktreeChange() && e.inLastCommit)
            continue;              // already in the tree being amended; nothing to take
        paths << e.path;
        if (!e.oldPath.isEmpty())
            paths << e.oldPath;
        if (e.untracked())
            untracked << e.path;
    }
    if (msg.isEmpty() || (paths.isEmpty() && dropped.isEmpty() && !amend))
        return;

    if (!prepareBranch())
        return;

    const bool merging = m_repo.isMerging();
    // Dropping files from the commit cannot be expressed with `commit --only`,
    // which only ever adds working-tree content on top of the tree it amends.
    const bool scratch = amend && !merging && !dropped.isEmpty();
    setBusy(true, tr("Committing…"));

    QString indexFile;
    if (scratch) {
        indexFile = m_repo.scratchIndexPath();
        if (indexFile.isEmpty()) {
            setBusy(false, tr("Nothing committed"));
            showError(tr("Could not amend"), tr("Could not locate the repository's git directory."));
            return;
        }
        QFile::remove(indexFile);
        const GitResult r = m_repo.prepareAmendIndex(indexFile, paths, dropped);
        if (!r.ok()) {
            QFile::remove(indexFile);
            setBusy(false, tr("Nothing committed"));
            showError(tr("Could not amend"), QString::fromUtf8(r.err));
            return;
        }
    } else {
        // New files have to be known to git before `commit --only` can take them;
        // during a merge everything checked is staged and committed together.
        const QStringList toAdd = merging ? paths : untracked;
        if (!toAdd.isEmpty()) {
            QStringList args{QStringLiteral("add"), QStringLiteral("-A"), QStringLiteral("--")};
            args << toAdd;
            const GitResult r = m_repo.run(args);
            if (!r.ok()) {
                setBusy(false, tr("Nothing committed"));
                showError(tr("Could not stage files"), QString::fromUtf8(r.err));
                return;
            }
        }
    }

    saveToHistory(msg);

    // Async so long-running hooks (linters, tests) don't freeze the window.
    QStringList touched = paths;
    touched << dropped;
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
        if (!indexFile.isEmpty())
            m_repo.refreshIndexAfterAmend(touched);
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
    // With the scratch index the tree is already exactly right, so the commit
    // takes the index as-is rather than naming paths.
    proc->start(QStringLiteral("git"),
                scratch ? QStringList{QStringLiteral("commit"), QStringLiteral("--amend"),
                                      QStringLiteral("-F"), QStringLiteral("-")}
                        : m_repo.commitArgs(paths, amend, merging));
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
        QTimer::singleShot(700, this, &QWidget::close);   // nothing left: get out of the way
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
        return;
    }
    m_writeBtn->setText(tr("✨ Write"));
    m_writeBtn->setEnabled(!m_busy && m_agent.usable());
    if (m_agent.usable())
        m_writeBtn->setToolTip(tr("Write a commit message for the checked changes with %1, your default agent.\n"
                                  "Sends their diff to it; nothing is sent until you click.").arg(m_agent.name));
    else if (!m_agent.supported)
        m_writeBtn->setToolTip(tr("Writing messages works with Claude Code or Codex; your default agent is %1.\n"
                                  "Change it with: omarchy default agent claude").arg(m_agent.name));
    else
        m_writeBtn->setToolTip(tr("%1 isn't installed yet. Open it once with: omarchy agent").arg(m_agent.name));
}

// What the agent is asked: the checked changes as they will be committed --
// against the parent when amending -- the recent subjects for the house
// style, and the draft if there is one. Empty when nothing is checked.
QString CommitWindow::messagePrompt() const
{
    QStringList tracked, untracked;
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        auto *it = m_files->topLevelItem(i);
        if (it->checkState(0) != Qt::Checked)
            continue;
        const FileEntry &e = m_entries.at(it->data(0, IndexRole).toInt());
        (e.untracked() ? untracked : tracked) << e.path;
        if (!e.oldPath.isEmpty())
            tracked << e.oldPath;
    }
    QString diff;
    if (!tracked.isEmpty()) {
        QStringList args{QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                         QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"), QStringLiteral("-M"),
                         m_repo.hasHead() ? diffBase() : m_repo.emptyTree(), QStringLiteral("--")};
        diff += QString::fromUtf8(m_repo.run(args << tracked).out);
    }
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

    m_writeStopped = false;
    m_writer = new QProcess(this);
    m_writer->setWorkingDirectory(m_repo.root());
    auto *limit = new QTimer(m_writer);   // a stuck agent shouldn't hold the dialog forever
    limit->setSingleShot(true);
    connect(limit, &QTimer::timeout, m_writer, &QProcess::kill);
    limit->start(180000);

    auto done = [this, replyFile](const QString &error) {
        const bool stopped = m_writeStopped;
        QString reply = m_agent.replyInFile()
            ? [&] { QFile f(replyFile); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString(); }()
            : QString::fromUtf8(m_writer->readAllStandardOutput());
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
void CommitWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (m_sized)
        return;
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
