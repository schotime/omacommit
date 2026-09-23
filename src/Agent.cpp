#include "Agent.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {
QString run(const QString &program, const QStringList &args, int *exitCode = nullptr)
{
    QProcess p;
    p.start(program, args);
    if (!p.waitForFinished(5000)) {
        p.kill();
        p.waitForFinished(1000);
        if (exitCode)
            *exitCode = -1;
        return {};
    }
    if (exitCode)
        *exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}
} // namespace

Agent Agent::detect()
{
    Agent a;
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("omarchy-default-agent"));
    if (tool.isEmpty())
        return a;   // not Omarchy
    a.id = run(tool, {});
    if (a.id == u"claude") {
        a.name = QStringLiteral("Claude Code");
        a.supported = true;
    } else if (a.id == u"codex") {
        a.name = QStringLiteral("Codex");
        a.supported = true;
    } else {
        a.name = a.id;
    }
    if (a.id.isEmpty())
        return a;

    // Installed the way omarchy-default-agent decides it: the user's own copy
    // in ~/.local/bin (anything there other than Omarchy's `mise use -g`
    // wrapper), or a mise install. Omarchy puts install-on-first-run wrappers
    // on PATH, so being on PATH proves nothing -- running one would start an
    // install.
    const QString local = QDir::homePath() + QStringLiteral("/.local/bin/") + a.id;
    const QFileInfo fi(local);
    if (fi.isExecutable()) {
        QFile f(local);
        const bool wrapper = !fi.isSymLink() && f.open(QIODevice::ReadOnly) && f.readAll().contains("mise use -g");
        if (!wrapper)
            a.installed = true;
    }
    if (!a.installed && !QStandardPaths::findExecutable(QStringLiteral("mise")).isEmpty()) {
        int code = -1;
        run(QStringLiteral("mise"), {QStringLiteral("where"), a.id}, &code);
        a.installed = code == 0;
    }
    return a;
}

QStringList Agent::arguments(const QString &replyFile) const
{
    if (id == u"claude")
        // Print mode, no tools at all, no MCP servers, nothing saved.
        return {QStringLiteral("-p"), QStringLiteral("--tools"), QString(), QStringLiteral("--strict-mcp-config"),
                QStringLiteral("--no-session-persistence"), QStringLiteral("--output-format"), QStringLiteral("text")};
    if (id == u"codex")
        // exec reads "-" from stdin; a read-only sandbox, nothing saved, and
        // the final message in a file instead of mixed with its progress output.
        return {QStringLiteral("exec"), QStringLiteral("--sandbox"), QStringLiteral("read-only"),
                QStringLiteral("--ephemeral"), QStringLiteral("--skip-git-repo-check"), QStringLiteral("--color"),
                QStringLiteral("never"), QStringLiteral("-o"), replyFile, QStringLiteral("-")};
    return {};
}
