#include "Workspace.h"

#include <QDir>
#include <QFileInfo>

namespace omafiles {

TabState::TabState(QString kind, QObject *parent)
    : QObject(parent)
    , m_kind(std::move(kind))
    , m_cwd(QDir::homePath())
{
}

QString TabState::title() const
{
    if (m_kind == QLatin1String("graph"))
        return QStringLiteral("Graph");
    if (!m_selectedTag.isEmpty())
        return QStringLiteral("#") + m_selectedTag;
    const QString name = QFileInfo(m_cwd).fileName();
    return name.isEmpty() ? m_cwd : name;
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
