#include "Syntax.h"
#include "Theme.h"

#include <QFileInfo>

#ifdef OG_SYNTAX
#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>

namespace {

using Style = KSyntaxHighlighting::Theme::TextStyle;

// The theme's colours by kind of token; Normal, Variable, Operator and the
// like stay in the text colour.
bool styleFor(Style s, QTextCharFormat &f)
{
    const ThemeColors &c = Theme::instance().colors();
    QColor col;
    switch (s) {
    case Style::Keyword:
    case Style::ControlFlow:
    case Style::Import:
    case Style::Preprocessor:
        col = c.magenta;
        break;
    case Style::Function:
    case Style::Attribute:
        col = c.blue;
        break;
    case Style::DataType:
    case Style::BuiltIn:
    case Style::Extension:
        col = c.cyan;
        break;
    case Style::String:
    case Style::VerbatimString:
    case Style::SpecialString:
    case Style::Char:
        col = c.green;
        break;
    case Style::SpecialChar:
    case Style::DecVal:
    case Style::BaseN:
    case Style::Float:
    case Style::Constant:
        col = c.orange;
        break;
    case Style::Annotation:
        col = c.yellow;
        break;
    case Style::Comment:
    case Style::Documentation:
    case Style::CommentVar:
    case Style::RegionMarker:
        col = c.muted;
        f.setFontItalic(true);
        break;
    case Style::Alert:
    case Style::Error:
    case Style::Warning:
        col = c.red;
        break;
    default:
        return false;
    }
    // A colour that is too close to the background (themes reuse slots) would
    // hide the text: keep the text colour then.
    if (Theme::distance(col, c.background) < 25)
        col = c.foreground;
    f.setForeground(col);
    return true;
}

class Collector : public KSyntaxHighlighting::AbstractHighlighter {
public:
    Syntax::Spans *out = nullptr;
    KSyntaxHighlighting::State line(const QString &text, const KSyntaxHighlighting::State &state)
    {
        return highlightLine(text, state);
    }

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override
    {
        QTextCharFormat f;
        if (length > 0 && format.isValid() && styleFor(format.textStyle(), f))
            out->append({offset, length, f});
    }
};

KSyntaxHighlighting::Repository &repository()
{
    static KSyntaxHighlighting::Repository r;   // loads the definitions once
    return r;
}

} // namespace

QVector<Syntax::Spans> Syntax::highlight(const QString &fileName, const QStringList &lines)
{
    const KSyntaxHighlighting::Definition def = repository().definitionForFileName(QFileInfo(fileName).fileName());
    if (!def.isValid() || lines.isEmpty())
        return {};
    Collector c;
    c.setDefinition(def);
    c.setTheme(repository().defaultTheme());
    QVector<Spans> out;
    out.reserve(lines.size());
    KSyntaxHighlighting::State state;
    for (const QString &line : lines) {
        Spans spans;
        c.out = &spans;
        state = c.line(line, state);
        out << spans;
    }
    return out;
}

#else

QVector<Syntax::Spans> Syntax::highlight(const QString &, const QStringList &)
{
    return {};
}

#endif
