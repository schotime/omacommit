#pragma once

#include "GitRepo.h"
#include "LogGraph.h"

#include <QHash>
#include <QWidget>

class DiffView;
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QTreeWidgetItem;
class PageTabs;
class QTreeWidget;

// History browser: the commit graph with branch and tag names on the left, the
// selected commit's details and changed files under it, and the selected
// file's diff against the commit's parent on the right.
class LogWindow : public QWidget {
    Q_OBJECT
public:
    explicit LogWindow(const QString &root, QWidget *parent = nullptr);

    // Read by the graph delegate.
    const QVector<LogCommit> &commits() const { return m_log; }
    const QVector<GraphRow> &graph() const { return m_graph; }
    const QHash<QString, QVector<RefLabel>> &refs() const { return m_refs; }

protected:
    void showEvent(QShowEvent *e) override;

private:
    void reload();
    void loadMore();
    void showCommit();
    void showWorkingChanges();
    void showComparison(const QList<QTreeWidgetItem *> &selected);
    void listCommitFiles();
    void showFileDiff();
    void showCommitMenu(const QPoint &pos);
    void revertCommit(const LogCommit &c);
    void showFileMenu(const QPoint &pos);
    void revertWorkingFile(const FileEntry &f);
    void revertFileChange(const LogCommit &c, const FileEntry &f);
    void applyTheme();
    void sizeColumns();
    void fitColumns();
    bool eventFilter(QObject *watched, QEvent *event) override;
    QString baseOf(const LogCommit &c) const;
    QColor statusColor(QChar status) const;

    GitRepo m_repo;
    QLabel *m_header;
    QLabel *m_repoPath;
    QCheckBox *m_allBranches;
    PageTabs *m_tabs;
    bool m_shownBefore = false;
    bool m_working = false;   // the first row is the working changes, not a commit
    QTreeWidget *m_commits;
    QPlainTextEdit *m_details;
    QLabel *m_fileCount;
    QTreeWidget *m_files;
    DiffView *m_diff;

    QVector<LogCommit> m_log;
    QVector<GraphRow> m_graph;
    QHash<QString, QVector<RefLabel>> m_refs;
    GraphLayout m_layout;
    QVector<FileEntry> m_commitFiles;
    QString m_shownCommit;   // what the files and diff show: a hash, or a comparison's span
    // The two ends the files and diffs are between: from `from` to `to`, or
    // to the working tree.
    struct Span {
        QString from, to;
        bool working = false;
    };
    Span m_span;
    int m_colWidth[4] = {0, 0, 0, 0};   // what Author, Date and Commit need
    int m_subjectMin = 0;      // the Graph/subject column's default floor
    int m_subjectDragged = 0;  // a width set by dragging the column, kept from then on
    bool m_fitting = false;
    bool m_exhausted = false;
    bool m_loading = false;
};
