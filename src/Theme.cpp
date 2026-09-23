#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <array>
#include <cmath>

namespace {
QColor hex(const char *s) { return QColor(QString::fromLatin1(s)); }

// Where omarchy-theme-set puts the active theme; the ~/.config location is
// where older Omarchy releases kept it.
QStringList colorFileCandidates()
{
    const QString home = QDir::homePath();
    return {home + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml"),
            home + QStringLiteral("/.config/omarchy/current/theme/colors.toml")};
}
} // namespace

Theme &Theme::instance()
{
    static Theme t;
    return t;
}

Theme::Theme()
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(150);   // theme switches touch several files; react once
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        load();
        apply();
        emit changed();
    });
}

double Theme::distance(const QColor &a, const QColor &b)
{
    // sRGB -> CIE Lab (D65), then Euclidean distance: roughly, 10 is a subtle
    // difference, 30+ reads as a different colour.
    auto lab = [](const QColor &c) {
        auto lin = [](double v) { return v > 0.04045 ? std::pow((v + 0.055) / 1.055, 2.4) : v / 12.92; };
        const double r = lin(c.redF()), g = lin(c.greenF()), b = lin(c.blueF());
        const double x = (r * 0.4124 + g * 0.3576 + b * 0.1805) / 0.95047;
        const double y = r * 0.2126 + g * 0.7152 + b * 0.0722;
        const double z = (r * 0.0193 + g * 0.1192 + b * 0.9505) / 1.08883;
        auto f = [](double v) { return v > 0.008856 ? std::cbrt(v) : 7.787 * v + 16.0 / 116.0; };
        return std::array<double, 3>{116 * f(y) - 16, 500 * (f(x) - f(y)), 200 * (f(y) - f(z))};
    };
    const auto p = lab(a), q = lab(b);
    return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
}

QString Theme::strong(const QString &plain)
{
    return QStringLiteral("<span style=\"font-weight:600\">%1</span>").arg(plain.toHtmlEscaped());
}

QColor Theme::mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t),
                            float(a.greenF() * (1 - t) + b.greenF() * t),
                            float(a.blueF() * (1 - t) + b.blueF() * t));
}

