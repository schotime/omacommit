#include "GitRepo.h"
#include "GitTask.h"
#include "DiffView.h"
#include "Theme.h"

#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <iostream>
#include <stdexcept>

void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}

void write(const QString &root, const QString &path, const QByteArray &data)
{
    QFile file(root + u'/' + path);
    require(file.open(QIODevice::WriteOnly), "open test file");
    require(file.write(data) == data.size(), "write test file");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    try {
        QTemporaryDir tmp;
        require(tmp.isValid(), "temporary repository");
        GitRepo repo(tmp.path());
        require(repo.run({"init"}).ok(), "init");
        require(repo.run({"config", "user.name", "Test"}).ok(), "user name");
        require(repo.run({"config", "user.email", "test@example.invalid"}).ok(), "user email");
        require(repo.run({"config", "commit.gpgsign", "false"}).ok(), "disable signing");
        require(repo.run({"config", "core.autocrlf", "false"}).ok(), "disable autocrlf");
        write(tmp.path(), "a.txt", "first\n");
        write(tmp.path(), "b.txt", "other\n");
        require(repo.run({"add", "a.txt", "b.txt"}).ok(), "stage initial");
        auto initial = repo.commitSnapshot(false);
        require(initial.entries.size() == 2 && !initial.base.isEmpty(), "unborn snapshot");
        auto committed = repo.commitSelection("Initial", false, {}, {"a.txt", "b.txt"}, {});
        require(committed.result.ok(), "root commit");
        require(repo.status().isEmpty(), "clean after root commit");
        auto amend = repo.commitSnapshot(true);
        require(amend.entries.size() == 2 && amend.lastMessage == "Initial", "root amend snapshot");

        write(tmp.path(), "a.txt", "second\n");
        write(tmp.path(), "b.txt", "pending\n");
        require(repo.run({"add", "b.txt"}).ok(), "stage unrelated change");
        committed = repo.commitSelection("Change A", false, "test-branch", {}, {"a.txt"});
        require(committed.result.ok() && repo.branch() == "test-branch", "new branch commit");
        require(repo.indexBlob("b.txt") == "pending\n", "unticked staged content survives");
        require(repo.stagedFiles("HEAD").size() == 1, "unrelated change remains staged");
        require(repo.run({"show", "HEAD:b.txt"}).out == "other\n", "unticked change not committed");
        auto rejected = repo.replaceIndexContent("b.txt", "stale\n", "corrupt\n");
        require(!rejected.ok() && repo.indexBlob("b.txt") == "pending\n", "reject stale line staging");
        require(repo.replaceIndexContent("b.txt", "pending\n", "partial\n").ok(), "checked index replacement");
        require(repo.unstageFiles({"b.txt"}).ok(), "unstage");
        require(repo.indexBlob("b.txt") == "other\n", "unstage restores HEAD");
        const QString sha = QString::fromUtf8(repo.run({"rev-parse", "HEAD"}).out);
        committed = repo.commitSelection("Reject", false, "bad..branch", {}, {"b.txt"});
        require(!committed.result.ok() && QString::fromUtf8(repo.run({"rev-parse", "HEAD"}).out) == sha,
                "invalid branch does not commit");
        write(tmp.path(), "new.txt", "untracked\n");
        require(repo.selectedChanges("HEAD", {}, {}, {"new.txt"}).contains("+untracked"), "Windows untracked prompt diff");
        require(repo.diffContents("old\n", "new\n").contains("+new"), "buffer diff");
        committed = repo.commitSelection("Amended A", true, {}, {"a.txt"}, {});
        require(committed.result.ok(), "amend selected file");
        require(repo.lastCommitMessage() == "Amended A", "amend message");
        require(repo.run({"show", "HEAD:b.txt"}).out == "other\n", "amend preserves parent content for unticked files");
        require(!QFile::exists(repo.scratchIndexPath()), "scratch index cleaned after success");
        const QString hooks = tmp.path() + "/.git/hooks";
        write(hooks, "pre-commit", "#!/bin/sh\nexit 1\n");
        committed = repo.commitSelection("Hook failure", false, {}, {}, {"b.txt"});
        require(!committed.result.ok(), "hook failure surfaced");
        require(!QFile::exists(repo.scratchIndexPath()), "scratch index cleaned after failure");
        QFile::remove(hooks + "/pre-commit");

        QWidget window;
        QWidget owner(&window);
        QEventLoop loop;
        int ticks = 0;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
        heartbeat.start(10);
        bool finished = false, correctThread = false;
        GitTask::run(&owner, [] { QThread::msleep(180); return 42; }, [&](int result) {
            finished = result == 42;
            correctThread = QThread::currentThread() == app.thread();
            loop.quit();
        });
        require(!window.isEnabled() && window.property("gitTaskBusy").toBool(), "exclusive task locks window");
        bool overlapping = false;
        GitTask::run(&owner, [&] { overlapping = true; return 0; }, [](int) {});
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
        require(finished && correctThread && ticks >= 5, "worker leaves event loop responsive");
        require(window.isEnabled() && !window.property("gitTaskBusy").toBool(), "task unlocks window");
        require(!overlapping, "exclusive task rejects overlapping writes");

        bool called = false;
        auto *gone = new QWidget;
        GitTask::run(gone, [] { QThread::msleep(80); return 0; }, [&](int) { called = true; }, false);
        delete gone;
        QTimer::singleShot(180, &loop, &QEventLoop::quit);
        loop.exec();
        require(!called, "destroyed owner drops completion");

        Theme::instance().load();
        DiffView view;
        view.onSave = [](const QStringList &) { return true; };
        QVector<std::function<void(const QByteArray &)>> replies;
        view.rediffAsync = [&](const QStringList &, auto done) { replies << done; };
        view.showDiff("a.txt", "diff --git a/a.txt b/a.txt\n--- a/a.txt\n+++ b/a.txt\n@@ -1 +1 @@\n-old\n+new\n", true);
        view.leftFile = [] { return QByteArray("old\n"); };
        view.take(DiffView::Take::WholeFile, 0);
        require(view.editedLines() == QStringList{"old"} && view.isDirty(), "async edit buffer updates immediately");
        view.undo();
        require(view.editedLines() == QStringList{"new"}, "undo before rediff completes");
        require(replies.size() == 2, "async rediff scheduled");
        replies[0]("stale");
        require(view.editedLines() == QStringList{"new"}, "stale rediff cannot overwrite undo");
        view.showMessage({}, "other file");
        replies[1]("stale");
        require(view.rowCount() == 0, "file change invalidates edit rediff");
        std::cout << "Git and background task regressions passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
