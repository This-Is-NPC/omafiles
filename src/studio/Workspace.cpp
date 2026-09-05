#include "Workspace.h"

#include <QDir>
#include <QFileInfo>

namespace omafiles {
namespace {
// Empty until `main` sets it, and resolved only when a tab asks, so a studio
// started with no argument opens exactly where it always did.
QString g_startPath;
} // namespace

void TabState::setStartPath(const QString &path)
{
    g_startPath = path;
}

QString TabState::startPath()
{
    return g_startPath.isEmpty() ? QDir::homePath() : g_startPath;
}

TabState::TabState(QString kind, QObject *parent)
    : QObject(parent)
    , m_kind(std::move(kind))
    , m_cwd(startPath())
{
}

QString TabState::title() const
{
    if (m_kind == QLatin1String("graph"))
        return QStringLiteral("Graph");
    if (m_kind == QLatin1String("tags")) {
        if (m_facets.isEmpty())
            return QStringLiteral("Tags");
        // Sorted, not in the order they were walked. The two orders name the
        // same files, and a title that changed with the route taken to it would
        // make one question look like two.
        QStringList sorted = m_facets;
        sorted.sort();
        return QStringLiteral("#") + sorted.join(QStringLiteral("/#"));
    }
    if (!m_selectedTag.isEmpty())
        return QStringLiteral("#") + m_selectedTag;
    const QString name = QFileInfo(m_cwd).fileName();
    return name.isEmpty() ? m_cwd : name;
}

// Marks end with the listing they were made in, for the reason `setCwd` gives:
// a mark you cannot see is one a destructive command would still act on.
void TabState::setFacets(const QStringList &facets)
{
    if (m_facets == facets)
        return;
    m_facets = facets;
    setMarked({});
    emit facetsChanged();
    emit titleChanged();
}

// Walking into a tag you are already under is not a move, and the listing
// would not change: the same files answer `#work` and `#work/#work`.
void TabState::enterFacet(const QString &tag)
{
    if (tag.isEmpty() || m_facets.contains(tag))
        return;
    setFacets(m_facets + QStringList{tag});
}

void TabState::leaveFacet()
{
    if (m_facets.isEmpty())
        return;
    QStringList rest = m_facets;
    rest.removeLast();
    setFacets(rest);
}

void TabState::setSelectedTag(const QString &tag)
{
    if (m_selectedTag == tag)
        return;
    m_selectedTag = tag;
    emit selectedTagChanged();
    emit titleChanged();
}

void TabState::setSelectedNode(const QString &id)
{
    if (m_selectedNode == id)
        return;
    m_selectedNode = id;
    emit selectedNodeChanged();
}

// Marks name files in the folder they were made in, so they end with it. A mark
// you cannot see is one a destructive key would still act on -- which is how
// every file manager with a selection treats walking into another directory.
void TabState::setCwd(const QString &path)
{
    if (m_cwd == path)
        return;
    m_cwd = path;
    setMarked({});
    emit cwdChanged();
    emit titleChanged();
}

void TabState::toggleMark(const QString &path)
{
    if (path.isEmpty())
        return;
    if (!m_marked.remove(path))
        m_marked.insert(path);
    emit markedChanged();
}

void TabState::setMarked(const QSet<QString> &paths)
{
    if (m_marked == paths)
        return;
    m_marked = paths;
    emit markedChanged();
}

void TabState::setFilter(const QString &text)
{
    if (m_filter == text)
        return;
    m_filter = text;
    emit filterChanged();
}

void TabState::setViewMode(const QString &mode)
{
    const QString next = (mode == QLatin1String("details") || mode == QLatin1String("icons"))
        ? mode
        : QStringLiteral("list");
    if (m_viewMode == next)
        return;
    m_viewMode = next;
    emit viewModeChanged();
}

// Dotfiles are hidden by convention and shown on request, per tab: two tabs
// side by side are two questions, and the answer to one is not the answer to
// the other.
void TabState::setShowHidden(bool show)
{
    if (m_showHidden == show)
        return;
    m_showHidden = show;
    emit showHiddenChanged();
}

void TabState::toggleHidden()
{
    setShowHidden(!m_showHidden);
}

void TabState::setSortRole(const QString &role)
{
    const QString next = validSortRole(role);
    if (m_sortRole == next)
        return;
    m_sortRole = next;
    emit sortChanged();
}

void TabState::setSortAscending(bool ascending)
{
    if (m_sortAscending == ascending)
        return;
    m_sortAscending = ascending;
    emit sortChanged();
}

// The keyboard equivalent of clicking the sort chips in turn.
//
// It walks the fields in the order the table lays them out, left to right: the
// key, the row of chips and the columns are one ordering of the same five
// things, so pressing the key twice lands where the eye expects.
//
// Which way each one starts is not a detail: name, tags and type go up, because
// that is the reading order of a directory, a vocabulary and an alphabet; size
// and date go down, because the row you want from those is the biggest or the
// newest one.
void TabState::pickSortRole(const QString &role)
{
    const QString valid = validSortRole(role);
    if (m_sortRole == valid) {
        toggleSortDirection();
        return;
    }
    m_sortRole = valid;
    m_sortAscending = ascendingByDefault(valid);
    emit sortChanged();
}

void TabState::cycleSortRole()
{
    const QString next = m_sortRole == QLatin1String("name")  ? QStringLiteral("tags")
                       : m_sortRole == QLatin1String("tags")  ? QStringLiteral("type")
                       : m_sortRole == QLatin1String("type")  ? QStringLiteral("size")
                       : m_sortRole == QLatin1String("size")  ? QStringLiteral("mtime")
                                                              : QStringLiteral("name");
    pickSortRole(next);
}

bool TabState::ascendingByDefault(const QString &role)
{
    return role == QLatin1String("name") || role == QLatin1String("tags")
        || role == QLatin1String("type");
}

QString TabState::validSortRole(const QString &role)
{
    if (role == QLatin1String("size") || role == QLatin1String("mtime")
        || role == QLatin1String("tags") || role == QLatin1String("type"))
        return role;
    return QStringLiteral("name");
}

void TabState::toggleSortDirection()
{
    m_sortAscending = !m_sortAscending;
    emit sortChanged();
}

PaneState::PaneState(const QString &kind, QObject *parent)
    : QObject(parent)
{
    addTab(kind);
}

QQmlListProperty<TabState> PaneState::tabs()
{
    return {this, this, &PaneState::tabCountFn, &PaneState::tabAt};
}

qsizetype PaneState::tabCountFn(QQmlListProperty<TabState> *list)
{
    return static_cast<PaneState *>(list->data)->m_tabs.size();
}

TabState *PaneState::tabAt(QQmlListProperty<TabState> *list, qsizetype index)
{
    return static_cast<PaneState *>(list->data)->m_tabs.at(index);
}

TabState *PaneState::current() const
{
    if (m_current < 0 || m_current >= m_tabs.size())
        return nullptr;
    return m_tabs.at(m_current);
}

void PaneState::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_tabs.size() || index == m_current)
        return;
    m_current = index;
    emit currentChanged();
}

