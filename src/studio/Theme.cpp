#include "Theme.h"

#include "Paths.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <optional>

namespace omafiles {
namespace {

QString currentThemePath()
{
    return stateHome() + QStringLiteral("/omarchy/current/theme");
}

// The hyprland configs that carry the corner rounding, most specific first.
// `colors.toml` is colours only; the rounding lives on the window manager's
// side of the desktop.
QStringList roundingPaths()
{
    const QString config = configHome();
    QString omarchy;
    if (!nameFromBytes(qgetenv("OMARCHY_PATH"), &omarchy) || omarchy.isEmpty())
        omarchy = QStringLiteral("/usr/share/omarchy");
    return {config + QStringLiteral("/hypr/looknfeel.lua"),
            omarchy + QStringLiteral("/default/hypr/looknfeel.lua")};
}

// `decoration.rounding` out of a Lua config, or nothing at all.
//
// A line beginning `--` is a Lua comment, and the override omarchy ships is the
// whole config commented out -- so a match inside one would report a rounding
// the desktop is not using. A file that says nothing on the subject yields
// nothing rather than zero, which is what lets the caller fall through to the
// next file instead of stopping at the first one that happens to exist.
std::optional<int> readRounding(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return std::nullopt;
    // Anchored, so the groupbar's `gradient_rounding` is a different key.
    static const QRegularExpression key(QStringLiteral("^rounding\\s*=\\s*(\\d+)"));
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.startsWith(QLatin1String("--")))
            continue;
        const QRegularExpressionMatch match = key.match(line);
        if (match.hasMatch())
            return match.captured(1).toInt();
    }
    return std::nullopt;
}

QHash<QString, QString> readFlatToml(const QString &path)
{
    QHash<QString, QString> values;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return values;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
            || line.startsWith(QLatin1Char('[')))
            continue;
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals < 0)
            continue;
        const QString key = line.left(equals).trimmed();
        QString value = line.mid(equals + 1).trimmed();
        if (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
            value = value.mid(1, value.size() - 2);
        values.insert(key, value);
    }
    return values;
}

// The colour under `key`, opaque: an alpha in the file is dropped.
QColor colourOr(const QHash<QString, QString> &values, const QString &key,
                const QColor &fallback)
{
    QColor parsed(values.value(key));
    if (!parsed.isValid())
        return fallback;
    parsed.setAlpha(255);
    return parsed;
}

QColor mix(const QColor &from, const QColor &toward, qreal amount)
{
    return QColor::fromRgbF(from.redF() + (toward.redF() - from.redF()) * amount,
                            from.greenF() + (toward.greenF() - from.greenF()) * amount,
                            from.blueF() + (toward.blueF() - from.blueF()) * amount);
}

// The keys of the tag palette in colors.toml: a terminal colour's number, and
// the name omarchy writes it under.
struct PaletteKey
{
    const char *number;
    const char *name;
};
constexpr std::array<PaletteKey, 12> paletteKeys{{
    {"color1", "red"}, {"color2", "green"}, {"color3", "yellow"},
    {"color4", "blue"}, {"color5", "magenta"}, {"color6", "cyan"},
    {"color9", "bright_red"}, {"color10", "bright_green"}, {"color11", "bright_yellow"},
    {"color12", "bright_blue"}, {"color13", "bright_magenta"}, {"color14", "bright_cyan"},
}};

// WCAG relative luminance of an opaque colour.
qreal luminance(const QColor &colour)
{
    const auto channel = [](qreal value) {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(colour.redF()) + 0.7152 * channel(colour.greenF())
        + 0.0722 * channel(colour.blueF());
}

} // namespace

qreal contrast(const QColor &a, const QColor &b)
{
    const qreal first = luminance(a);
    const qreal second = luminance(b);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

Theme::Theme(QObject *parent)
    : QObject(parent)
    , m_themePath(currentThemePath())
    , m_roundingPaths(roundingPaths())
{
    reload();
    // Both signals re-take the watches before reading. A theme switcher and an
    // editor alike write `colors.toml` by writing a temporary and renaming it
    // over, and a watch follows the old file out: without this the first change
    // is seen and no later one is.
    const auto rearm = [this] {
        watch();
        reload();
    };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, rearm);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, rearm);
    watch();
}

void Theme::watch()
{
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    const QString colours = m_themePath + QStringLiteral("/colors.toml");
    if (QFile::exists(colours))
        m_watcher.addPath(colours);
    const QString pointer = typedDirectory(m_themePath, nullptr);
    if (QFileInfo(pointer).isDir())
        m_watcher.addPath(pointer);
    for (const QString &looknfeel : std::as_const(m_roundingPaths)) {
        if (QFile::exists(looknfeel))
            m_watcher.addPath(looknfeel);
    }
}

void Theme::reload()
{
    // Square when there is no hyprland config to read, which is what omarchy's
    // own default says in any case.
    m_rounding = 0;
    for (const QString &looknfeel : std::as_const(m_roundingPaths)) {
        if (const std::optional<int> found = readRounding(looknfeel)) {
            m_rounding = *found;
            break;
        }
    }

    const QHash<QString, QString> values =
        readFlatToml(m_themePath + QStringLiteral("/colors.toml"));
    if (values.isEmpty()) {
        emit changed();
        return;
    }

    m_base = colourOr(values, QStringLiteral("background"), m_base);
    m_foreground = colourOr(values, QStringLiteral("foreground"), m_foreground);
    m_accent = colourOr(values, QStringLiteral("accent"), m_accent);
    m_urgent = colourOr(values, QStringLiteral("red"), m_urgent);
    for (std::size_t slot = 0; slot < paletteKeys.size(); ++slot) {
        m_tagPalette[slot] = colourOr(values, QLatin1String(paletteKeys[slot].number),
                                      colourOr(values, QLatin1String(paletteKeys[slot].name),
                                               m_tagPalette[slot]));
    }
    emit changed();
}

QColor Theme::chrome() const
{
    return mix(m_base, m_foreground, 0.05);
}

QColor Theme::raised() const
{
    return mix(m_base, m_foreground, 0.10);
}

QColor Theme::line() const
{
    return mix(m_base, m_foreground, 0.16);
}

QColor Theme::hoverBase() const
{
    return mix(m_base, m_foreground, 0.06);
}

QColor Theme::hoverChrome() const
{
    return mix(chrome(), m_foreground, 0.06);
}

QColor Theme::hoverRaised() const
{
    return mix(raised(), m_foreground, 0.06);
}

QColor Theme::cursor() const
{
    return mix(m_base, m_accent, 0.22);
}

QColor Theme::marked() const
{
    return mix(m_base, m_accent, 0.12);
}

QColor Theme::dim() const
{
    // Foreground eased toward the base, by at most 45 %, and by less where that
    // would read below 4.5:1 on chrome.
    const QColor surface = chrome();
    for (int step = 9; step > 0; --step) {
        const QColor eased = mix(m_foreground, m_base, step * 0.05);
        if (contrast(eased, surface) >= 4.5)
            return eased;
    }
    return m_foreground;
}

QColor Theme::tagColour(const QString &tag) const
{
    quint32 hash = 2166136261u;
    for (const char byte : tag.toUtf8()) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 16777619u;
    }
    return m_tagPalette[hash % m_tagPalette.size()];
}

QColor Theme::scrim() const
{
    return mix(m_base, Qt::black, 0.35);
}

} // namespace omafiles