void Theme::load()
{
    m_path.clear();
    for (const QString &c : colorFileCandidates()) {
        if (QFileInfo::exists(c)) {
            m_path = c;
            break;
        }
    }

    QHash<QString, QColor> v;
    QString mode;
    if (!m_path.isEmpty()) {
        QFile f(m_path);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            static const QRegularExpression re(QStringLiteral(R"(^\s*([A-Za-z0-9_]+)\s*=\s*["']?(#[0-9A-Fa-f]{6})["']?)"));
            static const QRegularExpression modeRe(QStringLiteral(R"(^\s*mode\s*=\s*["']?(\w+))"));
            const QStringList lines = QString::fromUtf8(f.readAll()).split(u'\n');
            for (const QString &line : lines) {
                if (const auto m = re.match(line); m.hasMatch())
                    v.insert(m.captured(1).toLower(), QColor(m.captured(2)));
                else if (const auto mm = modeRe.match(line); mm.hasMatch())
                    mode = mm.captured(1).toLower();
            }
        }
    }
    // Omarchy names its colours (red, green, ...). Older themes used terminal
    // slots (color1, color2, ...), which are still accepted.
    auto get = [&](std::initializer_list<const char *> keys, const QColor &fallback) {
        for (const char *key : keys)
            if (const auto it = v.constFind(QString::fromLatin1(key)); it != v.constEnd())
                return it.value();
        return fallback;
    };

    // Fallbacks are Tokyo Night, Omarchy's default theme.
    ThemeColors c;
    c.background = get({"background"}, hex("#1a1b26"));
    c.foreground = get({"foreground"}, hex("#a9b1d6"));
    c.red = get({"red", "color1"}, hex("#f7768e"));
    c.green = get({"green", "color2"}, hex("#9ece6a"));
    c.yellow = get({"yellow", "color3"}, hex("#e0af68"));
    c.blue = get({"blue", "color4"}, hex("#7aa2f7"));
    c.accent = get({"accent"}, c.blue);

    const bool lightFile = !m_path.isEmpty()
        && QFileInfo::exists(QFileInfo(m_path).absolutePath() + QStringLiteral("/light.mode"));
    c.light = mode.isEmpty() ? (lightFile || c.background.lightnessF() > 0.5) : mode == u"light";

    // --- colours that have to be told apart
    // Candidates are the theme's own colours, by name, in order of preference;
    // a pick must be clearly apart (ΔE) from the colours it sits next to and
    // readable on the background. Short of that, the furthest candidate wins.
    auto named = [&](std::initializer_list<const char *> keys) {
        QVector<QColor> out;
        for (const char *k : keys)
            if (const auto it = v.constFind(QString::fromLatin1(k)); it != v.constEnd())
                out << it.value();
        return out;
    };
    const QVector<QColor> palette = named({"accent", "green", "yellow", "blue", "red", "magenta", "cyan", "orange",
                                           "bright_green", "bright_yellow", "bright_blue", "bright_red",
                                           "bright_magenta", "bright_cyan"})
                                  + QVector<QColor>{c.accent, c.green, c.yellow, c.blue, c.red};
    auto visible = [&](const QColor &x) { return distance(x, c.background) >= 25; };
    auto pickApart = [&](QVector<QColor> order, const QVector<QColor> &avoid, double apart) {
        order += palette;
        auto apartFromAll = [&](const QColor &x) {
            bool ok = visible(x);
            for (const QColor &a : avoid)
                ok = ok && distance(x, a) >= apart;
            return ok;
        };
        for (const QColor &x : order)
            if (apartFromAll(x))
                return x;
        // Greyscale themes have no second hue: tell colours apart by lightness.
        const int named = int(order.size());
        for (int i = 0; i < named; ++i)
            order << mix(order.at(i), c.foreground, 0.55) << mix(order.at(i), c.background, 0.45);
        order << c.foreground << mix(c.foreground, c.background, 0.5);
        for (int i = named; i < order.size(); ++i)
            if (apartFromAll(order.at(i)))
                return order.at(i);
        QColor best = order.value(0, c.foreground);
        double bestGap = -1;
        for (const QColor &x : order) {
            double gap = 1e9;
            for (const QColor &a : avoid)
                gap = std::min(gap, distance(x, a));
            if (visible(x) && gap > bestGap) {
                best = x;
                bestGap = gap;
            }
        }
        return best;
    };

    // Diffs: added must not look like removed (monochrome themes: red == green).
    if (distance(c.green, c.red) < 30)
        c.green = pickApart(named({"green", "bright_green", "cyan", "bright_cyan", "blue", "accent"}), {c.red}, 30);
    // Resolving: the other side against your accent, and not the markers' red.
    c.other = pickApart(named({"yellow", "orange", "magenta", "cyan", "green", "bright_yellow", "bright_magenta",
                               "bright_cyan", "blue"}),
                        {c.accent, c.red}, 30);
    // Ref badges: current branch is the accent; the rest apart from it and each other.
    c.refBranch = pickApart(named({"green", "bright_green", "cyan", "yellow"}), {c.accent}, 25);
    c.refRemote = pickApart(named({"blue", "cyan", "magenta", "bright_blue"}), {c.accent, c.refBranch}, 25);
    c.refTag = pickApart(named({"yellow", "orange", "bright_yellow", "magenta"}), {c.accent, c.refBranch, c.refRemote}, 25);
    // Graph lanes: as many mutually distinct colours as the theme has, up to six.
    c.lanes = {c.accent};
    for (const QColor &x : palette) {
        if (c.lanes.size() == 6)
            break;
        bool ok = visible(x);
        for (const QColor &l : c.lanes)
            ok = ok && distance(x, l) >= 25;
        if (ok)
            c.lanes << x;
    }
    // Near-monochrome themes: add lighter and darker shades so lines still differ.
    for (int i = 0; c.lanes.size() < 3 && i < 8; ++i) {
        const QColor base = c.lanes.at(i % c.lanes.size());
        for (const QColor &x : {mix(base, c.foreground, 0.55), mix(base, c.background, 0.45)}) {
            bool ok = visible(x);
            for (const QColor &l : c.lanes)
                ok = ok && distance(x, l) >= 15;
            if (ok && c.lanes.size() < 3)
                c.lanes << x;
        }
    }

    c.selectionBg = get({"selection", "selection_background"}, mix(c.background, c.accent, 0.35));
    c.selectionFg = get({"selection_foreground"}, c.foreground);
    // Secondary text, borders and input fields stay derived from the theme's
    // background and foreground rather than its muted / dark_foreground: those
    // vary too much in contrast between themes to be readable as text in all
    // of them (White's dark_foreground is #c0c0c0 on #ffffff).
    c.muted = mix(c.background, c.foreground, 0.55);
    c.border = mix(c.background, c.foreground, 0.18);
    c.surface = mix(c.background, c.foreground, c.light ? 0.035 : 0.045);
    c.hover = mix(c.background, c.foreground, 0.09);
    m_c = c;

    if (!m_fontLoaded)
        loadFont();
    watch();
}

void Theme::loadFont()
{
    QString family = QStringLiteral("JetBrainsMono Nerd Font");
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("omarchy-font-current"));
    if (!tool.isEmpty()) {
        QProcess p;
        p.start(tool, {});
        if (p.waitForFinished(1500)) {
            const QString f = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
            if (!f.isEmpty())
                family = f;
        }
    }
    m_font = QFont(family);
    m_font.setPointSizeF(10.5);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    m_fontLoaded = true;
}