TabState *PaneState::addTab(const QString &kind)
{
    auto *tab = new TabState(kind, this);
    m_tabs.append(tab);
    m_current = m_tabs.size() - 1;
    emit tabsChanged();
    emit currentChanged();
    return tab;
}

void PaneState::closeTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    if (m_tabs.size() == 1)
        return;
    TabState *tab = m_tabs.takeAt(index);
    tab->deleteLater();
    const int before = m_current;
    if (m_current >= m_tabs.size())
        m_current = m_tabs.size() - 1;
    emit tabsChanged();
    // The tab under the cursor is a different object now even when the index
    // did not move, so the view has to be told either way.
    Q_UNUSED(before)
    emit currentChanged();
}

// Wraps, because a tab strip is a ring: the point of a next-tab key is that you
// can hold it and reach every tab without looking at where you started.
void PaneState::stepTab(int delta)
{
    if (m_tabs.size() < 2 || delta == 0)
        return;
    const int count = m_tabs.size();
    setCurrentIndex(((m_current + delta) % count + count) % count);
}

Workspace::Workspace(QObject *parent)
    : QObject(parent)
{
    adopt(new PaneState(QStringLiteral("files"), this), 0);
}

// Every pane reports its current tab upward, so `currentTab` is correct no
// matter which pane changed and which one holds the focus.
void Workspace::adopt(PaneState *pane, int at)
{
    connect(pane, &PaneState::currentChanged, this, &Workspace::currentTabChanged);
    m_panes.insert(qBound(0, at, m_panes.size()), pane);
}

