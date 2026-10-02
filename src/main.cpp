#include "GitRepo.h"
#include "OgWindow.h"
#ifdef OG_PORTAL
#include "Portal.h"
#endif
#include "Style.h"
#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QHash>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#ifndef OG_VERSION
#define OG_VERSION "0.0.0"
#endif

namespace {

bool noConsole = false;

void printToTerminal(const char *text, bool error = false)
{
#ifdef Q_OS_WIN
    // A GUI-subsystem executable may have unusable C runtime streams in Git
    // Bash, even though the inherited Windows pipe is still usable.
    const HANDLE handle = ::GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    const DWORD length = static_cast<DWORD>(std::strlen(text));
    DWORD written = 0;
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr
        && ::WriteFile(handle, text, length, &written, nullptr))
        return;
#endif
    std::fputs(text, error ? stderr : stdout);
    std::fflush(error ? stderr : stdout);
}

void printUsage()
{
    printToTerminal("oc — Omacommit, Git for Omarchy\n"
                    "\n"
                    "Usage:\n"
                    "  oc [commit|c] [path]   Commit dialog for the repo containing <path> (default: cwd)\n"
                    "  oc log|l [path]        History: commit graph, changed files and their diffs\n"
                    "  oc resolve|r [path]    Resolve merge conflicts (path may name a conflicted file)\n"
                    "\n"
                    "Options:\n"
                    "  --no-console           Run without a console (Windows shortcuts); errors use dialogs\n"
                    "  -w, --wait             Stay in the foreground until the window closes\n"
                    "                         (from a terminal, oc otherwise gives the prompt back)\n"
                    "  -h, --help             Show this help\n"
                    "  -V, --version          Show the version\n");
}

bool hasTerminal()
{
    if (noConsole)
        return false;
#ifdef Q_OS_WIN
    if (::_isatty(::_fileno(stderr)) || ::_isatty(::_fileno(stdout)))
        return true;
    DWORD mode = 0;
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE && out != nullptr && ::GetConsoleMode(out, &mode))
        return true;
    // Git Bash connects Windows programs through pipes, so neither isatty()
    // nor GetConsoleMode() identifies its terminal. MSYSTEM is set by its
    // shells but not by Explorer or a desktop launcher.
    return qEnvironmentVariableIsSet("MSYSTEM") && out != INVALID_HANDLE_VALUE && out != nullptr;
#else
    return ::isatty(STDERR_FILENO) || ::isatty(STDOUT_FILENO);
#endif
}

// Startup failures reach the user differently depending on how og was launched:
// from a terminal a line on stderr is what you want, from a keybind or a .desktop
// entry there is no terminal to read, so it has to be a dialog.
void reportStartupError(const QString &message)
{
    if (hasTerminal()) {
        const QByteArray text = "oc: " + message.toLocal8Bit() + '\n';
        printToTerminal(text.constData(), true);
        return;
    }
    QMessageBox::critical(nullptr, QStringLiteral("Omacommit"), message);
}

#if defined(Q_OS_UNIX) || defined(Q_OS_WIN)
// Started from a terminal, oc gives the prompt back, as gvim does: a copy of
// itself, in a session of its own so closing the terminal doesn't close it,
// shows the window while this one exits. The repository is checked first,
// so a mistake is still reported here. Returns true when that copy is
// running and this process should just exit; --wait stays in the foreground.
bool detachFromTerminal(int argc, char *argv[])
{
    if (qEnvironmentVariableIsSet("OC_DETACHED")) {
        qunsetenv("OC_DETACHED");   // the copy: carry on, and don't pass it down
        return false;
    }
    if (!hasTerminal())
        return false;   // from a launcher: nothing to give back
    QStringList args;
    for (int i = 1; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);
    for (const char *stay : {"-w", "--wait", "-h", "--help", "-V", "--version"})
        if (args.contains(QLatin1String(stay)))
            return false;

    QCoreApplication probe(argc, argv);   // for QProcess; gone before the real one
    static const QStringList commands{QStringLiteral("commit"), QStringLiteral("c"), QStringLiteral("log"),
                                      QStringLiteral("l"), QStringLiteral("resolve"), QStringLiteral("r")};
    QStringList rest = args;
    if (!rest.isEmpty() && commands.contains(rest.first()))
        rest.removeFirst();
    if (!rest.isEmpty()) {   // a path given: it has to be in a repository (without one, the copy asks)
        QString start = rest.first();
        if (QFileInfo(start).isFile())
            start = QFileInfo(start).absolutePath();
        if (GitRepo::findRoot(start).isEmpty()) {
            const QByteArray text = "oc: " + QObject::tr("%1 is not inside a Git repository.")
                                                  .arg(rest.first()).toLocal8Bit() + '\n';
            printToTerminal(text.constData(), true);
            std::exit(1);
        }
    }

    QProcess copy;
    copy.setProgram(QCoreApplication::applicationFilePath());
    copy.setArguments(args);
    copy.setWorkingDirectory(QDir::currentPath());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("OC_DETACHED"), QStringLiteral("1"));
    copy.setProcessEnvironment(env);
    copy.setStandardInputFile(QProcess::nullDevice());
    copy.setStandardOutputFile(QProcess::nullDevice());
    copy.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    copy.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    });
