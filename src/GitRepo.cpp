#include "GitRepo.h"

#include <algorithm>

#include <QFile>
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
    if (!hasHead())
        return {};
    QVector<FileEntry> out = changedFiles(headParent(), QStringLiteral("HEAD"));
    for (FileEntry &e : out)
        e.inLastCommit = true;
    return out;
}

// Files that differ between two trees, with what happened to each in
// commitStatus (M, A, D, R, C, T).
QVector<FileEntry> GitRepo::changedFiles(const QString &from, const QString &to) const
{
    QVector<FileEntry> out;
    const auto r = run({QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                        QStringLiteral("--name-status"), QStringLiteral("-z"), QStringLiteral("-M"), from, to});
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
        e.commitStatus = QChar::fromLatin1(code);
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

QByteArray GitRepo::diffBetween(const QString &from, const QString &to, const FileEntry &f) const
{
    QStringList args{QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                     QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"), QStringLiteral("--histogram"),
                     QStringLiteral("-U1000000"), QStringLiteral("-M"), from, to, QStringLiteral("--")};
    if (!f.oldPath.isEmpty())
        args << f.oldPath;
    args << f.path;
    return run(args).out;
}

QVector<LogCommit> GitRepo::log(int skip, int count, bool allRefs) const
{
    QVector<LogCommit> out;
    if (!hasHead())
        return out;
    // Unit and record separators can't appear in any of these fields.
    QStringList args{QStringLiteral("log"), QStringLiteral("--topo-order"), QStringLiteral("--no-color"),
                     QStringLiteral("--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%s%x1e"),
                     QStringLiteral("--skip=%1").arg(skip), QStringLiteral("-n"), QString::number(count)};
    if (allRefs)
        args << QStringLiteral("--branches") << QStringLiteral("--remotes") << QStringLiteral("--tags");
    args << QStringLiteral("HEAD");
    const auto r = run(args, {}, 120000);
    if (!r.ok())
        return out;
    for (const QByteArray &rec : r.out.split('\x1e')) {
        const QList<QByteArray> f = rec.trimmed().split('\x1f');
        if (f.size() < 6)
            continue;
        LogCommit c;
        c.hash = QString::fromLatin1(f.at(0));
        c.parents = QString::fromLatin1(f.at(1)).split(u' ', Qt::SkipEmptyParts);
        c.author = QString::fromUtf8(f.at(2));
        c.email = QString::fromUtf8(f.at(3));
        c.time = f.at(4).toLongLong();
        c.subject = QString::fromUtf8(f.at(5));
        out.push_back(c);
    }
    return out;
}

QHash<QString, QVector<RefLabel>> GitRepo::refsByCommit() const
{
    QHash<QString, QVector<RefLabel>> out;
    // %(*objectname) is the commit an annotated tag points at.
    const auto r = run({QStringLiteral("for-each-ref"),
                        QStringLiteral("--format=%(objectname)%00%(*objectname)%00%(refname)%00%(refname:short)"),
                        QStringLiteral("refs/heads"), QStringLiteral("refs/remotes"), QStringLiteral("refs/tags")});
    const QString current = branch();
    for (const QByteArray &line : r.out.split('\n')) {
        const QList<QByteArray> f = line.split('\0');
        if (f.size() < 4)
            continue;
        const QString full = QString::fromUtf8(f.at(2));
        const QString name = QString::fromUtf8(f.at(3));
        RefLabel ref;
        ref.name = name;
        if (full.startsWith(QLatin1String("refs/heads/"))) {
            ref.kind = RefLabel::Branch;
            ref.current = name == current;
        } else if (full.startsWith(QLatin1String("refs/remotes/"))) {
            if (full.endsWith(QLatin1String("/HEAD")))
                continue;   // origin/HEAD just repeats the default branch
            ref.kind = RefLabel::Remote;
        } else {
            ref.kind = RefLabel::Tag;
        }
        const QByteArray peeled = f.at(1);
        out[QString::fromLatin1(peeled.isEmpty() ? f.at(0) : peeled)].push_back(ref);
    }
    if (current.isEmpty() && hasHead()) {   // detached: say where HEAD is
        const QString head = QString::fromLatin1(run({QStringLiteral("rev-parse"), QStringLiteral("HEAD")}).out).trimmed();
        RefLabel ref;
        ref.name = QStringLiteral("HEAD");
        ref.kind = RefLabel::Head;
        ref.current = true;
        out[head].prepend(ref);
    }
    for (auto &refs : out)   // current branch first, then branches, remotes, tags
        std::stable_sort(refs.begin(), refs.end(), [](const RefLabel &a, const RefLabel &b) {
            if (a.current != b.current)
                return a.current;
            return a.kind < b.kind;
        });
    return out;
}

QString GitRepo::commitMessage(const QString &hash) const
{
    return QString::fromUtf8(run({QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%B"), hash}).out)
        .trimmed();
}

QString GitRepo::gitDir() const
{
    return QString::fromUtf8(run({QStringLiteral("rev-parse"), QStringLiteral("--absolute-git-dir")}).out).trimmed();
}

QString GitRepo::scratchIndexPath() const
{
    const QString dir = gitDir();
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/og-amend-index");
}

// What left the conflicts behind, from the state files git keeps while it waits.
QString GitRepo::operation() const
{
    const QString d = gitDir();
    if (d.isEmpty())
        return {};
    if (QFile::exists(d + QStringLiteral("/rebase-merge")) || QFile::exists(d + QStringLiteral("/rebase-apply")))
        return QStringLiteral("rebase");
    if (QFile::exists(d + QStringLiteral("/MERGE_HEAD")))
        return QStringLiteral("merge");
    if (QFile::exists(d + QStringLiteral("/CHERRY_PICK_HEAD")))
        return QStringLiteral("cherry-pick");
    if (QFile::exists(d + QStringLiteral("/REVERT_HEAD")))
        return QStringLiteral("revert");
    return {};
}

QVector<UnmergedFile> GitRepo::unmerged() const
{
    QVector<UnmergedFile> out;
    // One record per stage: "<mode> <blob> <stage>\t<path>".
    const auto r = run({QStringLiteral("ls-files"), QStringLiteral("-u"), QStringLiteral("-z")});
    for (const QByteArray &rec : r.out.split('\0')) {
        const int tab = rec.indexOf('\t');
        if (tab < 0)
            continue;
        const QList<QByteArray> meta = rec.left(tab).split(' ');
        if (meta.size() < 3)
            continue;
        const QString path = QString::fromUtf8(rec.mid(tab + 1));
        const int stage = meta.at(2).toInt();
        if (stage < 1 || stage > 3)
            continue;
        if (out.isEmpty() || out.constLast().path != path)
            out.push_back(UnmergedFile{path, {}});
        out.last().stage[stage] = QString::fromLatin1(meta.at(1));
    }
    return out;
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

QByteArray GitRepo::diff(const FileEntry &f, const QString &base) const
{
    // Full-file context so the side-by-side view shows the whole file.
    QStringList args{QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                     QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"), QStringLiteral("--histogram"),
                     QStringLiteral("-U1000000")};
    if (f.untracked()) {
        args << QStringLiteral("--no-index") << QStringLiteral("--") << QStringLiteral("/dev/null") << f.path;
        return run(args).out;   // exits 1 when files differ; that's expected
    }
    // Working tree vs `base` -- HEAD by default, or the parent when amending,
    // since the amended commit replaces HEAD. Either way, exactly what the
    // commit will record for this file.
    const QString from = !base.isEmpty() ? base : hasHead() ? QStringLiteral("HEAD") : emptyTree();
    args << QStringLiteral("-M") << from << QStringLiteral("--");
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
