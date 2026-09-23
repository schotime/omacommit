#pragma once

#include <QByteArray>
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

    bool untracked() const { return index == u'?'; }
    bool worktreeChange() const { return index != u' ' || worktree != u' '; }
    QString statusText() const;
    QString worktreeStatusText() const;
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
    QString headParent() const;
    QString scratchIndexPath() const;
    GitResult prepareAmendIndex(const QString &indexFile, const QStringList &include,
                                const QStringList &exclude) const;
    void refreshIndexAfterAmend(const QStringList &paths) const;
    QByteArray diff(const FileEntry &f) const;
    QByteArray diffFiles(const QString &a, const QString &b) const;
    QStringList commitArgs(const QStringList &paths, bool amend, bool merging) const;

private:
    QString emptyTree() const;
    QString m_root;
};
