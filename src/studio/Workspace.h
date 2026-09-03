#pragma once

#include <QObject>
#include <QQmlListProperty>
#include <QSet>
#include <QtQmlIntegration/qqmlintegration.h>
#include <QString>
#include <QVector>

namespace omafiles {

// One tab's state.
//
// Every property notifies on its own signal. A single `changed()` for the whole
// object would be cheap to write and wrong to use: it is the NOTIFY of the list
// properties further down, so one `cwd` write would tell QML that the tab list
// itself had changed and every Repeater over it would tear down and rebuild its
// delegates -- taking the focused view, and the keyboard with it.
class TabState : public QObject
{
    Q_OBJECT
    // Named for the tooling, never built in QML: these come out of `Workspace`
    // and a second one made by hand would belong to nothing.
    QML_ELEMENT
    QML_UNCREATABLE("comes from Workspace")
    Q_PROPERTY(QString kind READ kind CONSTANT)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString selectedTag READ selectedTag WRITE setSelectedTag NOTIFY selectedTagChanged)
    Q_PROPERTY(QString selectedNode READ selectedNode WRITE setSelectedNode NOTIFY selectedNodeChanged)
    Q_PROPERTY(QString cwd READ cwd WRITE setCwd NOTIFY cwdChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QString viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    Q_PROPERTY(QString sortRole READ sortRole WRITE setSortRole NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending WRITE setSortAscending NOTIFY sortChanged)

public:
    explicit TabState(QString kind, QObject *parent = nullptr);

    QString kind() const { return m_kind; }
    QString title() const;
    QString selectedTag() const { return m_selectedTag; }
    QString selectedNode() const { return m_selectedNode; }
    QString cwd() const { return m_cwd; }
    QString filter() const { return m_filter; }
    QString viewMode() const { return m_viewMode; }
    bool showHidden() const { return m_showHidden; }
    QString sortRole() const { return m_sortRole; }
    bool sortAscending() const { return m_sortAscending; }

    void setSelectedTag(const QString &tag);
    void setSelectedNode(const QString &id);
    void setCwd(const QString &path);
    void setFilter(const QString &text);
    void setViewMode(const QString &mode);
    void setShowHidden(bool show);
    void setSortRole(const QString &role);
    void setSortAscending(bool ascending);

    /// The rows marked in this tab.
    ///
    /// Marks belong to the tab and not to the view drawing it: two tabs in one
    /// pane share the view, so marks held there would still be counted, shown
    /// and acted on after a switch put a different folder underneath them.
    bool isMarked(const QString &path) const { return m_marked.contains(path); }
    int markedCount() const { return m_marked.size(); }
    void toggleMark(const QString &path);
    void setMarked(const QSet<QString> &paths);

    /// The path the cursor is on, so a rebuild can put it back. Per tab for the
    /// same reason the marks are.
    QString cursorPath() const { return m_cursorPath; }
    void setCursorPath(const QString &path) { m_cursorPath = path; }

    Q_INVOKABLE void toggleHidden();
    Q_INVOKABLE void pickSortRole(const QString &role);
    Q_INVOKABLE void cycleSortRole();
    Q_INVOKABLE void toggleSortDirection();
    /// Picking a new field starts it the way that field is usually read.
    Q_INVOKABLE static bool ascendingByDefault(const QString &role);
    static QString validSortRole(const QString &role);

signals:
    void titleChanged();
    void selectedTagChanged();
    void selectedNodeChanged();
    void cwdChanged();
    void filterChanged();
    void viewModeChanged();
    void showHiddenChanged();
    void sortChanged();
    void markedChanged();

private:
    QString m_kind;
    QString m_selectedTag;
    QString m_selectedNode;
    QString m_cwd;
    QString m_filter;
    QString m_viewMode = QStringLiteral("list");
    bool m_showHidden = false;
    QString m_sortRole = QStringLiteral("name");
    bool m_sortAscending = true;
    QSet<QString> m_marked;
    QString m_cursorPath;
};

class PaneState : public QObject
{
    Q_OBJECT
    // Named for the tooling, never built in QML: these come out of `Workspace`
    // and a second one made by hand would belong to nothing.
    QML_ELEMENT
    QML_UNCREATABLE("comes from Workspace")
    // `tabs` notifies only when a tab is added or removed, never when one of
    // them changes: rebuilding the tab strip is fine, rebuilding the view under
    // it on every keystroke is not.
    //
    // Namespace written out on every property whose type is one of ours: moc
    // records a property type exactly as it is spelled here, and the type
    // registry knows these by their full name, so an unqualified `TabState`
    // yields a qmltypes the tooling can parse but not resolve.
    Q_PROPERTY(QQmlListProperty<omafiles::TabState> tabs READ tabs NOTIFY tabsChanged)
    Q_PROPERTY(int tabCount READ tabCount NOTIFY tabsChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentChanged)
    Q_PROPERTY(omafiles::TabState *current READ current NOTIFY currentChanged)

public:
    explicit PaneState(const QString &kind = QStringLiteral("files"),
                        QObject *parent = nullptr);

