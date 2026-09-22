#include "CommitWindow.h"
#include "GitRepo.h"
#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("git-commit-ui"));
    QApplication::setOrganizationName(QStringLiteral("omarchy"));
    QGuiApplication::setDesktopFileName(QStringLiteral("omarchy-commit"));   // Wayland app_id / Hyprland class
    QApplication::setStyle(QStringLiteral("Fusion"));

    Theme::instance().load();
    Theme::instance().apply();

    const QStringList args = app.arguments();
    QString start = args.size() > 1 ? args.at(1) : QDir::currentPath();
    if (QFileInfo(start).isFile())
        start = QFileInfo(start).absolutePath();

    QString root = GitRepo::findRoot(start);
    if (root.isEmpty() && args.size() <= 1) {
        // Launched from a menu (cwd = $HOME): let the user pick a repo.
        const QString picked = QFileDialog::getExistingDirectory(nullptr, QObject::tr("Choose a Git repository"),
                                                                 QDir::homePath());
        if (picked.isEmpty())
            return 0;
        start = picked;
        root = GitRepo::findRoot(picked);
    }
    if (root.isEmpty()) {
        QMessageBox::critical(nullptr, QStringLiteral("git-commit-ui"),
                              QObject::tr("%1 is not inside a Git repository.").arg(start));
        return 1;
    }

    CommitWindow window(root);
    window.resize(1400, 860);
    window.show();
    return app.exec();
}
