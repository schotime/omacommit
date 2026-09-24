#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QFont>
#include <QObject>
#include <QPalette>
#include <QTimer>
#include <QVector>

struct ThemeColors {
    QColor background, foreground, accent;
    QColor selectionBg, selectionFg;
    QColor muted, border, surface, hover;
    QColor red, green, yellow, blue;
    QColor magenta, cyan, orange;     // for syntax highlighting
    // Picked from the theme's palette so they stay apart from each other, even
    // in themes whose named colours are close (accent == blue is common).
    QColor other;                     // the side that isn't yours, when resolving
    QColor refBranch, refRemote, refTag;
    QVector<QColor> lanes;            // log graph lines
    bool light = false;
};

// Reads the active Omarchy theme (colors.toml), turns it into a Qt palette +
// stylesheet, and live-reloads when you switch themes.
class Theme : public QObject {
    Q_OBJECT
public:
    static Theme &instance();

    void load();
    void apply();

    const ThemeColors &colors() const { return m_c; }
    QFont font() const { return m_font; }
    QString sourcePath() const { return m_path; }

    static QColor mix(const QColor &a, const QColor &b, qreal t);
    static double distance(const QColor &a, const QColor &b);   // perceptual (CIE76 ΔE)
    // A name emphasised inside a label's rich text (a branch in a heading).
    // Stylesheet classes don't reach into rich text, so the style lives here.
    static QString strong(const QString &plain);

signals:
    void changed();

private:
    Theme();
    void watch();
    void loadFont();
    QPalette palette() const;
    QString styleSheet() const;

    ThemeColors m_c;
    QFont m_font;
    QString m_path;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    bool m_fontLoaded = false;
};
