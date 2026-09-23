#include "GitRepo.h"

#include <QProcess>
#include <QProcessEnvironment>

namespace {
QProcessEnvironment gitEnv()
{
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));  // never hang on a tty prompt
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));   // don't fight other tools over index.lock
    return env;
}
} // namespace

QString FileEntry::statusText() const
{
    if (inLastCommit && !worktreeChange())
        return QStringLiteral("In last commit");
    const QString base = worktreeStatusText();
    return inLastCommit ? base + QStringLiteral(" + last commit") : base;
}

QString FileEntry::worktreeStatusText() const
{
    if (index == u'?') return QStringLiteral("Untracked");
    if (index == u'U' || worktree == u'U' || (index == u'A' && worktree == u'A') || (index == u'D' && worktree == u'D'))
        return QStringLiteral("Conflicted");
    if (index == u'R') return QStringLiteral("Renamed");
    if (index == u'C') return QStringLiteral("Copied");
    if (index == u'A') return QStringLiteral("Added");
    if (index == u'D' || worktree == u'D') return QStringLiteral("Deleted");
    if (index == u'T' || worktree == u'T') return QStringLiteral("Type changed");
    return QStringLiteral("Modified");
}

QString GitRepo::findRoot(const QString &startDir)
{
    QProcess p;
    p.setWorkingDirectory(startDir);
    p.setProcessEnvironment(gitEnv());
    p.start(QStringLiteral("git"), {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")});
    if (!p.waitForFinished(5000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        return {};
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

GitRepo::GitRepo(const QString &root) : m_root(root) {}

void GitRepo::configure(QProcess &proc, const QString &indexFile) const
{
    proc.setWorkingDirectory(m_root);
    QProcessEnvironment env = gitEnv();
    if (!indexFile.isEmpty())
        env.insert(QStringLiteral("GIT_INDEX_FILE"), indexFile);
    proc.setProcessEnvironment(env);
}

GitResult GitRepo::run(const QStringList &args, const QByteArray &stdinData, int timeoutMs,
                       const QString &indexFile) const
{
    GitResult r;
    QProcess p;
    configure(p, indexFile);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(5000)) {
        r.err = "could not start git";
        return r;
    }
    if (!stdinData.isEmpty())
        p.write(stdinData);
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        r.err = "git timed out";
        return r;
    }
    r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.out = p.readAllStandardOutput();
    r.err = p.readAllStandardError();
    return r;
}

QString GitRepo::branch() const
{
    return QString::fromUtf8(run({QStringLiteral("branch"), QStringLiteral("--show-current")}).out).trimmed();
}

QString GitRepo::upstream() const
{
    const auto r = run({QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"),
                        QStringLiteral("--symbolic-full-name"), QStringLiteral("@{u}")});
    return r.ok() ? QString::fromUtf8(r.out).trimmed() : QString();
}

QStringList GitRepo::remotes() const
{
    return QString::fromUtf8(run({QStringLiteral("remote")}).out).split(u'\n', Qt::SkipEmptyParts);
}

bool GitRepo::hasHead() const
{
    return run({QStringLiteral("rev-parse"), QStringLiteral("-q"), QStringLiteral("--verify"), QStringLiteral("HEAD")}).ok();
}

bool GitRepo::isMerging() const
{
    return run({QStringLiteral("rev-parse"), QStringLiteral("-q"), QStringLiteral("--verify"), QStringLiteral("MERGE_HEAD")}).ok();
}

QString GitRepo::lastCommitMessage() const
{
    return QString::fromUtf8(run({QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%B")}).out).trimmed();
}

bool GitRepo::branchExists(const QString &name) const
{
    return run({QStringLiteral("show-ref"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
                QStringLiteral("refs/heads/") + name}).ok();
}

bool GitRepo::isValidBranchName(const QString &name) const
{
    // A leading dash would be read as an option by git itself, so reject it
    // before asking; check-ref-format covers the rest of the rules.
    if (name.isEmpty() || name.startsWith(u'-'))
        return false;
    return run({QStringLiteral("check-ref-format"), QStringLiteral("--branch"), name}).ok();
}

// Carries the working tree and index across, so the pending changes are still
// there to be committed on the new branch.
GitResult GitRepo::createBranch(const QString &name) const
{
    return run({QStringLiteral("checkout"), QStringLiteral("-b"), name});
}

QString GitRepo::emptyTree() const
{
    // Works for both SHA-1 and SHA-256 repositories.
    return QString::fromUtf8(run({QStringLiteral("hash-object"), QStringLiteral("-t"), QStringLiteral("tree"),
                                  QStringLiteral("/dev/null")}).out).trimmed();
}

QVector<FileEntry> GitRepo::status() const
{
    QVector<FileEntry> out;
    const auto r = run({QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("-z"),
                        QStringLiteral("--untracked-files=all")});
    if (!r.ok())
        return out;

    const QList<QByteArray> parts = r.out.split('\0');
    for (int i = 0; i < parts.size(); ++i) {
        const QByteArray &p = parts.at(i);
        if (p.size() < 4)
            continue;
        FileEntry e;
        e.index = QChar::fromLatin1(p.at(0));
        e.worktree = QChar::fromLatin1(p.at(1));
        e.path = QString::fromUtf8(p.mid(3));
        if ((e.index == u'R' || e.index == u'C') && i + 1 < parts.size())
            e.oldPath = QString::fromUtf8(parts.at(++i));
        if (e.index == u'!')
            continue;
        out.push_back(e);
    }
    return out;
}

QString GitRepo::headParent() const
{
    const auto r = run({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
                        QStringLiteral("HEAD^1")});
    const QString p = QString::fromUtf8(r.out).trimmed();
    return p.isEmpty() ? emptyTree() : p;   // amending the root commit
}

// What the commit being amended actually changed, so the dialog can offer to
// drop any of it.
QVector<FileEntry> GitRepo::lastCommitFiles() const
{
    QVector<FileEntry> out;
    if (!hasHead())
        return out;
    const auto r = run({QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                        QStringLiteral("--name-status"), QStringLiteral("-z"), QStringLiteral("-M"),
                        headParent(), QStringLiteral("HEAD")});
    if (!r.ok())
        return out;

    const QList<QByteArray> parts = r.out.split('\0');
    int i = 0;
    while (i + 1 < parts.size()) {
        const QByteArray st = parts.at(i);
        if (st.isEmpty()) {
            ++i;
            continue;
        }
        const char code = st.at(0);
        const bool paired = (code == 'R' || code == 'C');   // status, old, new
        if (paired && i + 2 >= parts.size())
            break;
        FileEntry e;
        e.inLastCommit = true;
        if (paired) {
            e.oldPath = QString::fromUtf8(parts.at(i + 1));
            e.path = QString::fromUtf8(parts.at(i + 2));
        } else {
            e.path = QString::fromUtf8(parts.at(i + 1));
        }
        out.push_back(e);
        i += paired ? 3 : 2;
    }
    return out;
}

QString GitRepo::scratchIndexPath() const
{
    const QString dir = QString::fromUtf8(
        run({QStringLiteral("rev-parse"), QStringLiteral("--absolute-git-dir")}).out).trimmed();
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/og-amend-index");
}

// Builds the tree for an amend that drops files the commit currently has.
// `git commit --amend` commits whatever the index holds, so the index is what
// has to be shaped -- but shaping the real one would disturb whatever the user
// has staged, so this works in a scratch index that the commit is then pointed
// at. Hooks and signing still run, because it is still a plain `git commit`.
GitResult GitRepo::prepareAmendIndex(const QString &indexFile, const QStringList &include,
                                     const QStringList &exclude) const
{
    GitResult r = run({QStringLiteral("read-tree"), QStringLiteral("HEAD")}, {}, 30000, indexFile);
    if (!r.ok())
        return r;
    if (!exclude.isEmpty()) {
        // Back to the parent's version, which removes files the commit added.
        // The working tree is untouched, so the change reappears as pending.
        QStringList args{QStringLiteral("reset"), QStringLiteral("-q"), headParent(), QStringLiteral("--")};
        args << exclude;
        r = run(args, {}, 30000, indexFile);
        if (!r.ok())
            return r;
    }
    if (!include.isEmpty()) {
        QStringList args{QStringLiteral("add"), QStringLiteral("-A"), QStringLiteral("--")};
        args << include;
        r = run(args, {}, 30000, indexFile);
    }
    return r;
}

// The real index still describes the commit that was just replaced, which would
// show up as phantom staged changes. Point the touched paths back at HEAD.
void GitRepo::refreshIndexAfterAmend(const QStringList &paths) const
{
    if (paths.isEmpty())
        return;
    QStringList args{QStringLiteral("reset"), QStringLiteral("-q"), QStringLiteral("--")};
    args << paths;
    run(args);
}

// Two arbitrary files, with the same options as diff() so the rows line up
// the same way. Exits 1 when they differ; that's expected.
QByteArray GitRepo::diffFiles(const QString &a, const QString &b) const
{
    return run({QStringLiteral("diff"), QStringLiteral("--no-index"), QStringLiteral("--no-color"),
                QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"), QStringLiteral("--histogram"),
                QStringLiteral("-U1000000"), QStringLiteral("--"), a, b}).out;
}

QByteArray GitRepo::diff(const FileEntry &f) const
{
    // Full-file context so the side-by-side view shows the whole file.
    QStringList args{QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                     QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"), QStringLiteral("--histogram"),
                     QStringLiteral("-U1000000")};
    if (f.untracked()) {
        args << QStringLiteral("--no-index") << QStringLiteral("--") << QStringLiteral("/dev/null") << f.path;
        return run(args).out;   // exits 1 when files differ; that's expected
    }
    if (f.inLastCommit && !f.worktreeChange()) {
        // Nothing pending for it; show what the commit being amended did.
        args << QStringLiteral("-M") << headParent() << QStringLiteral("HEAD") << QStringLiteral("--");
        if (!f.oldPath.isEmpty())
            args << f.oldPath;
        args << f.path;
        return run(args).out;
    }
    // Working tree vs HEAD: exactly what a commit of this file would record.
    args << QStringLiteral("-M") << (hasHead() ? QStringLiteral("HEAD") : emptyTree()) << QStringLiteral("--");
    if (!f.oldPath.isEmpty())
        args << f.oldPath;
    args << f.path;
    return run(args).out;
}

QStringList GitRepo::commitArgs(const QStringList &paths, bool amend, bool merging) const
{
    QStringList a{QStringLiteral("commit"), QStringLiteral("-F"), QStringLiteral("-")};
    if (amend)
        a << QStringLiteral("--amend");
    if (merging)
        return a;   // partial commits aren't allowed mid-merge; files were staged beforehand
    // --only commits exactly the checked paths from the working tree, ignoring
    // whatever else happens to be staged — the TortoiseGit model.
    if (!paths.isEmpty())
        a << QStringLiteral("--only") << QStringLiteral("--") << paths;
    else if (amend)
        a << QStringLiteral("--only");   // reword only
    return a;
}
