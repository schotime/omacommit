#include "OgWindow.h"
#include "CommitWindow.h"
#include "LogWindow.h"
#include "ResolveWindow.h"
#include "GitRepo.h"
#ifdef OG_PORTAL
#include "Portal.h"
#endif

#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>

static const int MaxRecentRepos = 15;

// Most recent first, each repo once.
static QStringList recentRepos()
{
    return QSettings().value(QStringLiteral("recentRepos")).toStringList();
}

static void rememberRepo(const QString &root)
{
    QStringList repos = recentRepos();
    repos.removeAll(root);
    repos.prepend(root);
    while (repos.size() > MaxRecentRepos)
        repos.removeLast();
    QSettings().setValue(QStringLiteral("recentRepos"), repos);
}

// ~/Projects/og rather than /home/you/Projects/og.
static QString shortPath(const QString &path)
{
    const QString home = QDir::homePath();
    if (path == home || path.startsWith(home + QLatin1Char('/')))
        return QLatin1Char('~') + path.mid(home.size());
    return QDir::toNativeSeparators(path);
}

OgWindow::OgWindow(const QString &root, QWidget *parent) : QWidget(parent), m_root(root)
{
    m_stack = new QStackedWidget;
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(m_stack);

    new QShortcut(QKeySequence(QStringLiteral("Ctrl+O")), this, [this] { chooseRepo(); });
    rememberRepo(m_root);
}

QWidget *OgWindow::page(Page p) const
{
    switch (p) {
    case Commit: return m_commit;
    case Log: return m_log;
    case Resolve: return m_resolve;
    }
    return nullptr;
}

void OgWindow::go(Page p, const QString &file)
{
    QWidget *w = page(p);
    if (!w) {
        switch (p) {
        case Commit: w = m_commit = new CommitWindow(m_root); break;
        case Log: w = m_log = new LogWindow(m_root); break;
        case Resolve: w = m_resolve = new ResolveWindow(m_root, file); break;
        }
        m_stack->addWidget(w);
        connect(w, &QWidget::windowTitleChanged, this, [this] { updateTitle(); });
    } else if (p == Resolve && !file.isEmpty()) {
        m_resolve->select(file);
    }
    if (m_stack->count() == 1)
        m_home = p;
    // The divider between the left pane and the diff stays where it was: the
    // page being shown takes the one being left's.
    const int left = leftWidth(m_stack->currentWidget());
    m_current = p;
    m_stack->setCurrentWidget(w);
    setLeftWidth(w, left);
    updateTitle();
}

int OgWindow::sidebarWidth(int total)
{
    // Measured on an editor styled like the message box (every text editor
    // gets the code font), so it is right on any page.
    QPlainTextEdit probe;
    probe.ensurePolished();
    const int want = probe.fontMetrics().averageCharWidth() * 50 + 2 * int(probe.document()->documentMargin())
                   + 2 * probe.frameWidth() + probe.verticalScrollBar()->sizeHint().width()
                   + 28;   // + the panel's margins
    return qBound(int(total * 0.25), want, int(total * 0.45));
}

static QSplitter *pageSplit(QWidget *page)
{
    return page ? page->findChild<QSplitter *>(QStringLiteral("pageSplit")) : nullptr;
}

int OgWindow::leftWidth(QWidget *page) const
{
    QSplitter *s = pageSplit(page);
    return s && s->isVisible() && !s->sizes().isEmpty() ? s->sizes().constFirst() : -1;
}

void OgWindow::setLeftWidth(QWidget *page, int left)
{
    QSplitter *s = pageSplit(page);
    if (!s || left <= 0 || s->count() < 2)
        return;
    // The stack's width: a page shown for the first time may not be laid out yet.
    const int total = m_stack->width() - s->handleWidth();
    if (total > left)
        s->setSizes({left, total - left});
}

void OgWindow::go(QWidget *from, Page p, const QString &file)
{
    if (auto *w = qobject_cast<OgWindow *>(from->window()))
        w->go(p, file);
}

void OgWindow::back(QWidget *from)
{
    auto *w = qobject_cast<OgWindow *>(from->window());
    if (!w) {
        from->window()->close();
        return;
    }
    if (w->m_current == w->m_home)
        w->close();
    else
        w->go(w->m_home);
}

void OgWindow::updateTitle()
{
    if (QWidget *w = m_stack->currentWidget())
        setWindowTitle(w->windowTitle());
}

// Every page gets its say: the commit page keeps its draft and asks about
// unsaved edits, the resolve page about its unsaved merge.
bool OgWindow::closePages()
{
    QWidget *current = m_stack->currentWidget();
    QVector<QWidget *> pages{current};
    for (int i = 0; i < m_stack->count(); ++i)
        if (m_stack->widget(i) != current)
            pages << m_stack->widget(i);
    for (QWidget *p : pages) {
        if (p && !p->close()) {
            m_stack->setCurrentWidget(p);
            p->show();
            updateTitle();
            return false;
        }
    }
    return true;
}

void OgWindow::closeEvent(QCloseEvent *e)
{
    if (closePages())
        e->accept();
    else
        e->ignore();
}

void OgWindow::chooseRepo()
{
    QMenu menu(this);
    for (const QString &root : recentRepos()) {
        if (QDir::cleanPath(root) == QDir::cleanPath(m_root) || !QFileInfo(root).isDir())
            continue;   // where we are, or gone since
        QAction *a = menu.addAction(QStringLiteral("%1\t%2").arg(QFileInfo(root).fileName(), shortPath(root)));
        connect(a, &QAction::triggered, this, [this, root] { openRepo(root); });
    }
    if (!menu.isEmpty())
        menu.addSeparator();
    connect(menu.addAction(tr("Open Repository…")), &QAction::triggered, this, [this] {
        const QString title = tr("Choose a Git repository");
        const QString from = QFileInfo(m_root).absolutePath();
        QString picked;
#ifdef OG_PORTAL
        if (Portal::pickDirectory(title, from, &picked) == Portal::Result::Unavailable)
#endif
            picked = QFileDialog::getExistingDirectory(this, title, from);
        if (picked.isEmpty())
            return;
        const QString root = GitRepo::findRoot(picked);
        if (root.isEmpty()) {
            QMessageBox::warning(this, tr("Open Repository"), tr("%1 is not inside a Git repository.").arg(picked));
            return;
        }
        openRepo(root);
    });
    if (QAction *first = menu.actions().value(0))
        menu.setActiveAction(first);
    // Near the top of the window, centred, like a command palette.
    const QSize size = menu.sizeHint();
    menu.exec(mapToGlobal(QPoint((width() - size.width()) / 2, height() / 8)));
}

void OgWindow::openRepo(const QString &root)
{
    if (QDir::cleanPath(root) == QDir::cleanPath(m_root) || !closePages())
        return;
    // Resolve was about the old repo's conflicts: the new one opens on its commit page.
    const Page current = m_current == Resolve ? Commit : m_current;
    const int left = leftWidth(m_stack->currentWidget());
    while (m_stack->count()) {
        QWidget *w = m_stack->widget(0);
        m_stack->removeWidget(w);
        w->deleteLater();
    }
    // deleteLater leaves these set until the event loop gets to it.
    m_commit = nullptr;
    m_log = nullptr;
    m_resolve = nullptr;
    m_root = root;
    rememberRepo(m_root);
    go(current);
    setLeftWidth(m_stack->currentWidget(), left);
}
