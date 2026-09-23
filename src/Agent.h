#pragma once

#include <QString>
#include <QStringList>

// The coding agent Omarchy is set to use (omarchy-default-agent), for writing
// commit messages. Omarchy only launches agents interactively, so og runs the
// agent's own one-shot mode itself: read the prompt on stdin, print the reply,
// exit -- with no tools, and nothing it could change.
struct Agent {
    QString id;       // "claude", "codex", ... ; empty when none is set
    QString name;     // "Claude Code"
    bool supported = false;   // og knows how to run it one-shot
    bool installed = false;   // really installed, not Omarchy's install-on-first-run stub

    static Agent detect();
    bool usable() const { return supported && installed; }

    QString program() const { return id; }
    // Arguments for a one-shot run with the prompt on stdin. When the reply
    // is written to a file rather than stdout, `replyFile` is that file.
    QStringList arguments(const QString &replyFile) const;
    bool replyInFile() const { return id == u"codex"; }
};