#else
    copy.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
#endif
    return copy.startDetached();   // if it can't start, stay and run here
}
#endif

} // namespace

int main(int argc, char *argv[])
{
    // Windows builds are GUI executables. Attach only for CLI-only output;
    // preserve inherited redirected/Git Bash pipe handles when attaching.
    bool cliOnly = false;
    for (int i = 1; i < argc; ++i) {
        noConsole = noConsole || std::strcmp(argv[i], "--no-console") == 0;
        cliOnly = cliOnly || std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0
                  || std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-V") == 0;
    }
    noConsole = noConsole && !cliOnly;
#ifdef Q_OS_WIN
    if (cliOnly) {
        const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
        const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
        if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
            if (out != nullptr && out != INVALID_HANDLE_VALUE)
                ::SetStdHandle(STD_OUTPUT_HANDLE, out);
            if (err != nullptr && err != INVALID_HANDLE_VALUE)
                ::SetStdHandle(STD_ERROR_HANDLE, err);
        }
    }
#endif
#if defined(Q_OS_UNIX) || defined(Q_OS_WIN)
    if (detachFromTerminal(argc, argv))
        return 0;
#endif

    // Keep CLI-only options out of QApplication: constructing the GUI
    // application can alter the standard handles of a Windows GUI executable
    // under Git Bash before it gets the chance to print them.
    QStringList earlyArgs;
    for (int i = 1; i < argc; ++i)
        earlyArgs << QString::fromLocal8Bit(argv[i]);
    if (earlyArgs.contains(QStringLiteral("-h")) || earlyArgs.contains(QStringLiteral("--help"))) {
        printUsage();
        return 0;
    }
    if (earlyArgs.contains(QStringLiteral("-V")) || earlyArgs.contains(QStringLiteral("--version"))) {
        const QByteArray text = QStringLiteral("oc %1\n").arg(OG_VERSION).toLocal8Bit();
        printToTerminal(text.constData());
        return 0;
    }

    // These are static setters, and they must run before QApplication is
    // constructed: Qt registers the process with the xdg-desktop-portal during
    // construction, and a name set afterwards arrives too late to be used --
    // the re-registration is refused with "Connection already associated with
    // an application ID".
    QApplication::setApplicationName(QStringLiteral("oc"));
    QApplication::setOrganizationName(QStringLiteral("omarchy"));
    QGuiApplication::setDesktopFileName(QStringLiteral("omarchy-commit"));   // Wayland app_id / Hyprland class

    QApplication app(argc, argv);
    // The settings -- drafts, recent messages and repositories, diff options --
    // were kept under og, the old name. Bring them over the first time.
    {
        QSettings now;
        if (now.allKeys().isEmpty()) {
            QSettings old(QStringLiteral("omarchy"), QStringLiteral("og"));
            for (const QString &key : old.allKeys())
                now.setValue(key, old.value(key));
        }
    }
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/omarchy-commit.svg")));
    QApplication::setStyle(new Style);   // Fusion, with oc's checkboxes

    QStringList args = app.arguments();
    args.removeFirst();
    args.removeAll(QStringLiteral("-w"));        // --wait: stay in the foreground, handled above
    args.removeAll(QStringLiteral("--wait"));
    args.removeAll(QStringLiteral("--no-console"));

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
    QString file;   // oc resolve some/file: open that one first
    if (QFileInfo(start).isFile()) {
        file = QFileInfo(start).absoluteFilePath();
        start = QFileInfo(start).absolutePath();
    }

    QString root = GitRepo::findRoot(start);
    // Not in a repository and none named (launched from a menu, cwd = $HOME):
    // open the last one used. Ctrl+O, or clicking the path, picks another.
    if (root.isEmpty() && !hasPath)
        root = OgWindow::lastRepo();
    if (root.isEmpty() && !hasPath) {
        // Nothing opened before: let the user pick a repo, in the desktop's
        // file chooser when there is one.
        const QString title = QObject::tr("Choose a Git repository");
        QString picked;
#ifdef OG_PORTAL
        if (Portal::pickDirectory(title, QDir::homePath(), &picked) == Portal::Result::Unavailable)
#endif
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

    // One window; commit, log and resolve are its pages, reached from each other.
    OgWindow window(root);
    if (command == QStringLiteral("resolve")) {
        window.go(OgWindow::Resolve, file);
        window.resize(1500, 900);
    } else {
        window.go(command == QStringLiteral("log") ? OgWindow::Log : OgWindow::Commit);
        window.resize(1400, 860);
    }
    window.restoreWindowGeometry();   // saved geometry overrides first-run defaults
    window.show();
    return app.exec();
}
