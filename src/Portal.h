#pragma once

#include <QString>

// The desktop's own file chooser, through the XDG portal -- whatever the user
// set as FileChooser in portals.conf (on Omarchy, Strata). Qt's GTK theme
// never asks the portal; it draws GTK's chooser in-process instead.
namespace Portal {

enum class Result { Picked, Cancelled, Unavailable };

// Blocks until the chooser closes. Unavailable when there is no portal with a
// FileChooser, so the caller can fall back to QFileDialog.
Result pickDirectory(const QString &title, const QString &startDir, QString *picked);

}
