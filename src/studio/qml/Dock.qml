pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The right-hand panel: row details, removable devices, saved views and tags.
//
// It lives beside the pane rather than under the tab strip, so it runs the full
// height of the window and no row of the strip's controls sits over it.
Rectangle {
    id: dock

    property var tab: null
    property var entry: ({})
    /// True while the keyboard is walking the tag list rather than the files.
    property bool walking: false
    /// The same, for the views above them.
    property bool walkingViews: false
    /// Whether the tag vocabulary belongs here. On a tags tab it does not: the
    /// list in front of you is already the vocabulary, narrowed, and a second
    /// copy of it beside that one is a second thing to read.
    property bool showTags: true
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
    signal tagChipActivated(string path, string tag)
    signal viewActivated(int row)
    signal viewMenuRequested(int row, real x, real y)
    signal focusEntered()
    signal keyRequested(var event)

    color: Theme.panel

    function focusList(list) {
        if (!list.activeFocus) return
        if (list.currentIndex < 0) list.currentIndex = 0
        focusEntered()
        revealSelection()
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
        objectName: "dockScroll"
        focusOnScroll: true
        anchors.fill: parent
        anchors.topMargin: Theme.inset
        anchors.bottomMargin: Theme.inset
        contentHeight: column.implicitHeight
        onHeightChanged: Qt.callLater(dock.revealSelection)
        onWidthChanged: Qt.callLater(dock.revealSelection)
        onContentHeightChanged: Qt.callLater(dock.revealSelection)

        // Sections spacingL apart with a hairline halfway; inside a section
        // the heading and its rows touch, the heading being a row tall.
        Column {
            id: column
            width: scroll.width
            spacing: Theme.spacingS

            Column {
                width: parent.width

                SectionHeading { width: parent.width; usable: false; text: "details" }

                Details {
                    x: Theme.inset
                    width: parent.width - 2 * Theme.inset
                    entry: dock.entry
                    onTagActivated: function (tag) { dock.tagChipActivated(dock.entry.path, tag) }
                }
            }

            Rule {}

            DeviceSection {
                id: devices
                width: parent.width
                maximumHeight: Math.max(Theme.rowHeight, scroll.height * 0.3)
                onFocusEntered: { dock.focusEntered(); dock.revealSelection() }
                onSelectionChanged: Qt.callLater(dock.revealSelection)
                onKeyRequested: function (event) { dock.keyRequested(event) }
                onActivated: function (id) { dock.deviceActivated(id) }
                onMenuRequested: function (id, x, y) { dock.deviceMenuRequested(id, x, y) }
            }

            Rule {}

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

                SectionHeading {
                    id: viewsHeading
                    objectName: "viewsHeading"
                    width: parent.width
                    pointerFocus: dock.viewsFocusTarget
                    tabFocusable: Views.count === 0
                    Accessible.role: Accessible.Grouping
                    Accessible.name: "Saved views"
                    onActiveFocusChanged: if (activeFocus) dock.focusEntered()
                    text: "views"
                    key: "v"
                    walking: dock.walkingViews
                }

                ListView {
                    highlightMoveDuration: 0
                    highlightResizeDuration: 0
                    reuseItems: true
                    id: viewsListView
                    objectName: "sidebarViews"
                    activeFocusOnTab: count > 0
                    Keys.onPressed: function (event) { dock.keyRequested(event) }
                    onActiveFocusChanged: dock.focusList(viewsListView)
                    width: parent.width
                    // Its own contents, and never more than a third of the panel: the
                    // vocabulary underneath has to stay reachable however many views
                    // somebody saves.
                    height: Math.min(contentHeight, Math.max(Theme.rowHeight, scroll.height * 0.34))
                    onCurrentIndexChanged: Qt.callLater(dock.revealSelection)
                    visible: Views.count > 0
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: Views
                    keyNavigationEnabled: false
                    currentIndex: -1
                    spacing: 1
                    Accessible.role: Accessible.List
                    Accessible.name: "Views"

                    Rectangle {
                        parent: viewsListView
                        anchors.fill: parent
                        color: "transparent"
                        border.width: viewsListView.activeFocus ? 1 : 0
                        border.color: Theme.accent
                        z: 2
                    }

                    delegate: PanelRow {
                        id: viewRow
                        objectName: "viewRow"
                        required property string name
                        required property string kind
                        required property string asks
                        required property int index
                        readonly property bool onCursor: dock.walkingViews
                                                         && viewsListView.currentIndex === index

                        width: ListView.view.width
                        Accessible.role: Accessible.ListItem
                        Accessible.name: viewRow.name
                        Accessible.description: viewRow.asks
                        color: viewRow.onCursor ? Theme.fill(Theme.foreground, 0.10)
                             : viewRow.hovered ? Theme.fill(Theme.foreground, 0.06)
                                               : "transparent"
                        border.color: viewRow.onCursor ? Theme.fill(Theme.accent, 0.45) : "transparent"
                        // The icon says which of the two it is, so a name that does
                        // not is still readable at a glance.
                        iconKind: viewRow.kind === "folder" ? "folder" : "tag"
                        text: viewRow.name
                        textOpacity: 0.85

                        RowTapHandler {
                            onClicked: dock.viewActivated(viewRow.index)
                            onContextRequested: function (position) {
                                const at = viewRow.mapToItem(null, position.x, position.y)
                                dock.viewMenuRequested(viewRow.index, at.x, at.y)
                            }
                        }
                    }
                }

                // What the list says when there is nothing in it: where they come
                // from, in the words of the two ways to make one.
                Label {
                    x: Theme.inset
                    width: parent.width - 2 * Theme.inset
                    visible: Views.count === 0
                    quiet: true
                    text: "nothing saved yet · m to save one"
                }
            }

            Rule { visible: dock.showTags }

            Column {
                id: tagsSection
                width: parent.width
                visible: dock.showTags

                SectionHeading {
                    id: tagsHeading
                    objectName: "tagsHeading"
                    width: parent.width
                    pointerFocus: dock.tagsFocusTarget
                    tabFocusable: IndexModel.tagRowCount() === 0
                    Accessible.role: Accessible.Grouping
                    Accessible.name: "Tags"
                    onActiveFocusChanged: if (activeFocus) dock.focusEntered()
                    text: "tags"
                    key: "t"
                    walking: dock.walking
                    actions: Chip {
                        objectName: "editTagsButton"
                        label: "+"
                        caption: "Add a tag to the row or marked files"
                        key: "t a"
                        onClicked: dock.tagEditRequested()
                        usable: dock.entry && (dock.entry.kind === "file" || dock.entry.kind === "dir")
                    }
                }

                ListView {
                    highlightMoveDuration: 0
                    highlightResizeDuration: 0
                    reuseItems: true
                    id: tagsListView
                    // No off-screen delegates built ahead: one still being built when
                    // a reload shrinks the vocabulary is cancelled by a stale index.
                    cacheBuffer: 0
                    objectName: "sidebarTags"
                    activeFocusOnTab: count > 0
                    Keys.onPressed: function (event) { dock.keyRequested(event) }
                    onActiveFocusChanged: dock.focusList(tagsListView)
                    width: parent.width
                    height: dock.showTags ? Math.min(contentHeight,
                        Math.max(Theme.rowHeight, scroll.height - tagsHeading.height)) : 0
                    onCurrentIndexChanged: Qt.callLater(dock.revealSelection)
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: IndexModel.tags
                    keyNavigationEnabled: false
                    currentIndex: -1
                    spacing: 1
                    Accessible.role: Accessible.List
                    Accessible.name: "Tags"

                    Rectangle {
                        parent: tagsListView
                        anchors.fill: parent
                        color: "transparent"
                        border.width: tagsListView.activeFocus ? 1 : 0
                        border.color: Theme.accent
                        z: 2
                    }

                    delegate: PanelRow {
                        id: tagRow
                        objectName: "tagRow"
                        required property string tag
                        required property int count
                        required property int index
                        readonly property bool chosen: dock.tab && dock.tab.selectedTag === tag
                        readonly property bool onCursor: dock.walking && tagsListView.currentIndex === index

                        width: ListView.view.width
                        Accessible.role: Accessible.ListItem
                        Accessible.name: tagRow.tag
                        color: chosen ? Theme.fill(Theme.accent, 0.16)
                             : onCursor ? Theme.fill(Theme.foreground, 0.10)
                             : hovered ? Theme.fill(Theme.foreground, 0.06)
                                       : "transparent"
                        border.color: chosen ? Theme.fill(Theme.accent, 0.55)
                                    : onCursor ? Theme.fill(Theme.accent, 0.45)
                                               : "transparent"
                        iconKind: "tag"
                        iconInk: tagRow.chosen ? Theme.accent : Theme.dim
                        text: tagRow.tag
                        textOpacity: tagRow.chosen ? 1 : 0.85
                        trailing: Label {
                            anchors.verticalCenter: parent.verticalCenter
                            quiet: true
                            text: tagRow.count
                        }

                        RowTapHandler {
                            onClicked: dock.tagActivated(tagRow.tag)
                            onContextRequested: function (position) {
                                const at = tagRow.mapToItem(null, position.x, position.y)
                                dock.tagMenuRequested(tagRow.index, at.x, at.y)
                            }
                        }
                    }
                }
            }
        }
    }
}
