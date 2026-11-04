pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import omafiles

// The left-hand panel: where to go. Saved views, removable devices and tags,
// one panel for the window that acts on the focused pane.
Rectangle {
    id: sidebar

    property var tab: null
    property var entry: ({})
    /// True while the keyboard is walking the tag list rather than the files.
    property bool walking: false
    /// The same, for the views above them.
    property bool walkingViews: false
    property alias tagsList: tagsListView
    property alias viewsList: viewsListView
    property alias devicesList: devices.list
    readonly property Item devicesFocusTarget: devices.focusTarget
    property bool walkingDevices: false
    signal deviceMenuRequested(string id, real x, real y)
    signal deviceActivated(string id)
    property bool expanded: false
    readonly property Item tagsFocusTarget: tagsListView.count > 0 ? tagsListView : tagsHeading
    readonly property Item viewsFocusTarget: viewsListView.count > 0 ? viewsListView : viewsHeading
    signal tagActivated(string tag)
    signal tagMenuRequested(int row, real x, real y)
    signal tagEditRequested()
    signal viewActivated(int row)
    signal viewMenuRequested(int row, real x, real y)
    signal keyRequested(var event)

    color: Theme.chrome

    function focusList(list) {
        if (!list.activeFocus) return
        if (list.currentIndex < 0) list.currentIndex = 0
        revealSelection()
    }

    // A model reset keeps a cursor past the rows it left: back to the last one.
    function keepCursor(list) {
        if (list.currentIndex >= list.count) list.currentIndex = list.count - 1
    }

    function revealSelection() {
        const list = walking ? tagsListView : walkingViews ? viewsListView : walkingDevices ? devices.list : null
        const section = walking ? tagsSection : walkingDevices ? devices : viewsSection
        if (!list)
            return
        if (list.currentIndex >= 0)
            list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        scroll.revealRect(section, Qt.rect(0, 0, section.width, section.height))
    }
    onWalkingChanged: Qt.callLater(revealSelection)
    onWalkingViewsChanged: Qt.callLater(revealSelection)
    onWalkingDevicesChanged: Qt.callLater(revealSelection)

    OverlayScroll {
        id: scroll
        objectName: "sidebarScroll"
        focusOnScroll: true
        gutter: 0
        anchors.fill: parent
        anchors.topMargin: Theme.inset
        anchors.bottomMargin: Theme.inset
        contentHeight: column.implicitHeight
        onHeightChanged: Qt.callLater(sidebar.revealSelection)
        onWidthChanged: Qt.callLater(sidebar.revealSelection)
        onContentHeightChanged: Qt.callLater(sidebar.revealSelection)

        // Sections spacingL apart, with no rule: the space separates them.
        // Inside a section the rows sit spacingXs under the heading.
        Column {
            id: column
            width: scroll.contentWidth
            spacing: Theme.spacingL

            // Where you keep coming back to.
            //
            // A folder, a tag and a saved query are one list and not three: each of
            // them is a way of asking that somebody wanted back, and which kind it
            // is only matters at the moment it opens. Above the vocabulary because
            // it is a shorter list somebody chose, where the vocabulary is a long
            // one the index grew.
            // Always here, empty or not: a panel that showed only once something
            // was saved would hide where saving lives from whoever has not yet.
            Column {
                id: viewsSection
                width: parent.width
                spacing: Theme.spacingXs

                SectionHeading {
                    id: viewsHeading
                    objectName: "viewsHeading"
                    width: parent.width
                    target: sidebar.viewsFocusTarget
                    text: "Views"
                    key: "v"
                }

                ListView {
                    highlightMoveDuration: 0
                    highlightResizeDuration: 0
                    reuseItems: true
                    id: viewsListView
                    T.ScrollBar.vertical: ScrollBar {}
                    objectName: "sidebarViews"
                    activeFocusOnTab: count > 0
                    Keys.onPressed: function (event) { sidebar.keyRequested(event) }
                    onActiveFocusChanged: sidebar.focusList(viewsListView)
                    onCountChanged: sidebar.keepCursor(viewsListView)
                    width: parent.width
                    // Its own contents, and never more than a third of the panel: the
                    // vocabulary underneath has to stay reachable however many views
                    // somebody saves.
                    height: Math.min(contentHeight + topMargin + bottomMargin,
                                     Math.max(Theme.rowHeight, scroll.height * 0.34))
                    onCurrentIndexChanged: Qt.callLater(sidebar.revealSelection)
                    visible: Views.count > 0
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: Views
                    keyNavigationEnabled: false
                    currentIndex: -1
                    spacing: 1
                    topMargin: Theme.focusRing
                    bottomMargin: Theme.focusRing
                    Accessible.role: Accessible.List
                    Accessible.name: "Views"

                    FocusRing {
                        parent: viewsListView
                        focused: viewsListView.activeFocus
                        inset: Theme.focusRing
                    }

                    delegate: PanelRow {
                        id: viewRow
                        objectName: "viewRow"
                        required property string name
                        required property string kind
                        required property string asks
                        required property int index
                        readonly property bool onCursor: sidebar.walkingViews
                                                         && viewsListView.currentIndex === index

                        width: ListView.view.width - Theme.scrollBarWidth
                        Accessible.role: Accessible.ListItem
                        Accessible.name: viewRow.name
                        Accessible.description: viewRow.asks
                        washed: viewRow.onCursor || viewRow.hovered
                        color: viewRow.onCursor ? Theme.raised : Theme.hoverChrome
                        // The icon says which of the two it is, so a name that does
                        // not is still readable at a glance.
                        iconKind: viewRow.kind === "folder" ? "folder" : "tag"
                        text: viewRow.name

                        RowTapHandler {
                            onClicked: sidebar.viewActivated(viewRow.index)
                            onContextRequested: function (position) {
                                const at = viewRow.mapToItem(null, position.x, position.y)
                                sidebar.viewMenuRequested(viewRow.index, at.x, at.y)
                            }
                        }
                    }
                }

                // What the list says when there is nothing in it: where they come
                // from, in the words of the two ways to make one.
                Label {
                    objectName: "viewsEmpty"
                    x: Theme.inset
                    width: parent.width - 2 * Theme.inset
                    visible: Views.count === 0
                    quiet: true
                    wrapMode: Text.Wrap
                    text: "nothing saved yet · m to save one"
                }
            }

            DeviceSection {
                id: devices
                width: parent.width
                maximumHeight: Math.max(Theme.rowHeight, scroll.height * 0.3)
                onFocusEntered: sidebar.revealSelection()
                onSelectionChanged: Qt.callLater(sidebar.revealSelection)
                onKeyRequested: function (event) { sidebar.keyRequested(event) }
                onActivated: function (id) { sidebar.deviceActivated(id) }
                onMenuRequested: function (id, x, y) { sidebar.deviceMenuRequested(id, x, y) }
            }

            Column {
                id: tagsSection
                width: parent.width
                spacing: Theme.spacingXs

                SectionHeading {
                    id: tagsHeading
                    objectName: "tagsHeading"
                    width: parent.width
                    target: sidebar.tagsFocusTarget
                    text: "Tags"
                    key: "t"
                    // Drawn whether or not there is a row to tag, so where
                    // tagging lives is never hidden.
                    actions: Chip {
                        objectName: "editTagsButton"
                        readonly property bool hasRow: !!sidebar.entry
                            && (sidebar.entry.kind === "file" || sidebar.entry.kind === "dir")
                        label: "+ Tag"
                        caption: hasRow ? "Add a tag to the row or marked files" : "Select a file to tag it"
                        key: "t a"
                        onClicked: sidebar.tagEditRequested()
                        usable: hasRow
                    }
                }

                ListView {
                    highlightMoveDuration: 0
                    highlightResizeDuration: 0
                    reuseItems: true
                    id: tagsListView
                    T.ScrollBar.vertical: ScrollBar {}
                    // No off-screen delegates built ahead: one still being built when
                    // a reload shrinks the vocabulary is cancelled by a stale index.
                    cacheBuffer: 0
                    objectName: "sidebarTags"
                    activeFocusOnTab: count > 0
                    Keys.onPressed: function (event) { sidebar.keyRequested(event) }
                    onActiveFocusChanged: sidebar.focusList(tagsListView)
                    onCountChanged: sidebar.keepCursor(tagsListView)
                    width: parent.width
                    height: Math.min(contentHeight + topMargin + bottomMargin,
                                     Math.max(Theme.rowHeight, scroll.height - tagsHeading.height - tagsSection.spacing))
                    onCurrentIndexChanged: Qt.callLater(sidebar.revealSelection)
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: IndexModel.tags
                    keyNavigationEnabled: false
                    currentIndex: -1
                    spacing: 1
                    topMargin: Theme.focusRing
                    bottomMargin: Theme.focusRing
                    Accessible.role: Accessible.List
                    Accessible.name: "Tags"

                    FocusRing {
                        parent: tagsListView
                        focused: tagsListView.activeFocus
                        inset: Theme.focusRing
                    }

                    delegate: PanelRow {
                        id: tagRow
                        objectName: "tagRow"
                        required property string tag
                        required property int count
                        required property int index
                        readonly property bool chosen: sidebar.tab && sidebar.tab.selectedTag === tag
                        readonly property bool onCursor: sidebar.walking && tagsListView.currentIndex === index

                        width: ListView.view.width - Theme.scrollBarWidth
                        Accessible.role: Accessible.ListItem
                        Accessible.name: tagRow.tag
                        washed: chosen || onCursor || hovered
                        color: chosen ? Theme.cursor
                             : onCursor ? Theme.raised
                                        : Theme.hoverChrome
                        border.color: chosen ? Theme.accent : color
                        iconKind: "tag"
                        iconInk: tagRow.chosen ? Theme.accent : Theme.dim
                        text: tagRow.tag
                        trailing: Label {
                            anchors.verticalCenter: parent.verticalCenter
                            quiet: true
                            text: tagRow.count
                        }

                        RowTapHandler {
                            onClicked: sidebar.tagActivated(tagRow.tag)
                            onContextRequested: function (position) {
                                const at = tagRow.mapToItem(null, position.x, position.y)
                                sidebar.tagMenuRequested(tagRow.index, at.x, at.y)
                            }
                        }
                    }
                }
            }
        }
    }
}
