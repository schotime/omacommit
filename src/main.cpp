#include "CommitWindow.h"
#include "GitRepo.h"
#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

#include <cstdio>

#include <unistd.h>

#ifndef OG_VERSION
#define OG_VERSION "0.0.0"
#endif

namespace {

void printUsage()
{
    std::printf("og — Omarchy Git\n"
                "\n"
                "Usage:\n"
                "  og [commit] [path]   Commit dialog for the repo containing <path> (default: cwd)\n"
                "  og log [path]        Not implemented yet\n"
                "  og resolve [path]    Not implemented yet\n"
                "\n"
                "Options:\n"
                "  -h, --help           Show this help\n"
                "  -V, --version        Show the version\n");
}

// Startup failures reach the user differently depending on how og was launched:
// from a terminal a line on stderr is what you want, from a keybind or a .desktop
// entry there is no terminal to read, so it has to be a dialog.
void reportStartupError(const QString &message)
{
    if (::isatty(STDERR_FILENO)) {
        std::fprintf(stderr, "og: %s\n", qPrintable(message));
        return;
    }
    QMessageBox::critical(nullptr, QStringLiteral("og"), message);
}

} // namespace

int main(int argc, char *argv[])
{
    // These are static setters, and they must run before QApplication is
    // constructed: Qt registers the process with the xdg-desktop-portal during
    // construction, and a name set afterwards arrives too late to be used --
    // the re-registration is refused with "Connection already associated with
    // an application ID".
    QApplication::setApplicationName(QStringLiteral("og"));
    QApplication::setOrganizationName(QStringLiteral("omarchy"));
    QGuiApplication::setDesktopFileName(QStringLiteral("omarchy-commit"));   // Wayland app_id / Hyprland class

    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));

    QStringList args = app.arguments();
    args.removeFirst();

    if (args.contains(QStringLiteral("-h")) || args.contains(QStringLiteral("--help"))) {
        printUsage();
        return 0;
    }
    if (args.contains(QStringLiteral("-V")) || args.contains(QStringLiteral("--version"))) {
        std::printf("og %s\n", OG_VERSION);
        return 0;
    }

    // A leading subcommand is optional; anything else is taken as a path.
    QString command = QStringLiteral("commit");
    if (!args.isEmpty()) {
        static const QStringList known{QStringLiteral("commit"), QStringLiteral("log"),
                                       QStringLiteral("resolve")};
        if (known.contains(args.first())) {
            command = args.takeFirst();
        }
    }
    if (command != QStringLiteral("commit")) {
        std::fprintf(stderr, "og: '%s' is not implemented yet.\n", qPrintable(command));
        return 2;
    }

    Theme::instance().load();
    Theme::instance().apply();

    const bool hasPath = !args.isEmpty();
    QString start = hasPath ? args.first() : QDir::currentPath();
    if (QFileInfo(start).isFile())
        start = QFileInfo(start).absolutePath();

    QString root = GitRepo::findRoot(start);
    if (root.isEmpty() && !hasPath) {
        // Launched from a menu (cwd = $HOME): let the user pick a repo.
        const QString picked = QFileDialog::getExistingDirectory(nullptr, QObject::tr("Choose a Git repository"),
                                                                 QDir::homePath());
        if (picked.isEmpty())
            return 0;
        start = picked;
        root = GitRepo::findRoot(picked);
    }
    if (root.isEmpty()) {
        reportStartupError(QObject::tr("%1 is not inside a Git repository.").arg(start));
        return 1;
    }

    CommitWindow window(root);
    window.resize(1400, 860);
    window.show();
    return app.exec();
}
