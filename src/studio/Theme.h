#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace omafiles {

class Theme : public QObject
{
    Q_OBJECT
    // A QML singleton rather than a context property: a context property is
    // invisible to `qmllint`, so every use of it is an unqualified access the
    // linter can only warn about. A declared singleton is a type the tooling
    // and the engine agree on, and it is looked up once rather than searched
    // for in the context chain.
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(int rounding READ rounding NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor urgent READ urgent NOTIFY changed)
    Q_PROPERTY(QColor panel READ panel NOTIFY changed)
    Q_PROPERTY(QColor sunken READ sunken NOTIFY changed)
    Q_PROPERTY(QColor line READ line NOTIFY changed)
    Q_PROPERTY(QColor dim READ dim NOTIFY changed)
    Q_PROPERTY(QColor scrim READ scrim NOTIFY changed)
    Q_PROPERTY(QString fontFamily READ fontFamily CONSTANT)
    // The window's metrics, in pixels: every QML size reads one of these.
    Q_PROPERTY(int spacingXs READ spacingXs CONSTANT)
    Q_PROPERTY(int spacingS READ spacingS CONSTANT)
    Q_PROPERTY(int spacingM READ spacingM CONSTANT)
    Q_PROPERTY(int spacingL READ spacingL CONSTANT)
    Q_PROPERTY(int spacingXl READ spacingXl CONSTANT)
    Q_PROPERTY(int inset READ inset CONSTANT)
    Q_PROPERTY(int fontSmall READ fontSmall CONSTANT)
    Q_PROPERTY(int fontBody READ fontBody CONSTANT)
    Q_PROPERTY(int fontTitle READ fontTitle CONSTANT)
    Q_PROPERTY(int rowHeight READ rowHeight CONSTANT)
    Q_PROPERTY(int barHeight READ barHeight CONSTANT)
    Q_PROPERTY(int stripHeight READ stripHeight CONSTANT)
    Q_PROPERTY(int controlHeight READ controlHeight CONSTANT)
    Q_PROPERTY(int chipHeight READ chipHeight CONSTANT)
    Q_PROPERTY(int iconSize READ iconSize CONSTANT)
    Q_PROPERTY(int iconGap READ iconGap CONSTANT)
    Q_PROPERTY(int focusRing READ focusRing CONSTANT)

public:
    explicit Theme(QObject *parent = nullptr);

    int rounding() const { return m_rounding; }

    QColor background() const { return m_background; }
    QColor foreground() const { return m_foreground; }
    QColor accent() const { return m_accent; }
    QColor urgent() const { return m_urgent; }

    QColor panel() const;
    QColor sunken() const;
    QColor line() const;
    QColor dim() const;
    /// The wash a sheet or dialog lays over the window behind it.
    QColor scrim() const { return fill(m_background, 0.55); }

    QString fontFamily() const { return QStringLiteral("monospace"); }

    // Spacing steps on a 4 px grid; `inset` is the padding of every panel.
    static constexpr int spacingXs() { return 4; }
    static constexpr int spacingS() { return 8; }
    static constexpr int spacingM() { return 12; }
    static constexpr int spacingL() { return 16; }
    static constexpr int spacingXl() { return 24; }
    static constexpr int inset() { return spacingM(); }
    // Type: captions and keycaps, rows and values, sheet titles.
    static constexpr int fontSmall() { return 10; }
    static constexpr int fontBody() { return 12; }
    static constexpr int fontTitle() { return 14; }
    // Heights of a list row, the header bar, a strip (tabs, footer), a control
    // (buttons, tabs, chips that act) and a tag chip.
    static constexpr int rowHeight() { return 28; }
    static constexpr int barHeight() { return 36; }
    static constexpr int stripHeight() { return 28; }
    static constexpr int controlHeight() { return 24; }
    static constexpr int chipHeight() { return 18; }
    // A row's icon and the gap between it and the row's text.
    static constexpr int iconSize() { return 16; }
    static constexpr int iconGap() { return spacingS(); }
    // The width of the focus ring, drawn just outside the box it marks.
    static constexpr int focusRing() { return 2; }
    Q_INVOKABLE QColor fill(const QColor &role, qreal alpha) const;

signals:
    void changed();

private:
    void reload();
    void watch();

    QFileSystemWatcher m_watcher;
    QString m_themePath;
    /// The hyprland configs that carry `decoration.rounding`, most specific
    /// first: the user's override, then omarchy's default.
    QStringList m_roundingPaths;
    int m_rounding = 0;
    bool m_dark = true;
    QColor m_background{"#16161e"};
    QColor m_foreground{"#c0caf5"};
    QColor m_accent{"#7aa2f7"};
    QColor m_urgent{"#f7768e"};
    QColor m_muted{"#565f89"};
};

} // namespace omafiles
