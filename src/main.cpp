#include "CommitWindow.h"
#include "GitRepo.h"
#include "LogWindow.h"
#include "Portal.h"
#include "ResolveWindow.h"
#include "Style.h"
#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QHash>
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
                "  og [commit|c] [path]   Commit dialog for the repo containing <path> (default: cwd)\n"
                "  og log|l [path]        History: commit graph, changed files and their diffs\n"
                "  og resolve|r [path]    Resolve merge conflicts (path may name a conflicted file)\n"
                "\n"
                "Options:\n"
                "  -h, --help             Show this help\n"
                "  -V, --version          Show the version\n");
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
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/omarchy-commit.svg")));
    QApplication::setStyle(new Style);   // Fusion, with og's checkboxes

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
        // Each subcommand, and its one-letter alias.
        static const QHash<QString, QString> known{
            {QStringLiteral("commit"), QStringLiteral("commit")},   {QStringLiteral("c"), QStringLiteral("commit")},
            {QStringLiteral("log"), QStringLiteral("log")},         {QStringLiteral("l"), QStringLiteral("log")},
            {QStringLiteral("resolve"), QStringLiteral("resolve")}, {QStringLiteral("r"), QStringLiteral("resolve")},
        };
        if (known.contains(args.first()))
            command = known.value(args.takeFirst());
    }
    Theme::instance().load();
    Theme::instance().apply();

    const bool hasPath = !args.isEmpty();
    QString start = hasPath ? args.first() : QDir::currentPath();
    QString file;   // og resolve some/file: open that one first
    if (QFileInfo(start).isFile()) {
        file = QFileInfo(start).absoluteFilePath();
        start = QFileInfo(start).absolutePath();
    }

    QString root = GitRepo::findRoot(start);
    if (root.isEmpty() && !hasPath) {
        // Launched from a menu (cwd = $HOME): let the user pick a repo, in the
        // desktop's file chooser when there is one.
        const QString title = QObject::tr("Choose a Git repository");
        QString picked;
        if (Portal::pickDirectory(title, QDir::homePath(), &picked) == Portal::Result::Unavailable)
            picked = QFileDialog::getExistingDirectory(nullptr, title, QDir::homePath());
        if (picked.isEmpty())
            return 0;
        start = picked;
        root = GitRepo::findRoot(picked);
    }
    if (root.isEmpty()) {
        reportStartupError(QObject::tr("%1 is not inside a Git repository.").arg(start));
        return 1;
    }

    if (command == QStringLiteral("resolve")) {
        ResolveWindow window(root, file);
        window.resize(1500, 900);
        window.show();
        return app.exec();
    }
    if (command == QStringLiteral("log")) {
        LogWindow window(root);
        window.resize(1400, 860);
        window.show();
        return app.exec();
    }
    CommitWindow window(root);
    window.resize(1400, 860);
    window.show();
    return app.exec();
}
