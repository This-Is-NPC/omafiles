#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <array>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <array>

namespace omafiles {

/// The WCAG contrast ratio of two opaque colours, from 1 to 21.
qreal contrast(const QColor &a, const QColor &b);

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
    // Every colour is opaque (docs/design.md §3): a surface never shows what
    // is under it.
    Q_PROPERTY(QColor base READ base NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor urgent READ urgent NOTIFY changed)
    Q_PROPERTY(QColor chrome READ chrome NOTIFY changed)
    Q_PROPERTY(QColor raised READ raised NOTIFY changed)
    Q_PROPERTY(QColor line READ line NOTIFY changed)
    Q_PROPERTY(QColor hoverBase READ hoverBase NOTIFY changed)
    Q_PROPERTY(QColor hoverChrome READ hoverChrome NOTIFY changed)
    Q_PROPERTY(QColor hoverRaised READ hoverRaised NOTIFY changed)
    Q_PROPERTY(QColor cursor READ cursor NOTIFY changed)
    Q_PROPERTY(QColor marked READ marked NOTIFY changed)
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
    Q_PROPERTY(int fontCaption READ fontCaption CONSTANT)
    Q_PROPERTY(int fontHeading READ fontHeading CONSTANT)
    Q_PROPERTY(int fontBody READ fontBody CONSTANT)
    Q_PROPERTY(int fontTitle READ fontTitle CONSTANT)
    Q_PROPERTY(int rowHeight READ rowHeight CONSTANT)
    Q_PROPERTY(int barHeight READ barHeight CONSTANT)
    Q_PROPERTY(int stripHeight READ stripHeight CONSTANT)
    Q_PROPERTY(int headingHeight READ headingHeight CONSTANT)
    Q_PROPERTY(int controlHeight READ controlHeight CONSTANT)
    Q_PROPERTY(int chipHeight READ chipHeight CONSTANT)
    Q_PROPERTY(int tagSwatch READ tagSwatch CONSTANT)
    Q_PROPERTY(int removeGlyph READ removeGlyph CONSTANT)
    Q_PROPERTY(int listLeast READ listLeast CONSTANT)
    Q_PROPERTY(int iconSize READ iconSize CONSTANT)
    Q_PROPERTY(int iconGap READ iconGap CONSTANT)
    Q_PROPERTY(int focusRing READ focusRing CONSTANT)
    Q_PROPERTY(int scrollBarWidth READ scrollBarWidth CONSTANT)
    Q_PROPERTY(int scrollBarHover READ scrollBarHover CONSTANT)

public:
    explicit Theme(QObject *parent = nullptr);

    int rounding() const { return m_rounding; }

    /// The content surface: the file list, sheet bodies, the current tab.
    QColor base() const { return m_base; }
    QColor foreground() const { return m_foreground; }
    QColor accent() const { return m_accent; }
    QColor urgent() const { return m_urgent; }

    /// The surface of the bars, the sidebar and the details panel.
    QColor chrome() const;
    /// Menus, tooltips, keycaps and controls at rest.
    QColor raised() const;
    /// Every hairline and control border.
    QColor line() const;
    /// A hovered row or control on each of the three surfaces.
    QColor hoverBase() const;
    QColor hoverChrome() const;
    QColor hoverRaised() const;
    /// The keyboard cursor's row or cell, and a control that is on.
    QColor cursor() const;
    /// A marked row.
    QColor marked() const;
    /// Secondary text, at least 4.5:1 against chrome where the theme allows.
    QColor dim() const;
    /// The opaque surface behind a sheet or dialog, in place of the window.
    QColor scrim() const;
    /// The colour of a tag: FNV-1a of its UTF-8 name into the theme's terminal
    /// colours 1..6 and 9..14. A call is no binding dependency: a QML binding
    /// also reads a role, which changes with the theme.
    Q_INVOKABLE QColor tagColour(const QString &tag) const;

    QString fontFamily() const { return QStringLiteral("monospace"); }

    // Spacing steps on a 4 px grid; `inset` is the padding of every panel.
    static constexpr int spacingXs() { return 4; }
    static constexpr int spacingS() { return 8; }
    static constexpr int spacingM() { return 12; }
    static constexpr int spacingL() { return 16; }
    static constexpr int spacingXl() { return 24; }
    static constexpr int inset() { return spacingM(); }
    // Type sizes: captions and keycaps (Normal), section headings (DemiBold),
    // rows and values (Normal), titles (DemiBold). Weight, not opacity, is the
    // second axis.
    static constexpr int fontCaption() { return 11; }
    static constexpr int fontHeading() { return 11; }
    static constexpr int fontBody() { return 13; }
    static constexpr int fontTitle() { return 15; }
    // Heights of a list row, the header bar, a strip (tabs, footer), a section
    // heading, a control (buttons, tabs, chips that act) and a tag chip.
    static constexpr int rowHeight() { return 28; }
    static constexpr int barHeight() { return 36; }
    static constexpr int stripHeight() { return 28; }
    static constexpr int headingHeight() { return 24; }
    static constexpr int controlHeight() { return 24; }
    static constexpr int chipHeight() { return 18; }
    // The side of a tag's colour square.
    static constexpr int tagSwatch() { return 6; }
    // The × that removes a tag from a details chip.
    static constexpr int removeGlyph() { return 12; }
    // The least height the panes keep above the details dock: a column
    // heading and four rows.
    static constexpr int listLeast() { return 136; }
    // A row's icon and the gap between it and the row's text.
    static constexpr int iconSize() { return 16; }
    static constexpr int iconGap() { return spacingS(); }
    // The width of the focus ring, drawn just outside the box it marks.
    static constexpr int focusRing() { return 2; }
    // A scroll bar's width at rest, which its view reserves, and under the
    // pointer.
    static constexpr int scrollBarWidth() { return 8; }
    static constexpr int scrollBarHover() { return 12; }

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
    QColor m_base{"#16161e"};
    QColor m_foreground{"#c0caf5"};
    QColor m_accent{"#7aa2f7"};
    QColor m_urgent{"#f7768e"};
    /// Red, green, yellow, blue, magenta, cyan, then their bright forms.
    std::array<QColor, 12> m_tagPalette{
        QColor{"#f7768e"}, QColor{"#9ece6a"}, QColor{"#e0af68"}, QColor{"#7aa2f7"},
        QColor{"#ad8ee6"}, QColor{"#449dab"}, QColor{"#ff7a93"}, QColor{"#b9f27c"},
        QColor{"#ff9e64"}, QColor{"#7da6ff"}, QColor{"#bb9af7"}, QColor{"#0db9d7"}};
};

} // namespace omafiles