QQmlListProperty<PaneState> Workspace::panes()
{
    return {this, this, &Workspace::paneCountFn, &Workspace::paneAt};
}

qsizetype Workspace::paneCountFn(QQmlListProperty<PaneState> *list)
{
    return static_cast<Workspace *>(list->data)->m_panes.size();
}

PaneState *Workspace::paneAt(QQmlListProperty<PaneState> *list, qsizetype index)
{
    return static_cast<Workspace *>(list->data)->m_panes.at(index);
}

PaneState *Workspace::paneAtIndex(int index) const
{
    if (index < 0 || index >= m_panes.size())
        return nullptr;
    return m_panes.at(index);
}

TabState *Workspace::tabAtIndex(int paneIndex, int tabIndex) const
{
    PaneState *pane = paneAtIndex(paneIndex);
    return pane ? pane->tabList().value(tabIndex) : nullptr;
}

void Workspace::setFocusedPane(int index)
{
    if (index < 0 || index >= m_panes.size() || index == m_focused)
        return;
    m_focused = index;
    emit focusedPaneChanged();
    emit currentTabChanged();
}

TabState *Workspace::currentTab() const
{
    if (m_focused < 0 || m_focused >= m_panes.size())
        return nullptr;
    return m_panes.at(m_focused)->current();
}

void Workspace::addTab(const QString &kind)
{
    addTabIn(m_focused, kind);
}

void Workspace::addTabIn(int paneIndex, const QString &kind)
{
    if (paneIndex < 0 || paneIndex >= m_panes.size())
        return;
    m_panes.at(paneIndex)->addTab(kind);
    setFocusedPane(paneIndex);
    emit currentTabChanged();
}

void Workspace::openTabIn(int paneIndex, const QString &kind, const QString &directory)
{
    if (paneIndex < 0 || paneIndex >= m_panes.size())
        return;
    TabState *tab = m_panes.at(paneIndex)->addTab(kind);
    if (tab && !directory.isEmpty())
        tab->setCwd(directory);
    setFocusedPane(paneIndex);
    emit currentTabChanged();
}

void Workspace::splitWith(const QString &kind, const QString &directory)
{
    splitRight(kind);
    PaneState *pane = paneAtIndex(m_focused);
    TabState *tab = pane ? pane->current() : nullptr;
    if (tab && !directory.isEmpty()) {
        tab->setCwd(directory);
        emit currentTabChanged();
    }
}

namespace {

// Everything a tab is showing, minus the marks.
//
// The marks name a set somebody assembled by hand to act on, and a second copy
// of it in a second tab is a second chance to act on it by accident.
void copyView(const TabState &from, TabState *to)
{
    to->setCwd(from.cwd());
    to->setFacets(from.facets());
    to->setSelectedTag(from.selectedTag());
    to->setFilter(from.filter());
    to->setViewMode(from.viewMode());
    to->setShowHidden(from.showHidden());
    to->setSortRole(from.sortRole());
    to->setSortAscending(from.sortAscending());
}

} // namespace

