#include "CommitWindow.h"
#include "DiffView.h"
#include "MessageEdit.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
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
    m_repoPath = new QLabel(root);
    m_repoPath->setObjectName(QStringLiteral("muted"));

    m_historyBtn = new QToolButton;
    m_historyBtn->setText(tr("Recent ▾"));
    m_historyBtn->setToolTip(tr("Reuse a recent commit message"));
    m_historyBtn->setPopupMode(QToolButton::InstantPopup);
    m_historyMenu = new QMenu(m_historyBtn);
    m_historyBtn->setMenu(m_historyMenu);

    m_message = new MessageEdit;
    m_amend = new QCheckBox(tr("Amend last commit"));
    m_counter = new QLabel;

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
    msgHead->addWidget(m_historyBtn);
    l->addLayout(msgHead);
    l->addWidget(m_message, 2);

    auto *msgFoot = new QHBoxLayout;
    msgFoot->addWidget(m_amend);
    msgFoot->addStretch();
    msgFoot->addWidget(m_counter);
    l->addLayout(msgFoot);
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

    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(left);
    split->addWidget(m_diff);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    split->setSizes({520, 880});

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(split);

    // --- signals
    connect(m_message, &QPlainTextEdit::textChanged, this, &CommitWindow::updateCounts);
    connect(m_amend, &QCheckBox::toggled, this, &CommitWindow::onAmendToggled);
    connect(m_files, &QTreeWidget::currentItemChanged, this, &CommitWindow::showCurrentDiff);
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
    connect(&Theme::instance(), &Theme::changed, this, &CommitWindow::applyTheme);

    // --- keyboard
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), this, [this] { commit(false); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Enter")), this, [this] { commit(false); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Return")), this, [this] { commit(true); });
    new QShortcut(QKeySequence(QStringLiteral("F5")), this, [this] { refresh(); });
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
        m_message->setPlainText(draft);

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

    const QString branch = m_repo.branch();
    m_header->setText(branch.isEmpty() ? tr("Commit on <i>detached HEAD</i>")
                                       : tr("Commit to <b>%1</b>").arg(branch.toHtmlEscaped()));
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
    if (!it) {
        m_diff->showMessage({}, tr("Select a file to see its changes"));
        return;
    }
    const FileEntry &e = m_entries.at(it->data(0, IndexRole).toInt());
    m_diff->showDiff(it->text(0), m_repo.diff(e));
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

    const bool can = !m_busy && !msg.trimmed().isEmpty() && (checked > 0 || m_amend->isChecked());
    m_commitBtn->setEnabled(can);
    m_pushBtn->setEnabled(can);
}

void CommitWindow::onAmendToggled(bool on)
{
    if (on) {
        m_lastMessage = m_repo.lastCommitMessage();
        if (m_message->toPlainText().trimmed().isEmpty())
            m_message->setPlainText(m_lastMessage);
    } else if (m_message->toPlainText().trimmed() == m_lastMessage) {
        m_message->clear();
    }
    updateCounts();
}

void CommitWindow::commit(bool push)
{
    if (m_busy || !m_commitBtn->isEnabled())
        return;

    const QString msg = m_message->toPlainText().trimmed();
    const bool amend = m_amend->isChecked();
    QStringList paths, untracked;
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        auto *it = m_files->topLevelItem(i);
        if (it->checkState(0) != Qt::Checked)
            continue;
        const FileEntry &e = m_entries.at(it->data(0, IndexRole).toInt());
        paths << e.path;
        if (!e.oldPath.isEmpty())
            paths << e.oldPath;
        if (e.untracked())
            untracked << e.path;
    }
    if (msg.isEmpty() || (paths.isEmpty() && !amend))
        return;

    const bool merging = m_repo.isMerging();
    setBusy(true, tr("Committing…"));

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

    saveToHistory(msg);

    // Async so long-running hooks (linters, tests) don't freeze the window.
    auto *proc = new QProcess(this);
    m_repo.configure(*proc);
    connect(proc, &QProcess::finished, this, [this, proc, push](int code, QProcess::ExitStatus st) {
        const QString output = QString::fromUtf8(proc->readAllStandardOutput() + proc->readAllStandardError());
        proc->deleteLater();
        if (st != QProcess::NormalExit || code != 0) {
            setBusy(false, tr("Commit failed"));
            showError(tr("Commit failed"), output);
            return;
        }
        const QString hash = QString::fromUtf8(
            m_repo.run({QStringLiteral("rev-parse"), QStringLiteral("--short"), QStringLiteral("HEAD")}).out).trimmed();
        QSettings().remove(draftKey());
        {
            QSignalBlocker block(m_amend);
            m_amend->setChecked(false);
        }
        m_message->clear();
        if (push) {
            setBusy(true, tr("Committed %1, pushing…").arg(hash));
            startPush();
        } else {
            setBusy(false, tr("Committed %1").arg(hash));
            finishAfterSuccess();
        }
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart)
            return;
        proc->deleteLater();
        setBusy(false, tr("Commit failed"));
        showError(tr("Commit failed"), tr("Could not start git."));
    });
    proc->start(QStringLiteral("git"), m_repo.commitArgs(paths, amend, merging));
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
    m_selectAll->setEnabled(!busy);
    m_historyBtn->setEnabled(!busy);
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
        connect(act, &QAction::triggered, this, [this, msg] { m_message->setPlainText(msg); });
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

void CommitWindow::closeEvent(QCloseEvent *e)
{
    if (m_busy) {   // don't abandon a running commit/push
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
