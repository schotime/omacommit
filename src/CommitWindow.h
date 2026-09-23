#pragma once

#include "DiffView.h"
#include "GitRepo.h"

#include <QTemporaryDir>
#include <QWidget>

#include <memory>

class MessageEdit;
class QCheckBox;
class QLabel;
class QLineEdit;
class QMenu;
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

private:
    void refresh();
    void showCurrentDiff();
    bool canEditInDiff(const FileEntry &e) const;
    QByteArray rediffEdited(const QStringList &lines);
    bool writeEdited(const QStringList &lines);
    void saveEdited();
    bool resolveUnsavedEdits(bool allowCancel = true);
    void showFileMenu(const QPoint &pos);
    bool canRevert(const FileEntry &e) const;
    void revertFile(const FileEntry &e);
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
    QLabel *m_header;
    QLabel *m_repoPath;
    QToolButton *m_historyBtn;
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
    QByteArray m_diffLoaded;
    QByteArray m_diffBase;
    bool m_diffCr = false;   // git's own diff saw CRLF on the file's side
    std::unique_ptr<QTemporaryDir> m_tmp;

    QString m_lastMessage;
    bool m_updatingChecks = false;
    bool m_busy = false;
};
