#include "LogGraph.h"

int GraphLayout::freeLane()
{
    const int i = m_lanes.indexOf(QString());
    if (i >= 0)
        return i;
    m_lanes.push_back(QString());
    return int(m_lanes.size()) - 1;
}

GraphRow GraphLayout::add(const QString &hash, const QStringList &parents)
{
    GraphRow row;
    row.merge = parents.size() > 1;

    // A lane already heading here, or -- for a branch tip -- a fresh one.
    int col = m_lanes.indexOf(hash);
    if (col < 0)
        col = freeLane();
    row.column = col;

    // Top half: everything passes straight through, except lanes heading for
    // this commit, which converge on its dot. The ones passing through carry
    // on through the bottom half too.
    QVector<bool> passing(m_lanes.size(), false);
    for (int i = 0; i < m_lanes.size(); ++i)
        if (!m_lanes.at(i).isEmpty()) {
            passing[i] = m_lanes.at(i) != hash;
            row.top.push_back({i, passing.at(i) ? i : col});
        }
    for (QString &lane : m_lanes)
        if (lane == hash)
            lane.clear();

    // Bottom half: parents. The first parent always carries on straight down
    // this column, so a branch's own history reads as one line -- even when
    // another lane is heading for the same commit; the two meet at its dot.
    // Further parents (the branches a merge brought in) join a lane already
    // heading for them, or branch off into a new one.
    QVector<bool> fromHere(m_lanes.size(), false);
    for (int p = 0; p < parents.size(); ++p) {
        const QString &parent = parents.at(p);
        int lane = p == 0 ? col : m_lanes.indexOf(parent);
        if (lane < 0 || (p > 0 && fromHere.value(lane))) {
            lane = freeLane();
            fromHere.resize(m_lanes.size());
        }
        m_lanes[lane] = parent;
        fromHere[lane] = true;
        row.bottom.push_back({col, lane});
    }
    // A lane a parent joined still continues its own line: the join is drawn
    // on top of it, not instead of it, or that line would stop mid-row.
    for (int i = 0; i < passing.size(); ++i)
        if (passing.at(i))
            row.bottom.push_back({i, i});

    while (!m_lanes.isEmpty() && m_lanes.constLast().isEmpty())
        m_lanes.removeLast();

    row.width = col + 1;
    for (const auto &s : row.top)
        row.width = qMax(row.width, qMax(s.from, s.to) + 1);
    for (const auto &s : row.bottom)
        row.width = qMax(row.width, qMax(s.from, s.to) + 1);
    return row;
}
