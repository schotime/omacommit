#include "Agent.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {
bool isBatchFile(const QString &path)
{
#ifdef Q_OS_WIN
    return path.endsWith(QStringLiteral(".cmd"), Qt::CaseInsensitive)
        || path.endsWith(QStringLiteral(".bat"), Qt::CaseInsensitive);
#else
    Q_UNUSED(path);
    return false;
#endif
}

QStringList batchArguments(const QString &path, const QStringList &args)
{
    // CreateProcess cannot launch npm's .cmd shims directly. Let cmd.exe do
    // it, preserving the arguments Qt quotes on the way to the subprocess.
    return QStringList{QStringLiteral("/d"), QStringLiteral("/c"), path} + args;
}

QString run(const QString &program, const QStringList &args, int *exitCode = nullptr)
{
    QProcess p;
    p.start(isBatchFile(program) ? QStringLiteral("cmd.exe") : program,
            isBatchFile(program) ? batchArguments(program, args) : args);
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

QString Agent::program() const
{
    return isBatchFile(executable) ? QStringLiteral("cmd.exe") : executable;
}

Agent Agent::detect()
{
    const QVector<Agent> agents = available();
    return agents.isEmpty() ? Agent{} : agents.first();
}

QVector<Agent> Agent::available()
{
    QVector<Agent> agents;
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("omarchy-default-agent"));
    const QString preferred = tool.isEmpty() ? QString() : run(tool, {});

    for (const QString &id : {QStringLiteral("claude"), QStringLiteral("codex"), QStringLiteral("opencode")}) {
        Agent a;
        a.id = id;
        a.name = id == u"claude" ? QStringLiteral("Claude Code")
               : id == u"codex" ? QStringLiteral("Codex") : QStringLiteral("OpenCode");
        a.supported = true;
        a.executable = QStandardPaths::findExecutable(id);
        if (a.executable.isEmpty())
            continue;
#ifdef Q_OS_WIN
        // npm installs extensionless Unix shims beside the Windows .cmd file.
        if (QFileInfo(a.executable).suffix().isEmpty()) {
            const QString exe = a.executable + QStringLiteral(".exe");
            const QString cmd = a.executable + QStringLiteral(".cmd");
            if (QFileInfo::exists(exe))
                a.executable = exe;
            else if (QFileInfo::exists(cmd))
                a.executable = cmd;
            else
                continue;
        }
#else
        // Omarchy's PATH includes install-on-first-use mise wrappers. Do not
        // launch one merely to test availability (it would install an agent).
        const QFileInfo fi(a.executable);
        QFile f(a.executable);
        if (!fi.isSymLink() && f.open(QIODevice::ReadOnly) && f.read(4096).contains("mise use -g"))
            continue;
#endif
        a.installed = true;
        if (id == u"opencode") {
            const QString version = run(a.executable, {QStringLiteral("--version")});
            a.opencodeV2 = version.startsWith(QStringLiteral("opencode v2"), Qt::CaseInsensitive)
                || version.startsWith(QStringLiteral("2."));
        }
        if (id == preferred)
            agents.prepend(a);
        else
            agents.append(a);
    }
    return agents;
}

QStringList Agent::arguments(const QString &replyFile) const
{
    QStringList args;
    if (id == u"claude")
        // Print mode, no tools at all, no MCP servers, nothing saved.
        args = {QStringLiteral("-p"), QStringLiteral("--tools"), QString(), QStringLiteral("--strict-mcp-config"),
                QStringLiteral("--no-session-persistence"), QStringLiteral("--output-format"), QStringLiteral("text")};
    else if (id == u"codex")
        // exec reads "-" from stdin; a read-only sandbox, nothing saved, and
        // the final message in a file instead of mixed with its progress output.
        args = {QStringLiteral("exec"), QStringLiteral("--sandbox"), QStringLiteral("read-only"),
                QStringLiteral("--ephemeral"), QStringLiteral("--skip-git-repo-check"), QStringLiteral("--color"),
                 QStringLiteral("never"), QStringLiteral("-o"), replyFile, QStringLiteral("-")};
    else if (id == u"opencode") {
        // v2 accepts a prompt on stdin; v1 needs an attached file for large
        // diffs that would exceed Windows' command-line length limit.
        // JSON events let us extract just the model's text rather than progress.
        if (opencodeV2)
            args = {QStringLiteral("run"), QStringLiteral("--standalone"), QStringLiteral("--agent"),
                    QStringLiteral("oc-commit-writer"), QStringLiteral("--format"), QStringLiteral("json")};
        else
            // Keep --file last: v1's array parser consumes all later values.
            args = {QStringLiteral("--pure"), QStringLiteral("run"), QStringLiteral("--format"),
                QStringLiteral("json"), QStringLiteral("Read the attached prompt and answer it. Do not use tools."),
                QStringLiteral("--file"), replyFile};
    }
    return isBatchFile(executable) ? batchArguments(executable, args) : args;
}
