#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// How one row of the history graph is drawn. Lanes are columns; a segment
// joins a lane at one edge of the row to a lane at the row's middle (top) or
// from the middle to the bottom edge (bottom), so rows stack into lines.
struct GraphRow {
    struct Seg {
        int from, to;
    };
    int column = 0;       // where this commit's dot goes
    int width = 1;        // lanes this row needs
    bool merge = false;
    QVector<Seg> top;     // top edge -> middle
    QVector<Seg> bottom;  // middle -> bottom edge
};

// Assigns lanes to commits fed newest first in topological order. Keeps its
// state between calls, so history can be laid out a batch at a time.
class GraphLayout {
public:
    GraphRow add(const QString &hash, const QStringList &parents);
    void reset() { m_lanes.clear(); }

private:
    int freeLane();
    QVector<QString> m_lanes;   // the commit each lane is heading for; empty = free
};