void Theme::watch()
{
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());

    // Omarchy swaps a symlink / regenerates files on theme change, so watch the
    // directories as well as the file itself.
    const QString home = QDir::homePath();
    QStringList paths{home + QStringLiteral("/.config/omarchy/current"),
                      home + QStringLiteral("/.config/omarchy/current/theme"),
                      home + QStringLiteral("/.local/state/omarchy/current"),
                      home + QStringLiteral("/.local/state/omarchy/current/theme")};
    if (!m_path.isEmpty())
        paths << m_path;
    for (const QString &p : paths)
        if (QFileInfo::exists(p))
            m_watcher.addPath(p);
}

void Theme::apply()
{
    qApp->setFont(m_font);
    qApp->setPalette(palette());
    qApp->setStyleSheet(styleSheet());
}

QPalette Theme::palette() const
{
    const ThemeColors &c = m_c;
    QPalette p;
    p.setColor(QPalette::Window, c.background);
    p.setColor(QPalette::WindowText, c.foreground);
    p.setColor(QPalette::Base, c.surface);
    p.setColor(QPalette::AlternateBase, c.hover);
    p.setColor(QPalette::Text, c.foreground);
    p.setColor(QPalette::Button, c.surface);
    p.setColor(QPalette::ButtonText, c.foreground);
    p.setColor(QPalette::BrightText, c.red);
    p.setColor(QPalette::Highlight, c.selectionBg);
    p.setColor(QPalette::HighlightedText, c.selectionFg);
    p.setColor(QPalette::ToolTipBase, c.surface);
    p.setColor(QPalette::ToolTipText, c.foreground);
    p.setColor(QPalette::PlaceholderText, c.muted);
    p.setColor(QPalette::Link, c.accent);
    p.setColor(QPalette::Light, c.hover);
    p.setColor(QPalette::Midlight, c.hover);
    p.setColor(QPalette::Mid, c.border);
    p.setColor(QPalette::Dark, c.border);
    p.setColor(QPalette::Shadow, c.background);
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, c.muted);
    return p;
}

