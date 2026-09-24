#pragma once

#include "GitRepo.h"

#include <QSet>
#include <QTemporaryDir>
#include <QWidget>

#include <memory>

class ImageCompare;
class DiffPane;
class DiffView;
class QLabel;
class QProcess;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QTreeWidget;

// Three-way conflict resolution, TortoiseGitMerge style: the two sides on top
// (read-only, aligned), the merged file below (editable, what gets saved), and
// the conflicted files on the left.
class ResolveWindow : public QWidget {
    Q_OBJECT
public:
    explicit ResolveWindow(const QString &root, const QString &selectPath = QString(), QWidget *parent = nullptr);
    void select(const QString &path);   // open this conflicted file

protected:
    void closeEvent(QCloseEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    bool m_shownBefore = false;
    bool m_highlighting = false;
    // One unresolved conflict in the merged text, by line index of its markers.
    struct Conflict {
        int start = -1, base = -1, sep = -1, end = -1;
    };
    enum class Pick { Current, Incoming, CurrentThenIncoming, IncomingThenCurrent };

    void describeSides();
    void refreshList(const QString &select = QString());
    void openSelected();
    void openText(const UnmergedFile &u);
    void openWholeFile(const UnmergedFile &u);
    void showSides();
    bool resolveUnsaved(bool allowCancel = true);

    QStringList mergedLines() const;
    void parseMerged();
    int targetConflict() const;
    // `revealAbove`: scroll the panes above to it (not when it was clicked there).
    void gotoConflict(int k, bool revealAbove = true);
    void pick(Pick how, int k = -1);   // k: which conflict; the target one by default
    // The file's conflicts as it was opened, framed and numbered in the panes
    // above and followed as they are resolved.
    void buildOriginals();
    void placeOriginals();
    void matchOriginals();
    void updateMarks();
    int originalAtRow(int row) const;
    void extendSidesMenu(QMenu *menu, QAction *before, int row, bool right);
    void useWholeSide(bool incoming);
    QStringList section(const Conflict &c, bool incoming) const;

    bool save();
    void markResolved();
    void finishWith(const QStringList &gitArgs, const QStringList &thenArgs = {});
    void commit();
    void continueRebase();
    void updateOpenFileState();
    static QString bothState(bool hasBase, int left);
    void selectNextUnresolved();
    void updateActions();
    void applyTheme();

    GitRepo m_repo;
    QString m_operation;
    // What each side is, in words -- "mine" is stage 3 during a rebase, so the
    // UI never says ours/theirs, only these.
    QString m_curShort, m_curLong, m_incShort, m_incLong, m_hint, m_opText;
    bool m_canCommit = false;
    // Your own work is always shown on the right, as in every other diff here.
    // That is git's HEAD side, except in a rebase, where it is the incoming one.
    bool m_mineIsIncoming = false;

    QLabel *m_subtitle;
    QTreeWidget *m_files;
    QLabel *m_hintLabel;
    QLabel *m_status;
    QPushButton *m_commitBtn;
    QPushButton *m_continueBtn;
    QProcess *m_continuing = nullptr;
    QStackedWidget *m_stack = nullptr;
    DiffView *m_top;
    DiffPane *m_merged;
    QLabel *m_mergedTitle;
    QLabel *m_counter;
    QLabel *m_pickFor;
    QToolButton *m_prevBtn;
    QToolButton *m_nextBtn;
    QPushButton *m_useInc;
    QPushButton *m_useCur;
    QPushButton *m_useIncCur;
    QPushButton *m_useCurInc;
    QToolButton *m_wholeBtn;
    QPushButton *m_saveBtn;
    QPushButton *m_resolvedBtn;
    QLabel *m_wholeText;
    ImageCompare *m_wholeImages;   // the two sides, when the file is an image
    QPushButton *m_wholeA;
    QPushButton *m_wholeB;
    QLabel *m_message;

    QString m_path;              // the file open on the right
    QByteArray m_loaded;         // its bytes as opened, to spot outside changes
    QStringList m_curLines, m_incLines;
    bool m_openHasBase = true;   // the open file has a common ancestor (both modified, not both added)
    QString m_incPath, m_curPath;   // the two sides, written out for git diff
    QVector<Conflict> m_conflicts;
    struct Original {
        QStringList cur, inc;             // its two sections
        int firstRow = -1, lastRow = -1;  // where it is in the panes above
    };
    QVector<Original> m_originals;
    QVector<int> m_originalOf;   // for each conflict left, which original it is
    int m_current = -1;
    QSet<QString> m_resolved;    // marked resolved this session: kept in the list, ticked
    QStringList m_listed;
    std::unique_ptr<QTemporaryDir> m_tmp;
};
