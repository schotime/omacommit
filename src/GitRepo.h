#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

class QProcess;

struct FileEntry {
    QString path;
    QString oldPath;          // set for renames/copies
    QChar index = u' ';       // porcelain X
    QChar worktree = u' ';    // porcelain Y
    bool inLastCommit = false;   // part of the commit being amended
    QChar commitStatus = u' ';   // what that commit did to it: M, A, D, R, ...

    bool untracked() const { return index == u'?'; }
    bool worktreeChange() const { return index != u' ' || worktree != u' '; }
    QString statusText() const;
    QString worktreeStatusText() const;
};

struct LogCommit {
    QString hash;
    QStringList parents;
    QString author, email;
    qint64 time = 0;   // author date, seconds since the epoch
    QString subject;
};

struct RefLabel {
    enum Kind { Head, Branch, Remote, Tag };
    QString name;
    Kind kind = Branch;
    bool current = false;   // the branch HEAD is on
};

// A path git could not merge, with the blob at each index stage it has:
// 1 = common ancestor, 2 = HEAD's side, 3 = the side being brought in.
struct UnmergedFile {
    QString path;
    QString stage[4];   // [1]..[3]; empty when that side has no such file
    bool has(int s) const { return !stage[s].isEmpty(); }
};

struct GitResult {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    bool ok() const { return exitCode == 0; }
};

// Thin wrapper around the git CLI. Using the real git binary (rather than
// libgit2) means hooks, GPG/SSH signing, credential helpers and every config
// option behave exactly as they do in the terminal.
class GitRepo {
public:
    static QString findRoot(const QString &startDir);

    explicit GitRepo(const QString &root);
    QString root() const { return m_root; }

    void configure(QProcess &proc, const QString &indexFile = QString()) const;
    GitResult run(const QStringList &args, const QByteArray &stdinData = {}, int timeoutMs = 30000,
                  const QString &indexFile = QString()) const;

    QString branch() const;          // empty when detached
    QString upstream() const;        // empty when none
    QStringList remotes() const;
    bool hasHead() const;
    bool isMerging() const;
    QString lastCommitMessage() const;
    bool branchExists(const QString &name) const;
    bool isValidBranchName(const QString &name) const;
    GitResult createBranch(const QString &name) const;

    QVector<FileEntry> status() const;
    QVector<FileEntry> lastCommitFiles() const;
    QVector<FileEntry> changedFiles(const QString &from, const QString &to) const;
    QByteArray diffBetween(const QString &from, const QString &to, const FileEntry &f) const;

    // History, newest first in topological order, `count` at a time.
    QVector<LogCommit> log(int skip, int count, bool allRefs) const;
    QHash<QString, QVector<RefLabel>> refsByCommit() const;
    QString commitMessage(const QString &hash) const;
    QString emptyTree() const;

    // Conflicts.
    QString gitDir() const;
    QString operation() const;   // "merge", "rebase", "cherry-pick", "revert" or empty
    QVector<UnmergedFile> unmerged() const;
    QString headParent() const;
    QString scratchIndexPath() const;
    GitResult prepareAmendIndex(const QString &indexFile, const QStringList &include,
                                const QStringList &exclude) const;
    void refreshIndexAfterAmend(const QStringList &paths) const;
    QByteArray diff(const FileEntry &f, const QString &base = QString()) const;
    QByteArray diffFiles(const QString &a, const QString &b) const;
    QStringList commitArgs(const QStringList &paths, bool amend, bool merging) const;

private:
    QString m_root;
};
