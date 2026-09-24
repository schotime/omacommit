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
    QChar commitStatus = u' ';   // in a commit's file list: what it did to it (M, A, D, R, ...)
    // The commit window lists staged changes (the index against the commit's
    // base: index set, worktree ' ') apart from unstaged ones (the working
    // tree against the index: index ' ', or '?' when untracked, or both set
    // for a conflict).
    bool staged = false;

    bool untracked() const { return index == u'?'; }
    bool conflicted() const;
    QString statusText() const;
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

// Git's whitespace rules for one path: its `whitespace` attribute, else
// core.whitespace. Empty rules mean git's defaults; check = false means the
// attribute turns checking off for the path.
struct WhitespaceRules {
    bool check = true;
    QString rules;
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

    // Diffs ignore whitespace changes (-w) while this is on. Display only:
    // nothing about what gets committed depends on it.
    static void setIgnoreWhitespace(bool on) { s_ignoreWhitespace = on; }
    static bool ignoreWhitespace() { return s_ignoreWhitespace; }

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
    // The commit window's two lists. `base` is what the commit builds on:
    // HEAD, or its parent when amending (so the last commit's changes count as
    // staged), or the empty tree before the first commit.
    QVector<FileEntry> stagedFiles(const QString &base) const;
    QVector<FileEntry> unstagedFiles() const;
    QByteArray diffStaged(const FileEntry &f, const QString &base) const;     // base -> index
    QByteArray diffUnstaged(const FileEntry &f) const;                        // index -> working tree
    // Everything not committed yet -- staged, unstaged and untracked (status
    // '?') -- against `base`, and one file of it: the log's "Working changes".
    QVector<FileEntry> workingChanges(const QString &base) const;
    QByteArray diffWorking(const FileEntry &f, const QString &base) const;
    QString commitBase(bool amend) const;
    // The index's copy of a file; `exists` false when the index has none.
    QByteArray indexBlob(const QString &path, bool *exists = nullptr) const;
    // Puts `data` in the index as `path` -- staging or unstaging part of it.
    GitResult setIndexContent(const QString &path, const QByteArray &data) const;
    // A scratch index for the commit: `base`, plus the index's version of
    // `fromIndex` and the working tree's of `fromWorktree`.
    GitResult prepareCommitIndex(const QString &indexFile, const QString &base, const QStringList &fromIndex,
                                 const QStringList &fromWorktree) const;
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

    // Whitespace problems on the lines newText adds relative to oldText, as
    // `git diff --check` reports them, keyed by 1-based line in newText.
    WhitespaceRules whitespaceRules(const QString &path) const;
    QHash<int, QString> whitespaceIssues(const WhitespaceRules &rules, const QByteArray &oldText,
                                         const QByteArray &newText) const;
    QString headParent() const;
    QString scratchIndexPath() const;
    QByteArray diffFiles(const QString &a, const QString &b) const;

private:
    QStringList fullDiffArgs() const;
    static QVector<FileEntry> parseNameStatus(const QByteArray &out);

    static inline bool s_ignoreWhitespace = false;
    QString m_root;
};
