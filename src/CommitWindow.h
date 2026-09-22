#pragma once

#include "GitRepo.h"

#include <QWidget>

class DiffView;
class MessageEdit;
class QCheckBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

class CommitWindow : public QWidget {
    Q_OBJECT
public:
    explicit CommitWindow(const QString &root, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void refresh();
    void showCurrentDiff();
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
    QColor statusColor(const FileEntry &e) const;

    GitRepo m_repo;
    QVector<FileEntry> m_entries;

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

    QString m_lastMessage;
    bool m_updatingChecks = false;
    bool m_busy = false;
};
