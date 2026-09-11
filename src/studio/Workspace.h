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
    /// The tags a `tags` tab has walked into, in the order they were walked.
    ///
    /// A list and not a set, though the question it asks is one: `#work/#draft`
    /// and `#draft/#work` name the same files, and the only thing the order is
    /// for is knowing which one `h` takes back off.
    Q_PROPERTY(QStringList facets READ facets WRITE setFacets NOTIFY facetsChanged)
    Q_PROPERTY(bool untagged READ untagged WRITE setUntagged NOTIFY facetsChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    /// A shell pattern a files tab is answering across the roots -- `*.pdf` --
    /// out of the lists of names `sweep` writes. Empty when the tab is a
    /// folder. It is not the filter: the filter narrows what is on screen,
    /// and this decides what is on screen.
    Q_PROPERTY(QString pattern READ pattern WRITE setPattern NOTIFY patternChanged)
    /// Whether a files tab is listing what the desktop opened lately, out of
    /// its recently-used list, instead of a folder. Exclusive with a pattern
    /// and a tag the way those are with each other: one question per tab.
    Q_PROPERTY(bool recent READ recent WRITE setRecent NOTIFY recentChanged)
    Q_PROPERTY(QString viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    Q_PROPERTY(QString sortRole READ sortRole WRITE setSortRole NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending WRITE setSortAscending NOTIFY sortChanged)

public:
    explicit TabState(QString kind, QObject *parent = nullptr);

    /// The folder a tab opens in: the home directory, unless the command
    /// line named one. Static and set by `main` before the engine runs,
    /// because the first tab is made by the QML the engine loads and there
    /// is no instance to hand a path to before that.
    static void setStartPath(const QString &path);
    static QString startPath();

    QString kind() const { return m_kind; }
    QString title() const;
    QString selectedTag() const { return m_selectedTag; }
    QString selectedNode() const { return m_selectedNode; }
    QString cwd() const { return m_cwd; }
    QStringList facets() const { return m_facets; }
    void setFacets(const QStringList &facets);
    /// Whether a tags tab is standing in the complement of its vocabulary --
    /// what carries no tag at all.
    ///
    /// Not a facet, and it cannot be one: the facets are a query, `#a/#b` is
    /// `ls --tag a --tag b`, and "has no tag" is not a tag. It is the other
    /// place the root of a tags tab leads to, so it takes a flag of its own and
    /// the two are exclusive by construction.
    bool untagged() const { return m_untagged; }
    void setUntagged(bool untagged);
    Q_INVOKABLE void enterFacet(const QString &tag);
    Q_INVOKABLE void leaveFacet();
    QString filter() const { return m_filter; }
    QString pattern() const { return m_pattern; }
    void setPattern(const QString &pattern);
    bool recent() const { return m_recent; }
    void setRecent(bool recent);
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
    void facetsChanged();
    void filterChanged();
    void patternChanged();
    void recentChanged();
    void viewModeChanged();
    void showHiddenChanged();
    void sortChanged();
    void markedChanged();

private:
    QString m_kind;
    QString m_selectedTag;
    QString m_selectedNode;
    QString m_cwd;
    QStringList m_facets;
    bool m_untagged = false;
    QString m_filter;
    QString m_pattern;
    bool m_recent = false;
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
    Q_INVOKABLE TabState *tabAtIndex(int paneIndex, int tabIndex) const;
    Q_INVOKABLE void addTab(const QString &kind);
    Q_INVOKABLE void addTabIn(int paneIndex, const QString &kind);
    /// A new tab already standing somewhere, which is what every `open in a new
    /// tab` means: made and then walked would show the home folder for a frame
    /// and put a listing nobody asked for in the history of the new tab.
    Q_INVOKABLE void openTabIn(int paneIndex, const QString &kind, const QString &directory);
    /// The same, in a pane that does not exist yet.
    Q_INVOKABLE void splitWith(const QString &kind, const QString &directory);
    /// This tab again, beside it: the same folder, tag, view and sort.
    ///
    /// The marks are not carried over. They name a set somebody assembled by
    /// hand to act on, and a second copy of it in a second tab is a second
    /// chance to act on it by accident.
    Q_INVOKABLE void duplicateTab(int paneIndex, int tabIndex);
    Q_INVOKABLE void closeTabsAfter(int paneIndex, int tabIndex);
    /// Moves a tab to the pane beside it, with everything it was showing.
    Q_INVOKABLE void moveTabToOtherPane(int paneIndex, int tabIndex);
    /// The pane a `to the other pane` command means: the next one, wrapping, or
    /// nothing at all when this is the only pane there is.
    Q_INVOKABLE PaneState *otherPane(int paneIndex) const;
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
