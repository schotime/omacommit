#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QFont>
#include <QObject>
#include <QPalette>
#include <QTimer>

struct ThemeColors {
    QColor background, foreground, accent;
    QColor selectionBg, selectionFg;
    QColor muted, border, surface, hover;
    QColor red, green, yellow, blue;
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
