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

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    QWidget *page(Page p) const;
    void updateTitle();

    QString m_root;
    QStackedWidget *m_stack;
    QPointer<CommitWindow> m_commit;
    QPointer<LogWindow> m_log;
    QPointer<ResolveWindow> m_resolve;
    Page m_home = Commit;      // the page og was started on
    Page m_current = Commit;
};
