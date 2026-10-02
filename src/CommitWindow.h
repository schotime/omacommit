#pragma once

#include "Agent.h"
#include "DiffView.h"
#include "GitRepo.h"

#include <QSet>
#include <QTemporaryDir>
#include <QWidget>

#include <memory>
#include <functional>

class ElidedLabel;
class MessageEdit;
class PageTabs;
class QCheckBox;
class QLabel;
class QLineEdit;
class QMenu;
class QProcess;
class QPushButton;
class QSplitter;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

class CommitWindow : public QWidget {
    Q_OBJECT
public:
    explicit CommitWindow(const QString &root, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *e) override;
    void showEvent(QShowEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refresh(std::function<void()> after = {});
    void applyRefresh(const QVector<FileEntry> &entries);
    QVector<QTreeWidgetItem *> fileItems() const;   // the file rows of both sections, in order
    const FileEntry *entryOf(const QTreeWidgetItem *it) const;   // null for a section header
    void showCurrentDiff();
    bool canEditInDiff(const FileEntry &e) const;
    void rediffEdited(const QStringList &lines, std::function<void(const QByteArray &)> done);
    bool writeEdited(const QStringList &lines);
    void saveEdited();
    bool resolveUnsavedEdits(bool allowCancel = true);
    void updateWriteButton();
    void setMessage(const QString &text);
    void writeMessage();
    void startWriter(const QString &prompt);
    void showFileMenu(const QPoint &pos);
    bool canUnstage(const FileEntry &e) const;
    void stageFile(const FileEntry &e);
    void unstageFile(const FileEntry &e);
    void stageLines(const DiffView::Selection &picked);
    void unstageLines(const DiffView::Selection &picked);
    bool canRevert(const FileEntry &e) const;
    void revertFile(const FileEntry &e);
    void discardUnstaged(const FileEntry &e);
    bool canStash(const FileEntry &e) const;
    void stashFiles(const QVector<FileEntry> &files);
    QString commitIgnoreFile() const;
    bool isCommitIgnored(const FileEntry &e) const;
    void setCommitIgnored(const QStringList &paths, bool on);
    void afterIndexChange(const GitResult &r, const QString &failure, const QString &done);
    void runIndexTask(std::function<GitResult(const GitRepo &)> work, const QString &failure,
                      const QString &done, bool discardEdits = false);
    void onAmendToggled(bool on);
    void updateBranchField();
    void updateCounts();
    void updateSelectAllState();
    void toggleAll();
    void applyFilter();
    void applyTheme();
    void updateHeader();
    void chooseBranch();
    void commit(bool push);
    void startPush();
    void finishAfterSuccess();
    void setBusy(bool busy, const QString &message = QString());
    void showError(const QString &title, const QString &details);
    void rebuildHistoryMenu();
    void saveToHistory(const QString &message);
    QString draftKey() const;
    QString diffBase() const;
    QColor statusColor(const FileEntry &e) const;

    GitRepo m_repo;
    QVector<FileEntry> m_entries;
    QString m_commitBase;          // resolved by the latest background refresh
    QString m_gitDirectory;
    int m_refreshRequest = 0;
    bool m_refreshing = false;
    QString m_refreshStatus;

    QSplitter *m_split = nullptr;
    bool m_sized = false;
    PageTabs *m_tabs;
    QLabel *m_header;
    ElidedLabel *m_repoPath;
    QToolButton *m_historyBtn;
    QToolButton *m_writeBtn;
    QToolButton *m_agentMenuBtn = nullptr;
    Agent m_agent;
    QVector<Agent> m_agents;
    QProcess *m_writer = nullptr;   // the agent writing a message, while it runs
    bool m_writeStopped = false;
    QMenu *m_historyMenu;
    MessageEdit *m_message;
    QCheckBox *m_amend;
    QLineEdit *m_newBranch;
    QLabel *m_counter;
    QCheckBox *m_selectAll;
    QLabel *m_fileCount;
    QLineEdit *m_filter;
    QTreeWidget *m_files;
    QLabel *m_status;
    QPushButton *m_pushBtn;
    QPushButton *m_commitBtn;
    DiffView *m_diff;

    // The file open in the diff view: its path, its bytes as loaded (to spot
    // outside changes before saving) and the left side's copy -- HEAD's, or
    // the parent's when amending -- to re-diff edits against.
    QString m_diffPath;
    bool m_diffStaged = false;      // it is the file's staged side that is shown
    QByteArray m_shownIndex;        // the index's copy of it when it was shown
    bool m_shownInIndex = false;
    int m_editRequest = 0;
    QByteArray m_diffLoaded;
    QByteArray m_diffBase;
    bool m_diffCr = false;   // git's own diff saw CRLF on the file's side
    int m_diffRequest = 0;   // discard background results for an older selection
    // Whitespace checking for the shown file: its rules and the base version
    // its added lines are measured against.
    WhitespaceRules m_wsRules;
    QByteArray m_wsBase;
    std::unique_ptr<QTemporaryDir> m_tmp;

    // While amending, the staged list also holds the last commit's changes;
    // these are the paths the index itself changes on top of HEAD.
    QSet<QString> m_indexChanged;

    // Ignored files: the lines of .git/omacommit-ignore. Those files are
    // listed in a section of their own and start unticked.
    QStringList m_ignorePatterns;
    QSet<QString> m_resetChecks;   // paths whose rows take their default tick on the next refresh

    QString m_branch;   // empty when detached
    bool m_merging = false;
    QString m_lastMessage;
    bool m_updatingChecks = false;
    bool m_busy = false;
};
