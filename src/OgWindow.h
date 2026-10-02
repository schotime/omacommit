#pragma once

#include <QPointer>
#include <QWidget>
#include <functional>

class CommitWindow;
class GitRepo;
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
    // The left pane's width in a window `total` wide: the message box up to
    // its dashed ruler, no more however wide the window, so the rest goes to
    // the diff -- or a share of a window too narrow for a side-by-side diff.
    static int sidebarWidth(int total);
    // Opens a file for editing: Omarchy's chosen editor when there is one (its
    // default for text is a terminal app, which a plain desktop open can't
    // start), otherwise whatever the desktop opens the file with.
    static void openInEditor(const QString &path);
    // Shows the file selected in the file manager, or at least its folder.
    static void showInFileManager(const QString &path);

    // Ctrl+O: the repos og has recently opened, to search, and a folder chooser.
    void chooseRepo();
    static void chooseRepo(QWidget *from);   // from a page: its window's, as Ctrl+O there
    // The most recently opened repository that is still one, or empty.
    static QString lastRepo();
    // Ctrl+B, or the branch name: load branches and switch asynchronously.
    // Completion is called only after a successful switch; errors are shown.
    static void chooseBranch(QWidget *from, const GitRepo &repo, std::function<void(const QString &)> done);
    static void switchBranch(QWidget *from, const GitRepo &repo, const QString &name, std::function<void()> done);
    // Shows `root` in this window instead, on the same page, once every page
    // has agreed to close (drafts kept, unsaved edits asked about).
    void openRepo(const QString &root);

protected:
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

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
    bool m_fitPending = false;   // a divider fit is queued for after the layout settles
};
