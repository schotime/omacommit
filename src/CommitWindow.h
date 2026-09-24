#pragma once

#include "Agent.h"
#include "DiffView.h"
#include "GitRepo.h"

#include <QSet>
#include <QTemporaryDir>
#include <QWidget>

#include <memory>

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
    void refresh();
    QVector<QTreeWidgetItem *> fileItems() const;   // the file rows of both sections, in order
    const FileEntry *entryOf(const QTreeWidgetItem *it) const;   // null for a section header
    void showCurrentDiff();
    bool canEditInDiff(const FileEntry &e) const;
    QByteArray rediffEdited(const QStringList &lines);
    bool writeEdited(const QStringList &lines);
    void saveEdited();
    bool resolveUnsavedEdits(bool allowCancel = true);
    void flagWhitespace(const QByteArray &text);
    void updateWriteButton();
    void setMessage(const QString &text);
    void writeMessage();
    QString messagePrompt() const;
    void showFileMenu(const QPoint &pos);
    bool canUnstage(const FileEntry &e) const;
    void stageFile(const FileEntry &e);
    void unstageFile(const FileEntry &e);
    bool indexMovedOn(const QByteArray &index);
    void stageLines(const DiffView::Selection &picked);
    void unstageLines(const DiffView::Selection &picked);
    bool canRevert(const FileEntry &e) const;
    void revertFile(const FileEntry &e);
    void discardUnstaged(const FileEntry &e);
    void afterIndexChange(const GitResult &r, const QString &failure, const QString &done);
    void onAmendToggled(bool on);
    bool prepareBranch();
    void updateCounts();
    void updateSelectAllState();
    void toggleAll();
    void applyFilter();
    void applyTheme();
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

    QSplitter *m_split = nullptr;
    bool m_sized = false;
    PageTabs *m_tabs;
    QLabel *m_header;
    QLabel *m_repoPath;
    QToolButton *m_historyBtn;
    QToolButton *m_writeBtn;
    Agent m_agent;
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
    QByteArray m_diffLoaded;
    QByteArray m_diffBase;
    bool m_diffCr = false;   // git's own diff saw CRLF on the file's side
    // Whitespace checking for the shown file: its rules and the base version
    // its added lines are measured against.
    WhitespaceRules m_wsRules;
    QByteArray m_wsBase;
    std::unique_ptr<QTemporaryDir> m_tmp;

    // While amending, the staged list also holds the last commit's changes;
    // these are the paths the index itself changes on top of HEAD.
    QSet<QString> m_indexChanged;

    QString m_lastMessage;
    bool m_updatingChecks = false;
    bool m_busy = false;
};