QString Theme::styleSheet() const
{
    // Flat, square, accent-on-focus: matches Omarchy's Hyprland look.
    QString css = QStringLiteral(R"(
* { outline: none; }
QWidget { background: @bg@; color: @fg@; font-family: "@uiFamily@"; font-size: @uiSize@pt; }
QPlainTextEdit { font-family: "@codeFamily@"; font-size: @codeSize@pt; }
QLabel#title { font-size: @titleSize@pt; font-weight: 500; }
QLabel#muted { color: @muted@; }
QLabel#section { color: @muted@; font-weight: 500; }
QPlainTextEdit, QLineEdit, QTreeWidget {
    background: @surface@; border: 1px solid @border@;
    selection-background-color: @selbg@; selection-color: @selfg@;
}
QPlainTextEdit:focus, QLineEdit:focus, QTreeWidget:focus { border: 1px solid @accent@; }
QPlainTextEdit#diffPane, QPlainTextEdit#diffPane:focus { background: @bg@; border: none; }
QLineEdit { padding: 4px 6px; }
QTreeWidget::item { padding: 3px 2px; }
QTreeWidget::item:selected { background: @selbg@; color: @selfg@; }
QTreeWidget::item:hover:!selected { background: @hover@; }
QHeaderView::section {
    background: @bg@; color: @muted@; border: none;
    border-bottom: 1px solid @border@; padding: 4px 6px;
}
QPushButton, QToolButton { background: @surface@; border: 1px solid @border@; padding: 6px 14px; }
QToolButton { padding: 3px 9px; }
QToolButton::menu-indicator { image: none; width: 0; }
QPushButton:hover, QToolButton:hover { border-color: @accent@; }
QPushButton:pressed, QToolButton:pressed { background: @hover@; }
QPushButton:disabled, QToolButton:disabled { color: @muted@; border-color: @border@; }
QPushButton#primary { background: @accentTint@; color: @fg@; border-color: @accentEdge@; font-weight: bold; }
QPushButton#primary:hover { background: @accentTintHover@; border-color: @accent@; }
QPushButton#primary:disabled { background: @border@; color: @muted@; border-color: @border@; }
QCheckBox { spacing: 8px; background: transparent; }
QMenu { background: @surface@; border: 1px solid @border@; padding: 4px; }
QMenu::item { padding: 5px 14px; background: transparent; }
QMenu::item:selected { background: @selbg@; color: @selfg@; }
QMenu::item:disabled { color: @muted@; }
QToolTip { background: @surface@; color: @fg@; border: 1px solid @border@; padding: 4px; }
QSplitter::handle { background: @border@; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:vertical { background: @border@; min-height: 24px; }
QScrollBar::handle:horizontal { background: @border@; min-width: 24px; }
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background: @muted@; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
QAbstractScrollArea::corner { background: @bg@; }
)");
    const ThemeColors &c = m_c;
    const QList<QPair<const char *, QColor>> tokens{
        {"@bg@", c.background}, {"@fg@", c.foreground}, {"@accent@", c.accent},
        {"@selbg@", c.selectionBg}, {"@selfg@", c.selectionFg}, {"@muted@", c.muted},
        {"@border@", c.border}, {"@surface@", c.surface}, {"@hover@", c.hover}};
    for (const auto &t : tokens)
        css.replace(QString::fromLatin1(t.first), t.second.name());
    // The accent see-through, for the primary button's tint: real alpha, so a
    // translucent window shows through it too.
    auto rgba = [](const QColor &col, qreal alpha) {
        return QStringLiteral("rgba(%1, %2, %3, %4)").arg(col.red()).arg(col.green()).arg(col.blue()).arg(int(alpha * 255));
    };
    css.replace(QStringLiteral("@accentTintHover@"), rgba(c.accent, 0.42));
    css.replace(QStringLiteral("@accentTint@"), rgba(c.accent, 0.30));
    css.replace(QStringLiteral("@accentEdge@"), rgba(c.accent, 0.75));

    // Fonts go in the stylesheet: with one set, Qt gives lists and labels the
    // desktop's font over the app font. The desktop's sizes, scaled down --
    // they're sized for reading, og is dense: at 11pt, UI text 10pt, code 9pt.
    auto pt = [](qreal size) { return QString::number(std::round(size * 2) / 2); };
    auto scaled = [](const QFont &f, qreal at11) { return (f.pointSizeF() > 0 ? f.pointSizeF() : 11) * at11 / 11; };
    const QFont ui = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    const qreal uiSize = scaled(ui, 10);
    const qreal codeSize = scaled(QFontDatabase::systemFont(QFontDatabase::FixedFont), 9);
    css.replace(QStringLiteral("@uiFamily@"), ui.family());
    css.replace(QStringLiteral("@uiSize@"), pt(uiSize));
    css.replace(QStringLiteral("@codeFamily@"), m_font.family());
    css.replace(QStringLiteral("@codeSize@"), pt(codeSize));
    css.replace(QStringLiteral("@titleSize@"), pt(uiSize * 13 / 11));
    return css;
}
