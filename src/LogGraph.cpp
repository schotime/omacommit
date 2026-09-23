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
    // this commit, which converge on its dot.
    for (int i = 0; i < m_lanes.size(); ++i)
        if (!m_lanes.at(i).isEmpty())
            row.top.push_back({i, m_lanes.at(i) == hash ? col : i});
    for (QString &lane : m_lanes)
        if (lane == hash)
            lane.clear();

    // Bottom half: parents. One that some lane is already heading for is
    // joined to that lane rather than given a parallel one; otherwise the
    // first parent carries on in this column and the rest branch off.
    QVector<bool> fromHere(m_lanes.size(), false);
    for (int p = 0; p < parents.size(); ++p) {
        const QString &parent = parents.at(p);
        int lane = m_lanes.indexOf(parent);
        if (lane < 0) {
            lane = (p == 0 && m_lanes.at(col).isEmpty()) ? col : freeLane();
            m_lanes[lane] = parent;
            fromHere.resize(m_lanes.size());
        }
        fromHere[lane] = true;
        row.bottom.push_back({col, lane});
    }
    for (int i = 0; i < m_lanes.size(); ++i)
        if (!m_lanes.at(i).isEmpty() && !fromHere.at(i))
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
