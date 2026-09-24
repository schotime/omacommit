#pragma once

#include "OgWindow.h"

#include <QWidget>

class QPushButton;

// The top of each page: its name as the heading, the other pages beside it as
// links to switch to (Ctrl+L / Ctrl+Tab do the same between commit and log).
// Resolve is there only while there are conflicts.
class PageTabs : public QWidget {
public:
    PageTabs(OgWindow::Page current, QWidget *page);
    void setConflicts(bool any);

private:
    OgWindow::Page m_current;
    QPushButton *m_tabs[3];
};
