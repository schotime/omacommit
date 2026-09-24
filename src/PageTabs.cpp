#include "PageTabs.h"

#include <QHBoxLayout>
#include <QPushButton>

PageTabs::PageTabs(OgWindow::Page current, QWidget *page) : m_current(current)
{
    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(18);
    const struct { OgWindow::Page page; QString name; QString tip; } tabs[3] = {
        {OgWindow::Commit, tr("Commit"), tr("The commit dialog (Ctrl+L or Ctrl+Tab from the log)")},
        {OgWindow::Log, tr("Log"), tr("The history (Ctrl+L or Ctrl+Tab)")},
        {OgWindow::Resolve, tr("Resolve"), tr("Resolve the conflicts")},
    };
    for (const auto &t : tabs) {
        auto *b = new QPushButton(t.name);
        b->setObjectName(QStringLiteral("tab"));
        b->setProperty("current", t.page == current);
        b->setFocusPolicy(Qt::NoFocus);
        if (t.page != current) {
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(t.tip);
            const OgWindow::Page to = t.page;
            connect(b, &QPushButton::clicked, page, [page, to] { OgWindow::go(page, to); });
        }
        m_tabs[t.page] = b;
        l->addWidget(b);
    }
    l->addStretch();
    setConflicts(current == OgWindow::Resolve);
}

void PageTabs::setConflicts(bool any)
{
    m_tabs[OgWindow::Resolve]->setVisible(any || m_current == OgWindow::Resolve);
}
