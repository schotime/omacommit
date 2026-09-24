#pragma once

#include <QStringList>
#include <QTextLayout>
#include <QVector>

// Code in colour: KSyntaxHighlighting picks the language from the file name
// and splits each line into keywords, strings, comments and so on, which get
// the Omarchy theme's colours. Built without the library (OG_SYNTAX unset),
// everything stays plain text.
namespace Syntax {

using Spans = QVector<QTextLayout::FormatRange>;

// The colours for each of `lines` -- a whole file, in order, so strings and
// comments running over several lines come out right. Empty when the
// language isn't known.
QVector<Spans> highlight(const QString &fileName, const QStringList &lines);

}
