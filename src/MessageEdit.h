#pragma once

#include <QColor>
#include <QPlainTextEdit>

class MessageHighlighter;

// Commit message editor with a 50/72 guide: the summary turns yellow past 50
// characters, red past 72, and a dashed ruler marks column 72.
class MessageEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit MessageEdit(QWidget *parent = nullptr);
    void applyTheme();

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    MessageHighlighter *m_hl;
    QColor m_guide;
};
