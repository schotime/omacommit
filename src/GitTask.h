#pragma once

#include "GitRepo.h"

#include <QApplication>
#include <QPointer>
#include <QThreadPool>
#include <QVariant>
#include <QWidget>

// Workers only touch captured values. Completion runs on the UI thread and
// is dropped if its owner was destroyed. Exclusive jobs hold the whole window
// (including page navigation) until completion, preventing overlapping writes.
namespace GitTask {
template<class Work, class Done>
void run(QWidget *owner, Work work, Done done, bool exclusive = true)
{
    QPointer<QWidget> self(owner), window(owner->window());
    if (exclusive) {
        if (window->property("gitTaskBusy").toBool())
            return;
        window->setProperty("gitTaskBusy", true);
        window->setEnabled(false);
    }
    QThreadPool::globalInstance()->start([self, window, work, done, exclusive]() mutable {
        const auto result = work();
        QMetaObject::invokeMethod(qApp, [self, window, result, done, exclusive] {
            if (exclusive && window) {
                window->setProperty("gitTaskBusy", false);
                window->setEnabled(true);
            }
            if (self)
                done(result);
        }, Qt::QueuedConnection);
    });
}
} // namespace GitTask
