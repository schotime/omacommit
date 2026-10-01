#pragma once

#include <QString>
#include <QVector>

class QWidget;

// A command-palette list: a search box over the choices, near the top of the
// window. Typing narrows the list (every word must appear in the name or its
// detail), Up/Down move, Enter picks, Esc or a click outside cancels.
struct PickerItem {
    QString text;
    QString detail;        // shown muted, on the right
    bool always = false;   // stays listed whatever is typed (e.g. "Open Repository…")
};

namespace Picker {
// The index into `items` of what was picked, or -1.
int choose(QWidget *window, const QString &placeholder, const QVector<PickerItem> &items);
}
