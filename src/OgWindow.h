#pragma once

#include <QPointer>
#include <QWidget>

class CommitWindow;
class LogWindow;
class QStackedWidget;
class ResolveWindow;

// og's one window. Commit, log and resolve are pages of it: going from one to
// another swaps what the window shows, keeping each page as you left it
// (message, ticks, selection, unsaved edits), and nothing moves on screen.
class OgWindow : public QWidget {
    Q_OBJECT
public:
    enum Page { Commit, Log, Resolve };

    explicit OgWindow(const QString &root, QWidget *parent = nullptr);

    // Shows `page` (created the first time); `file` picks the file to resolve.
    void go(Page page, const QString &file = QString());
    // From a page: go to another page of its window.
    static void go(QWidget *from, Page page, const QString &file = QString());
    // From a page: back to the page og was started on, or, on that page,
    // close the window. Esc, and a commit that leaves nothing to do.
    static void back(QWidget *from);
    // The left pane's width when a page first opens in a window `total` wide:
    // room for a 50-column summary line in the message box, no more however
    // wide the window, so the rest goes to the diff.
    static int sidebarWidth(int total);

    // Ctrl+O: a menu of the repos og has recently opened, and a folder chooser.
    void chooseRepo();
    // Shows `root` in this window instead, on the same page, once every page
    // has agreed to close (drafts kept, unsaved edits asked about).
    void openRepo(const QString &root);

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    QWidget *page(Page p) const;
    bool closePages();
    void updateTitle();
    int leftWidth(QWidget *page) const;
    void setLeftWidth(QWidget *page, int left);

    QString m_root;
    QStackedWidget *m_stack;
    QPointer<CommitWindow> m_commit;
    QPointer<LogWindow> m_log;
    QPointer<ResolveWindow> m_resolve;
    Page m_home = Commit;      // the page og was started on
    Page m_current = Commit;
};
