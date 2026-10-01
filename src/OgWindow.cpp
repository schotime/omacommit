#include "OgWindow.h"
#include "CommitWindow.h"
#include "LogWindow.h"
#include "ResolveWindow.h"
#include "GitRepo.h"
#include "Picker.h"
#ifdef OG_PORTAL
#include "Portal.h"
#endif

#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QUrl>
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

void OgWindow::openInEditor(const QString &path)
{
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("omarchy-launch-editor"));
    if (!tool.isEmpty() && QProcess::startDetached(tool, {path}))
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void OgWindow::showInFileManager(const QString &path)
{
#ifdef Q_OS_WIN
    if (QProcess::startDetached(QStringLiteral("explorer"), {QStringLiteral("/select,") + QDir::toNativeSeparators(path)}))
        return;
#elif defined(OG_PORTAL)
    if (Portal::showItem(path))
        return;
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
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
    QStringList roots;
    QVector<PickerItem> items;
    for (const QString &root : recentRepos()) {
        if (QDir::cleanPath(root) == QDir::cleanPath(m_root) || !QFileInfo(root).isDir())
            continue;   // where we are, or gone since
        roots << root;
        items.push_back({QFileInfo(root).fileName(), shortPath(root)});
    }
    PickerItem open{tr("Open Repository…")};
    open.always = true;
    items.push_back(open);
    const int picked = Picker::choose(this, tr("Search recent repositories"), items);
    if (picked < 0)
        return;
    if (picked < roots.size()) {
        openRepo(roots.at(picked));
        return;
    }
    const QString title = tr("Choose a Git repository");
    const QString from = QFileInfo(m_root).absolutePath();
    QString dir;
#ifdef OG_PORTAL
    if (Portal::pickDirectory(title, from, &dir) == Portal::Result::Unavailable)
#endif
        dir = QFileDialog::getExistingDirectory(this, title, from);
    if (dir.isEmpty())
        return;
    const QString root = GitRepo::findRoot(dir);
    if (root.isEmpty()) {
        QMessageBox::warning(this, tr("Open Repository"), tr("%1 is not inside a Git repository.").arg(dir));
        return;
    }
    openRepo(root);
}

bool OgWindow::chooseBranch(QWidget *from, const GitRepo &repo, QString *switchedTo)
{
    const QVector<BranchRef> branches = repo.branches();
    QVector<PickerItem> items;
    for (const BranchRef &b : branches)
        items.push_back({b.name, b.current ? tr("current · %1").arg(b.when) : b.when});
    const int picked = Picker::choose(from, tr("Switch to branch"), items);
    if (picked < 0 || branches.at(picked).current)
        return false;
    const QString name = branches.at(picked).localName;
    if (!switchBranch(from, repo, name))
        return false;
    if (switchedTo)
        *switchedTo = name;
    return true;
}

// Uncommitted changes come along when they can; when git won't switch -- they
// would be overwritten, or a merge is in progress -- nothing changes and its
// reason is shown.
bool OgWindow::switchBranch(QWidget *from, const GitRepo &repo, const QString &name)
{
    const GitResult r = repo.run({QStringLiteral("switch"), name});
    if (!r.ok())
        QMessageBox::warning(from, tr("Could not switch to %1").arg(name),
                             QString::fromUtf8(r.err + r.out).trimmed().right(1500));
    return r.ok();
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
