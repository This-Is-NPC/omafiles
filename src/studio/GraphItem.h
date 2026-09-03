#pragma once

#include "IndexModel.h"
#include "Theme.h"

#include <QFontMetricsF>
#include <QPointF>
#include <QRectF>
#include <QQuickPaintedItem>
#include <QtQmlIntegration/qqmlintegration.h>
#include <QString>
#include <QVector>

namespace omafiles {

class GraphItem : public QQuickPaintedItem
{
    Q_OBJECT
    // Declared here rather than by a `qmlRegisterType` call in main(): a type
    // registered in code is one `qmllint` cannot see, and both the studio and
    // the test harness would have to remember the call. Declaring it puts the
    // registration in the build, where all three get it from the same place.
    QML_ELEMENT
    Q_PROPERTY(omafiles::IndexModel *index READ index WRITE setIndex NOTIFY indexChanged)
    Q_PROPERTY(omafiles::Theme *theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(QString selectedId READ selectedId WRITE setSelectedId NOTIFY selectedIdChanged)
    /// How far the map is zoomed in.
    ///
    /// Not `scale`: every QQuickItem already has one, and a second property of
    /// that name on this type shadows it -- the item's own render scale becomes
    /// unassignable by name, and the reader cannot tell which of the two a
    /// binding means.
    Q_PROPERTY(qreal zoomLevel READ zoomLevel NOTIFY viewChanged)
    /// What the status line says about a graph tab, which has no folder under
    /// it to count: how many nodes there are, which one the cursor is on, and
    /// how much of the index the map is drawing.
    Q_PROPERTY(int nodeCount READ nodeCount NOTIFY graphChanged)
    Q_PROPERTY(int selectedRow READ selectedRow NOTIFY graphChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY graphChanged)

public:
    explicit GraphItem(QQuickItem *parent = nullptr);

    IndexModel *index() const { return m_index; }
    void setIndex(IndexModel *index);
    Theme *theme() const { return m_theme; }
    void setTheme(Theme *theme);

    QString selectedId() const { return m_selectedId; }
    void setSelectedId(const QString &id);
    qreal zoomLevel() const { return m_scale; }

    int nodeCount() const { return m_nodes.size(); }
    int selectedRow() const;
    /// The tags and the files on the map, and -- when the index holds more
    /// files than one map can carry -- how many of them are drawn.
    QString summary() const;

    void paint(QPainter *painter) override;

    Q_INVOKABLE void resetView();

    // The graph answers the same keys as the file list, and for the same
    // reason: it is a view of the index, and a view of the index you can only
    // work with a mouse is half a view. The window holds the keyboard and calls
    // these, so there is one place deciding what a key means.
    Q_INVOKABLE void stepSelection(int delta);
    Q_INVOKABLE void selectEnd(bool last);
    Q_INVOKABLE void activateSelection();
    Q_INVOKABLE void menuForSelection();
    Q_INVOKABLE void zoomBy(qreal factor);
    Q_INVOKABLE void panBy(qreal dx, qreal dy);
    Q_INVOKABLE QPointF pan() const;

signals:
    void indexChanged();
    void themeChanged();
    void selectedIdChanged();
    void viewChanged();
    /// The node list, or the node the cursor is on, changed.
    void graphChanged();
    void nodeActivated(const QString &id, const QString &kind, const QString &label);
    // `double` rather than `qreal` on a signal QML connects to: moc records the
    // spelling, `qreal` is a typedef the QML tooling has no entry for, and a
    // handler whose parameter types cannot be resolved is a handler the compiler
    // falls back to interpreting.
    void nodeMenuRequested(const QString &id, const QString &kind, const QString &path,
                          double x, double y);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    struct Node {
        QString id;
        QString kind;
        QString label;
        QPointF pos;
        /// For a file, the tag node it hangs off. -1 for a tag.
        int owner = -1;
    };
    // A branch is the line from a tag to a file that hangs off it; a member
    // link is the same file's other tags, which are real but are not where it
    // sits; a shared link is two tags that appear together on some file. Only
    // the branches are drawn at rest -- the other two turn a readable map into
    // a ball of string, and they are what you actually want to see when you
    // touch a node, not before.
    enum EdgeKind { Branch, Member, Shared };
    struct Edge {
        int from = 0;
        int to = 0;
        int kind = Branch;
    };

    void rebuild();
    void ensureNodes();
    QString emptyText() const;
    QRectF labelRect(const Node &node, const QFontMetricsF &fm) const;
    static QString displayLabel(const Node &node);
    void layout();
    QPointF toView(QPointF graph) const;
    int hitNode(QPointF view) const;
    void selectIndex(int index);

    IndexModel *m_index = nullptr;
    Theme *m_theme = nullptr;
    QVector<Node> m_nodes;
    QVector<Edge> m_edges;
    int m_tagCount = 0;
    /// Files in the index with a tag on them, drawn or not.
    int m_taggedFiles = 0;
    QString m_selectedId;
    QString m_hoverId;
    qreal m_scale = 1.0;
    QPointF m_pan;
    bool m_dragging = false;
    QPointF m_lastMouse;
    bool m_dirty = true;
};

} // namespace omafiles