void Workspace::duplicateTab(int paneIndex, int tabIndex)
{
    PaneState *pane = paneAtIndex(paneIndex);
    if (!pane)
        return;
    TabState *from = pane->tabList().value(tabIndex);
    if (!from)
        return;
    if (TabState *tab = pane->addTab(from->kind()))
        copyView(*from, tab);
    setFocusedPane(paneIndex);
    emit currentTabChanged();
}

// Backwards, so that closing one does not renumber the ones still to close.
void Workspace::closeTabsAfter(int paneIndex, int tabIndex)
{
    PaneState *pane = paneAtIndex(paneIndex);
    if (!pane || tabIndex < 0)
        return;
    for (int i = pane->tabCount() - 1; i > tabIndex; --i)
        pane->closeTab(i);
}

// Made again in the other pane and then closed here, rather than handed over: a
// `TabState` is a child of the pane that owns it. The pane it leaves has to keep
// a tab -- `closeTab` refuses the last one -- so a pane showing only this tab
// keeps it, and the entry that offers this is drawn only where it can be taken.
void Workspace::moveTabToOtherPane(int paneIndex, int tabIndex)
{
    PaneState *pane = paneAtIndex(paneIndex);
    PaneState *destination = otherPane(paneIndex);
    if (!pane || !destination || pane->tabCount() < 2)
        return;
    TabState *from = pane->tabList().value(tabIndex);
    if (!from)
        return;
    if (TabState *tab = destination->addTab(from->kind()))
        copyView(*from, tab);
    pane->closeTab(tabIndex);
    setFocusedPane(m_panes.indexOf(destination));
    emit currentTabChanged();
}

PaneState *Workspace::otherPane(int paneIndex) const
{
    if (m_panes.size() < 2 || paneIndex < 0 || paneIndex >= m_panes.size())
        return nullptr;
    return m_panes.at((paneIndex + 1) % m_panes.size());
}

void Workspace::closeTab(int paneIndex, int tabIndex)
{
    if (paneIndex < 0 || paneIndex >= m_panes.size())
        return;
    m_panes.at(paneIndex)->closeTab(tabIndex);
}

void Workspace::closeCurrentTab()
{
    PaneState *pane = paneAtIndex(m_focused);
    if (pane)
        pane->closeTab(pane->currentIndex());
}

void Workspace::stepTab(int delta)
{
    PaneState *pane = paneAtIndex(m_focused);
    if (pane)
        pane->stepTab(delta);
}

void Workspace::selectTab(int index)
{
    PaneState *pane = paneAtIndex(m_focused);
    if (pane)
        pane->setCurrentIndex(index);
}

void Workspace::splitRight(const QString &kind)
{
    const int insertAt = qMin(m_focused + 1, m_panes.size());
    adopt(new PaneState(kind.isEmpty() ? QStringLiteral("files") : kind, this), insertAt);
    m_focused = insertAt;
    emit panesChanged();
    emit focusedPaneChanged();
    emit currentTabChanged();
}

void Workspace::closePane(int paneIndex)
{
    if (m_panes.size() <= 1)
        return;
    if (paneIndex < 0 || paneIndex >= m_panes.size())
        return;
    PaneState *pane = m_panes.takeAt(paneIndex);
    pane->deleteLater();
    if (m_focused >= m_panes.size())
        m_focused = m_panes.size() - 1;
    emit panesChanged();
    emit focusedPaneChanged();
    emit currentTabChanged();
}

// Clamped rather than wrapped: panes sit side by side on screen, so running off
// the right edge and reappearing on the left would not read as movement.
void Workspace::stepPane(int delta)
{
    if (m_panes.size() < 2 || delta == 0)
        return;
    setFocusedPane(qBound(0, m_focused + delta, m_panes.size() - 1));
}

} // namespace omafiles
