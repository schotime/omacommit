#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Coding agents available for writing commit messages. Prefer Omarchy's
// default on Linux, but also discover installed agents on PATH.
struct Agent {
    QString id;       // "claude", "codex", ... ; empty when none is set
    QString name;     // "Claude Code"
    bool supported = false;   // og knows how to run it one-shot
    bool installed = false;   // really installed, not Omarchy's install-on-first-run stub
    QString executable;       // resolved path, including .cmd on Windows

    static Agent detect();
    static QVector<Agent> available();
    bool usable() const { return supported && installed; }

    QString program() const { return executable; }
    // Arguments for a one-shot run with the prompt on stdin. When the reply
    // is written to a file rather than stdout, `replyFile` is that file.
    QStringList arguments(const QString &replyFile) const;
    bool replyInFile() const { return id == u"codex"; }
};
