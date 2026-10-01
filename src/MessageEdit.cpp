#include "MessageEdit.h"
#include "Theme.h"

#include <QPainter>
#include <QSyntaxHighlighter>
#include <QTextBlock>

class MessageHighlighter : public QSyntaxHighlighter {
public:
    using QSyntaxHighlighter::QSyntaxHighlighter;
    QColor warn, error;

protected:
    void highlightBlock(const QString &text) override
    {
        const int n = currentBlock().blockNumber();
        // Distinct state per line index so inserting lines re-highlights below.
        setCurrentBlockState(qMin(n, 2));

        QTextCharFormat warnFmt;
        warnFmt.setForeground(warn);
        QTextCharFormat errFmt;
        errFmt.setForeground(error);
        errFmt.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        errFmt.setUnderlineColor(error);

        constexpr int summary = MessageEdit::SummaryColumn, wrap = MessageEdit::GuideColumn;
        if (n == 0) {
            if (text.size() > summary)
                setFormat(summary, qMin<int>(text.size(), wrap) - summary, warnFmt);
            if (text.size() > wrap)
                setFormat(wrap, text.size() - wrap, errFmt);
        } else if (n == 1) {
            if (!text.trimmed().isEmpty()) {   // line 2 should be blank
                QTextCharFormat f;
                f.setUnderlineStyle(QTextCharFormat::WaveUnderline);
                f.setUnderlineColor(error);
                setFormat(0, text.size(), f);
            }
        } else if (text.size() > wrap) {
            setFormat(wrap, text.size() - wrap, warnFmt);
        }
    }
};

MessageEdit::MessageEdit(QWidget *parent) : QPlainTextEdit(parent)
{
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabChangesFocus(true);
    setPlaceholderText(tr("Summary on the first line, a blank line, then the details"));
    m_hl = new MessageHighlighter(document());
    applyTheme();
}

void MessageEdit::applyTheme()
{
    const ThemeColors &c = Theme::instance().colors();
    m_hl->warn = c.yellow;
    m_hl->error = c.red;
    m_guide = c.border;
    m_hl->rehighlight();
    viewport()->update();
}

void MessageEdit::paintEvent(QPaintEvent *e)
{
    QPlainTextEdit::paintEvent(e);
    QPainter p(viewport());
    const qreal x = contentOffset().x() + document()->documentMargin()
        + fontMetrics().horizontalAdvance(QString(GuideColumn, QLatin1Char('m')));
    p.setPen(QPen(m_guide, 1, Qt::DashLine));
    p.drawLine(QPointF(x, 0), QPointF(x, viewport()->height()));
}
