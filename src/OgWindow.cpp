#include "OgWindow.h"
#include "CommitWindow.h"
#include "LogWindow.h"
#include "ResolveWindow.h"

#include <QCloseEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>

OgWindow::OgWindow(const QString &root, QWidget *parent) : QWidget(parent), m_root(root)
{
    m_stack = new QStackedWidget;
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(m_stack);
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
void OgWindow::closeEvent(QCloseEvent *e)
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
            e->ignore();
            return;
        }
    }
    e->accept();
}