    QQmlListProperty<TabState> tabs();
    int tabCount() const { return m_tabs.size(); }
    QVector<TabState *> tabList() const { return m_tabs; }
    int currentIndex() const { return m_current; }
    TabState *current() const;
    void setCurrentIndex(int index);

    TabState *addTab(const QString &kind);
    void closeTab(int index);
    Q_INVOKABLE void stepTab(int delta);

signals:
    void tabsChanged();
    void currentChanged();

private:
    static qsizetype tabCountFn(QQmlListProperty<TabState> *list);
    static TabState *tabAt(QQmlListProperty<TabState> *list, qsizetype index);

    QVector<TabState *> m_tabs;
    int m_current = 0;
};

class Workspace : public QObject
{
    Q_OBJECT
    // A QML singleton rather than a context property: a context property is
    // invisible to `qmllint`, so every use of it is an unqualified access the
    // linter can only warn about. A declared singleton is a type the tooling
    // and the engine agree on, and it is looked up once rather than searched
    // for in the context chain.
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QQmlListProperty<omafiles::PaneState> panes READ panes NOTIFY panesChanged)
    Q_PROPERTY(int paneCount READ paneCount NOTIFY panesChanged)
    Q_PROPERTY(int focusedPane READ focusedPane WRITE setFocusedPane NOTIFY focusedPaneChanged)
    Q_PROPERTY(omafiles::TabState *currentTab READ currentTab NOTIFY currentTabChanged)

public:
    explicit Workspace(QObject *parent = nullptr);

    QQmlListProperty<PaneState> panes();
    int paneCount() const { return m_panes.size(); }
    int focusedPane() const { return m_focused; }
    void setFocusedPane(int index);
    TabState *currentTab() const;

    Q_INVOKABLE PaneState *paneAtIndex(int index) const;
    Q_INVOKABLE void addTab(const QString &kind);
    Q_INVOKABLE void addTabIn(int paneIndex, const QString &kind);
    Q_INVOKABLE void closeTab(int paneIndex, int tabIndex);
    Q_INVOKABLE void closeCurrentTab();
    Q_INVOKABLE void stepTab(int delta);
    Q_INVOKABLE void selectTab(int index);
    Q_INVOKABLE void splitRight(const QString &kind);
    Q_INVOKABLE void closePane(int paneIndex);
    Q_INVOKABLE void stepPane(int delta);

signals:
    void panesChanged();
    void focusedPaneChanged();
    void currentTabChanged();

private:
    static qsizetype paneCountFn(QQmlListProperty<PaneState> *list);
    static PaneState *paneAt(QQmlListProperty<PaneState> *list, qsizetype index);

    void adopt(PaneState *pane, int at);

    QVector<PaneState *> m_panes;
    int m_focused = 0;
};

} // namespace omafiles
