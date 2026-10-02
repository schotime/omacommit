#include "GitRepo.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
QProcessEnvironment gitEnv()
{
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));  // never hang on a tty prompt
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));   // don't fight other tools over index.lock
    return env;
}
} // namespace

bool FileEntry::conflicted() const
{
    return index == u'U' || worktree == u'U' || (index == u'A' && worktree == u'A') || (index == u'D' && worktree == u'D');
}

QString FileEntry::statusText() const
{
    if (untracked()) return QStringLiteral("Untracked");
    if (conflicted()) return QStringLiteral("Conflicted");
    const QChar c = staged ? index : worktree;
    if (c == u'R') return QStringLiteral("Renamed");
    if (c == u'C') return QStringLiteral("Copied");
    if (c == u'A') return QStringLiteral("Added");
    if (c == u'D') return QStringLiteral("Deleted");
    if (c == u'T') return QStringLiteral("Type changed");
    return QStringLiteral("Modified");
}

QString GitRepo::findRoot(const QString &startDir)
{
    QProcess p;
    p.setWorkingDirectory(startDir);
    p.setProcessEnvironment(gitEnv());
#ifdef Q_OS_WIN
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    p.start(QStringLiteral("git"), {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")});
    if (!p.waitForFinished(5000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        return {};
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

GitRepo::GitRepo(const QString &root) : m_root(root) {}

void GitRepo::configure(QProcess &proc, const QString &indexFile) const
{
#ifdef Q_OS_WIN
    // Git reads/writes pipes; it needs no console, even when oc has detached
    // from its own console for a desktop shortcut.
    proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    proc.setWorkingDirectory(m_root);
    QProcessEnvironment env = gitEnv();
    if (!indexFile.isEmpty())
        env.insert(QStringLiteral("GIT_INDEX_FILE"), indexFile);
    proc.setProcessEnvironment(env);
}

GitResult GitRepo::run(const QStringList &args, const QByteArray &stdinData, int timeoutMs,
                        const QString &indexFile, bool noEditor) const
{
    GitResult r;
    QProcess p;
    configure(p, indexFile);
    if (noEditor) {
        auto env = p.processEnvironment();
        env.insert(QStringLiteral("GIT_EDITOR"), QStringLiteral("true"));
        p.setProcessEnvironment(env);
    }
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
    return QString::fromUtf8(run({QStringLiteral("log"), QStringLiteral("--no-show-signature"), QStringLiteral("-1"), QStringLiteral("--format=%B")}).out).trimmed();
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

QVector<BranchRef> GitRepo::branches() const
{
    const GitResult r = run({QStringLiteral("for-each-ref"), QStringLiteral("--sort=-committerdate"),
                             QStringLiteral("--format=%(refname)%00%(refname:short)%00%(committerdate:relative)"),
                             QStringLiteral("refs/heads"), QStringLiteral("refs/remotes")});
    const QString current = branch();
    QVector<BranchRef> local, remote;
    QSet<QString> localNames;
    for (const QByteArray &line : r.out.split('\n')) {
        const QList<QByteArray> f = line.split('\0');
        if (f.size() < 3)
            continue;
        const QString full = QString::fromUtf8(f.at(0));
        BranchRef b;
        b.name = QString::fromUtf8(f.at(1));
        b.when = QString::fromUtf8(f.at(2));
        if (full.startsWith(QLatin1String("refs/heads/"))) {
            b.localName = b.name;
            b.current = b.name == current;
            localNames.insert(b.name);
            local << b;
        } else if (!full.endsWith(QLatin1String("/HEAD"))) {   // origin/HEAD just repeats the default branch
            b.remote = true;
            b.localName = b.name.section(u'/', 1);
            remote << b;
        }
    }
    QVector<BranchRef> out = local;
    for (const BranchRef &b : remote)
        if (!localNames.contains(b.localName)) {
            localNames.insert(b.localName);   // the same branch on a second remote: once is enough
            out << b;
        }
    return out;
}

QString GitRepo::emptyTree() const
{
    // Works for both SHA-1 and SHA-256 repositories. Empty stdin rather than
    // /dev/null, which is not a file on Windows.
    return QString::fromUtf8(run({QStringLiteral("hash-object"), QStringLiteral("-t"), QStringLiteral("tree"),
                                  QStringLiteral("--stdin")}).out).trimmed();
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

CommitSnapshot GitRepo::commitSnapshot(bool amend, bool includePreparedMessage) const
{
    CommitSnapshot d;
    d.base = commitBase(amend);
    d.entries = stagedFiles(d.base);
    if (amend) {
        d.lastMessage = lastCommitMessage();
        for (const FileEntry &e : stagedFiles(QStringLiteral("HEAD")))
            d.indexChanged.insert(e.path);
        // The last commit's files come first, in the commit's order.
        QHash<QString, int> order;
        const auto last = changedFiles(d.base, QStringLiteral("HEAD"));
        for (int i = 0; i < last.size(); ++i)
            order.insert(last.at(i).path, i);
        std::stable_sort(d.entries.begin(), d.entries.end(), [&](const FileEntry &a, const FileEntry &b) {
            return order.value(a.path, INT_MAX) < order.value(b.path, INT_MAX);
        });
    }
    d.entries += unstagedFiles();
    d.branch = branch();
    d.merging = isMerging();
    const QString dir = gitDir();
    d.gitDirectory = dir;
    if (!dir.isEmpty()) {
        QFile ignore(dir + QStringLiteral("/omacommit-ignore"));
        if (ignore.open(QIODevice::ReadOnly)) {
            for (const QString &raw : QString::fromUtf8(ignore.readAll()).split(u'\n')) {
                const QString line = raw.trimmed();
                if (!line.isEmpty() && !line.startsWith(u'#'))
                    d.ignorePatterns << line;
            }
        }
        // Git's prepared merge/cherry-pick/revert message, without instructions.
        QFile prepared(dir + QStringLiteral("/MERGE_MSG"));
        if (includePreparedMessage && prepared.open(QIODevice::ReadOnly)) {
            QStringList keep;
            for (const QString &line : QString::fromUtf8(prepared.readAll()).split(u'\n'))
                if (!line.startsWith(u'#'))
                    keep << line;
            d.preparedMessage = keep.join(u'\n').trimmed();
        }
    }
    return d;
}


// Files that differ between two trees, with what happened to each in
// commitStatus (M, A, D, R, C, T).
QVector<FileEntry> GitRepo::changedFiles(const QString &from, const QString &to) const
{
    const auto r = run({QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                        QStringLiteral("--name-status"), QStringLiteral("-z"), QStringLiteral("-M"), from, to});
    return r.ok() ? parseNameStatus(r.out) : QVector<FileEntry>();
}

// `diff --name-status -z` output: status, then the path -- or, for a rename
// or copy, the old path and the new one.
QVector<FileEntry> GitRepo::parseNameStatus(const QByteArray &data)
{
    QVector<FileEntry> out;
    const QList<QByteArray> parts = data.split('\0');
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

QString GitRepo::commitBase(bool amend) const
{
    if (!hasHead())
        return emptyTree();
    return amend ? headParent() : QStringLiteral("HEAD");
}

QVector<FileEntry> GitRepo::stagedFiles(const QString &base) const
{
    const auto r = run({QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                        QStringLiteral("--cached"), QStringLiteral("--name-status"), QStringLiteral("-z"),
                        QStringLiteral("-M"), base});
    QVector<FileEntry> out;
    if (!r.ok())
        return out;
    for (FileEntry e : parseNameStatus(r.out)) {
        if (e.commitStatus == u'U')
            continue;   // a conflict: listed with the unstaged changes, to resolve
        e.index = e.commitStatus;
        e.commitStatus = u' ';
        e.staged = true;
        out << e;
    }
    return out;
}

QVector<FileEntry> GitRepo::unstagedFiles() const
{
    QVector<FileEntry> out;
    for (FileEntry e : status()) {
        if (e.untracked() || e.conflicted()) {
            out << e;
        } else if (e.worktree != u' ') {
            e.index = u' ';
            e.oldPath.clear();   // a rename is the staged side's business
            out << e;
        }
    }
    return out;
}

QStringList GitRepo::fullDiffArgs() const
{
    // Full-file context so the side-by-side view shows the whole file.
    QStringList args{QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                     QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"), QStringLiteral("--histogram"),
                     QStringLiteral("-U1000000")};
    if (s_ignoreWhitespace)
        args << QStringLiteral("-w");
    return args;
}

QByteArray GitRepo::diffStaged(const FileEntry &f, const QString &base) const
{
    QStringList args = fullDiffArgs();
    args << QStringLiteral("--cached") << QStringLiteral("-M") << base << QStringLiteral("--");
    if (!f.oldPath.isEmpty())
        args << f.oldPath;
    return run(args << f.path).out;
}

QByteArray GitRepo::diffUnstaged(const FileEntry &f) const
{
    QStringList args = fullDiffArgs();
    if (f.untracked())
        // Exits 1 when the files differ; that's expected. git reads /dev/null
        // as "no file" on every platform.
        return run(args << QStringLiteral("--no-index") << QStringLiteral("--") << QStringLiteral("/dev/null")
                        << f.path).out;
    return run(args << QStringLiteral("--") << f.path).out;
}

QVector<FileEntry> GitRepo::workingChanges(const QString &base) const
{
    const auto r = run({QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                        QStringLiteral("--name-status"), QStringLiteral("-z"), QStringLiteral("-M"), base});
    QVector<FileEntry> out = r.ok() ? parseNameStatus(r.out) : QVector<FileEntry>();
    for (const FileEntry &e : status())
        if (e.untracked()) {
            FileEntry u = e;
            u.commitStatus = u'?';
            out << u;
        }
    return out;
}

QByteArray GitRepo::diffWorking(const FileEntry &f, const QString &base) const
{
    if (f.commitStatus == u'?') {
        FileEntry u = f;
        u.index = u'?';
        return diffUnstaged(u);
    }
    QStringList args = fullDiffArgs();
    args << QStringLiteral("-M") << base << QStringLiteral("--");
    if (!f.oldPath.isEmpty())
        args << f.oldPath;
    return run(args << f.path).out;
}

QByteArray GitRepo::indexBlob(const QString &path, bool *exists) const
{
    const GitResult r = run({QStringLiteral("cat-file"), QStringLiteral("blob"), QStringLiteral(":") + path});
    if (exists)
        *exists = r.ok();
    return r.ok() ? r.out : QByteArray();
}

GitResult GitRepo::setIndexContent(const QString &path, const QByteArray &data) const
{
    // Keep the index's mode; a file new to the index takes the working tree's.
    QString mode;
    const QList<QByteArray> fields =
        run({QStringLiteral("ls-files"), QStringLiteral("-s"), QStringLiteral("--"), path}).out.split(' ');
    if (fields.size() > 1)
        mode = QString::fromLatin1(fields.first());
    if (mode.isEmpty())
        mode = QFileInfo(QDir(m_root).filePath(path)).isExecutable() ? QStringLiteral("100755")
                                                                     : QStringLiteral("100644");
    GitResult r = run({QStringLiteral("hash-object"), QStringLiteral("-w"), QStringLiteral("--stdin")}, data);
    if (!r.ok())
        return r;
    const QString sha = QString::fromLatin1(r.out).trimmed();
    return run({QStringLiteral("update-index"), QStringLiteral("--add"), QStringLiteral("--cacheinfo"),
                mode + u',' + sha + u',' + path});
}

GitResult GitRepo::prepareCommitIndex(const QString &indexFile, const QString &base, const QStringList &fromIndex,
                                      const QStringList &fromWorktree) const
{
    GitResult r = run({QStringLiteral("read-tree"), base}, {}, 30000, indexFile);
    if (!r.ok())
        return r;
    if (!fromIndex.isEmpty()) {
        // The real index's entries for these paths, as-is; a path it doesn't
        // have (a staged deletion, a rename's old name) goes.
        QStringList ls{QStringLiteral("ls-files"), QStringLiteral("-s"), QStringLiteral("-z"), QStringLiteral("--")};
        const GitResult entries = run(ls << fromIndex);
        if (!entries.ok())
            return entries;
        // Only the scratch index is touched, so none of `rm --cached`'s checks
        // for losing staged work apply.
        QStringList rm{QStringLiteral("update-index"), QStringLiteral("--force-remove"), QStringLiteral("--")};
        r = run(rm << fromIndex, {}, 30000, indexFile);
        if (!r.ok())
            return r;
        QByteArray info = entries.out;
        info.replace('\0', '\n');
        r = run({QStringLiteral("update-index"), QStringLiteral("--index-info")}, info, 30000, indexFile);
        if (!r.ok())
            return r;
    }
    if (!fromWorktree.isEmpty()) {
        QStringList args{QStringLiteral("add"), QStringLiteral("-A"), QStringLiteral("--")};
        r = run(args << fromWorktree, {}, 30000, indexFile);
    }
    return r;
}

QByteArray GitRepo::diffBetween(const QString &from, const QString &to, const FileEntry &f) const
{
    QStringList args{QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                     QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"), QStringLiteral("--histogram"),
                     QStringLiteral("-U1000000"), QStringLiteral("-M"), from, to, QStringLiteral("--")};
    if (s_ignoreWhitespace)
        args.insert(args.indexOf(QStringLiteral("-M")), QStringLiteral("-w"));
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
    // Unit and record separators can't appear in any of these fields. (Every
    // log call says --no-show-signature: log.showSignature would otherwise put
    // GPG/SSH verification text in among the formatted output.)
    QStringList args{QStringLiteral("log"), QStringLiteral("--no-show-signature"), QStringLiteral("--topo-order"),
                     QStringLiteral("--no-color"),
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
    return QString::fromUtf8(run({QStringLiteral("log"), QStringLiteral("--no-show-signature"), QStringLiteral("-1"), QStringLiteral("--format=%B"), hash}).out)
        .trimmed();
}

QString GitRepo::gitDir() const
{
    return QString::fromUtf8(run({QStringLiteral("rev-parse"), QStringLiteral("--absolute-git-dir")}).out).trimmed();
}

QString GitRepo::scratchIndexPath() const
{
    const QString dir = gitDir();
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/og-commit-index");
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

ResolveSnapshot GitRepo::resolveSnapshot() const
{
    ResolveSnapshot d;
    d.operation = operation();
    d.branch = branch();
    d.conflicts = !unmerged().isEmpty();
    auto out = [this](const QStringList &args) { return QString::fromUtf8(run(args).out).trimmed(); };
    auto nameOf = [&](const QString &rev) {
        QString name = out({QStringLiteral("name-rev"), QStringLiteral("--name-only"), rev});
        if (name.isEmpty() || name == u"undefined")
            name = out({QStringLiteral("rev-parse"), QStringLiteral("--short"), rev});
        return name.remove(QStringLiteral("remotes/"));
    };
    auto describe = [&](const QString &rev) {
        return out({QStringLiteral("log"), QStringLiteral("--no-show-signature"), QStringLiteral("-1"),
                    QStringLiteral("--format=%h %s"), rev});
    };
    if (d.operation == u"merge")
        d.other = nameOf(QStringLiteral("MERGE_HEAD"));
    else if (d.operation == u"rebase") {
        const QString dir = gitDir();
        for (const char *path : {"/rebase-merge/onto", "/rebase-apply/onto"}) {
            QFile f(dir + QLatin1String(path));
            if (f.open(QIODevice::ReadOnly)) {
                const QString sha = QString::fromLatin1(f.readAll()).trimmed();
                d.onto = nameOf(sha);
                d.replayed = out({QStringLiteral("rev-list"), QStringLiteral("--count"), sha + QStringLiteral("..HEAD")}).toInt();
                break;
            }
        }
        d.replaying = describe(QStringLiteral("REBASE_HEAD"));
    } else if (d.operation == u"cherry-pick")
        d.picked = describe(QStringLiteral("CHERRY_PICK_HEAD"));
    else if (d.operation == u"revert")
        d.reverted = describe(QStringLiteral("REVERT_HEAD"));
    return d;
}

GitResult GitRepo::continueRebase() const
{
    return run({QStringLiteral("rebase"), QStringLiteral("--continue")}, {}, 1800000, {}, true);
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
// Two arbitrary files, with the same options as diff() so the rows line up
// the same way. Exits 1 when they differ; that's expected.
QByteArray GitRepo::diffFiles(const QString &a, const QString &b) const
{
    QStringList args{QStringLiteral("diff"), QStringLiteral("--no-index"), QStringLiteral("--no-color"),
                     QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"), QStringLiteral("--histogram"),
                     QStringLiteral("-U1000000")};
    if (s_ignoreWhitespace)
        args << QStringLiteral("-w");
    args << QStringLiteral("--") << a << b;
    return run(args).out;
}

GitResult GitRepo::runSequence(const QVector<QStringList> &commands) const
{
    GitResult result;
    result.exitCode = 0;
    for (const auto &command : commands) {
        result = run(command);
        if (!result.ok())
            break;
    }
    return result;
}

GitResult GitRepo::unstageFiles(const QStringList &paths) const
{
    QStringList args = hasHead()
        ? QStringList{QStringLiteral("restore"), QStringLiteral("--staged"), QStringLiteral("--")}
        : QStringList{QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-q"), QStringLiteral("--")};
    return run(args << paths);
}

GitResult GitRepo::replaceIndexContent(const QString &path, const QByteArray &expected,
                                      const QByteArray &data, bool remove) const
{
    if (indexBlob(path) != expected) {
        GitResult result;
        result.err = "The file changed in the index meanwhile. Refresh and try again.";
        return result;
    }
    return remove ? run({QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-q"),
                         QStringLiteral("--"), path}) : setIndexContent(path, data);
}

QByteArray GitRepo::diffContents(const QByteArray &before, const QByteArray &after) const
{
    QTemporaryDir tmp;
    const QString left = tmp.filePath(QStringLiteral("before"));
    const QString right = tmp.filePath(QStringLiteral("after"));
    QFile a(left), b(right);
    if (!a.open(QIODevice::WriteOnly) || !b.open(QIODevice::WriteOnly)
        || a.write(before) != before.size() || b.write(after) != after.size())
        return {};
    a.close();
    b.close();
    return diffFiles(left, right);
}

CommitOutcome GitRepo::commitSelection(const QString &message, bool amend, const QString &newBranch,
                                       const QStringList &fromIndex, const QStringList &fromWorktree) const
{
    CommitOutcome out;
    if (!newBranch.isEmpty()) {
        if (!isValidBranchName(newBranch)) {
            out.result.err = "Invalid Git branch name.";
            return out;
        }
        if (branchExists(newBranch)) {
            out.result.err = "The branch already exists. Choose another name or clear the branch field.";
            return out;
        }
        out.result = createBranch(newBranch);
        if (!out.result.ok())
            return out;
    }
    const bool merging = isMerging();
    QString indexFile;
    if (merging) {
        if (!fromWorktree.isEmpty()) {
            out.result = run(QStringList{QStringLiteral("add"), QStringLiteral("-A"), QStringLiteral("--")} << fromWorktree);
            if (!out.result.ok())
                return out;
        }
    } else {
        indexFile = scratchIndexPath();
        if (indexFile.isEmpty()) {
            out.result.err = "Could not locate the repository's Git directory.";
            return out;
        }
        QFile::remove(indexFile);
        out.result = prepareCommitIndex(indexFile, commitBase(amend), fromIndex, fromWorktree);
        if (!out.result.ok()) {
            QFile::remove(indexFile);
            return out;
        }
    }
    QStringList args{QStringLiteral("commit"), QStringLiteral("-F"), QStringLiteral("-")};
    if (amend)
        args << QStringLiteral("--amend");
    out.result = run(args, message.toUtf8(), 1800000, indexFile);
    if (!indexFile.isEmpty())
        QFile::remove(indexFile);
    if (!out.result.ok())
        return out;
    if (!merging && !fromWorktree.isEmpty()) {
        const auto reset = run(QStringList{QStringLiteral("reset"), QStringLiteral("-q"), QStringLiteral("--")} << fromWorktree);
        if (!reset.ok())
            out.result.err += "\nCommitted, but could not update the real index:\n" + reset.err;
    }
    out.hash = QString::fromUtf8(run({QStringLiteral("rev-parse"), QStringLiteral("--short"), QStringLiteral("HEAD")}).out).trimmed();
    return out;
}

GitResult GitRepo::pushCurrentBranch() const
{
    QStringList args{QStringLiteral("push")};
    if (upstream().isEmpty()) {
        const QString name = branch();
        const QStringList destinations = remotes();
        if (!name.isEmpty() && !destinations.isEmpty()) {
            const QString remote = destinations.contains(QStringLiteral("origin")) ? QStringLiteral("origin") : destinations.first();
            args << QStringLiteral("-u") << remote << name;
        }
    }
    return run(args, {}, 1800000);
}

QString GitRepo::selectedChanges(const QString &base, const QStringList &staged,
                                 const QStringList &worktree, const QStringList &untracked) const
{
    QString diff;
    auto baseDiff = [&](bool cached, const QStringList &paths) {
        QStringList args{QStringLiteral("-c"), QStringLiteral("core.quotepath=off"), QStringLiteral("diff"),
                         QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"), QStringLiteral("-M")};
        if (cached)
            args << QStringLiteral("--cached");
        return QString::fromUtf8(run(args << base << QStringLiteral("--") << paths).out);
    };
    if (!staged.isEmpty())
        diff += baseDiff(true, staged);
    if (!worktree.isEmpty())
        diff += baseDiff(false, worktree);
    // A real empty file works on Windows as well as Linux.
    QTemporaryDir tmp;
    const QString empty = tmp.filePath(QStringLiteral("empty"));
    QFile file(empty);
    if (file.open(QIODevice::WriteOnly)) {
        file.close();
        for (const QString &path : untracked)
            diff += QString::fromUtf8(run({QStringLiteral("diff"), QStringLiteral("--no-color"),
                                           QStringLiteral("--no-ext-diff"), QStringLiteral("--no-index"),
                                           QStringLiteral("--"), empty, path}).out);
    }
    return diff;
}

WhitespaceRules GitRepo::whitespaceRules(const QString &path) const
{
    WhitespaceRules r;
    // "<path>: whitespace: <unspecified|set|unset|rules>"
    const QString line = QString::fromUtf8(run({QStringLiteral("check-attr"), QStringLiteral("whitespace"),
                                                QStringLiteral("--"), path}).out).trimmed();
    const QString value = line.section(QStringLiteral(": "), -1);
    if (value == u"unset") {
        r.check = false;
    } else if (value == u"set") {
        // What git applies for a bare `whitespace` attribute: every rule that
        // flags an error and isn't opt-in.
        r.rules = QStringLiteral("blank-at-eol,blank-at-eof,space-before-tab,indent-with-non-tab");
    } else if (value != u"unspecified" && !value.isEmpty()) {
        r.rules = value;
    } else {
        r.rules = QString::fromUtf8(run({QStringLiteral("config"), QStringLiteral("--get"),
                                         QStringLiteral("core.whitespace")}).out).trimmed();
    }
    return r;
}

QHash<int, QString> GitRepo::whitespaceIssues(const WhitespaceRules &rules, const QByteArray &oldText,
                                              const QByteArray &newText) const
{
    QHash<int, QString> out;
    if (!rules.check || newText.isEmpty())
        return out;
    QTemporaryDir tmp;
    const QString oldPath = tmp.filePath(QStringLiteral("old"));
    const QString newPath = tmp.filePath(QStringLiteral("new"));
    for (const auto &[file, data] : {std::pair{oldPath, oldText}, std::pair{newPath, newText}}) {
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly))
            return out;
        f.write(data);
    }
    // --no-index doesn't read .gitattributes, so the path's rules come in
    // through core.whitespace -- plus cr-at-eol: a CRLF line ending is a line
    // ending, not trailing whitespace, and flagging it would mark every line
    // of a CRLF file. A space before the \r\n is still flagged.
    const QString effective = rules.rules.isEmpty() ? QStringLiteral("cr-at-eol")
                                                    : rules.rules + QStringLiteral(",cr-at-eol");
    QStringList args{QStringLiteral("-c"), QStringLiteral("core.whitespace=") + effective};
    args << QStringLiteral("diff") << QStringLiteral("--no-index") << QStringLiteral("--check")
         << QStringLiteral("--no-color") << QStringLiteral("--") << oldPath << newPath;
    static const QRegularExpression re(QStringLiteral(R"(^(.*):(\d+): (.+)\.$)"));
    for (const QString &line : QString::fromUtf8(run(args).out).split(u'\n')) {
        const auto m = re.match(line);
        if (!m.hasMatch() || !m.captured(1).endsWith(QLatin1String("new")))
            continue;
        const int n = m.captured(2).toInt();
        out[n] = out.value(n).isEmpty() ? m.captured(3) : out.value(n) + QStringLiteral("; ") + m.captured(3);
    }
    return out;
}
