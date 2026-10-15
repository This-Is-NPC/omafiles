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
    property alias tagsList: tagsList
    property alias viewsList: viewsList
    property alias devicesList: devices.list
    readonly property Item devicesFocusTarget: devices.focusTarget
    property bool walkingDevices: false
    signal deviceMenuRequested(string id, real x, real y)
    signal deviceActivated(string id)
    property bool expanded: false
    readonly property Item tagsFocusTarget: tagsList.count > 0 ? tagsList : tagsHeading
    readonly property Item viewsFocusTarget: viewsList.count > 0 ? viewsList : viewsHeading
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
        const list = walking ? tagsList : walkingViews ? viewsList : walkingDevices ? devices.list : null
        const heading = walking ? tagsHeading : walkingDevices ? devices : viewsHeading
        if (!list)
            return
        if (list.currentIndex >= 0)
            list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        scroll.revealRect(heading, Qt.rect(0, 0, heading.width,
            heading.height + column.spacing + list.height))
    }
    onWalkingChanged: Qt.callLater(revealSelection)
    onWalkingViewsChanged: Qt.callLater(revealSelection)
    onWalkingDevicesChanged: Qt.callLater(revealSelection)

    OverlayScroll {
        id: scroll
        objectName: "dockScroll"
        focusOnScroll: true
        anchors.fill: parent
        anchors.margins: 12
        contentHeight: column.implicitHeight
        onHeightChanged: Qt.callLater(dock.revealSelection)
        onWidthChanged: Qt.callLater(dock.revealSelection)
        onContentHeightChanged: Qt.callLater(dock.revealSelection)

    Column {
        id: column
        width: scroll.width
        spacing: 9

        Label { quiet: true; text: "details" }

        Details {
            width: parent.width
            entry: dock.entry
            onTagActivated: function (tag) { dock.tagChipActivated(dock.entry.path, tag) }
        }

        Item { width: 1; height: 2 }

        Rule {}

        Item { width: 1; height: 2 }

        DeviceSection {
            id: devices
            width: parent.width
            maximumHeight: Math.max(32, scroll.height * 0.3)
            onFocusEntered: { dock.focusEntered(); dock.revealSelection() }
            onSelectionChanged: Qt.callLater(dock.revealSelection)
            onKeyRequested: function (event) { dock.keyRequested(event) }
            onActivated: function (id) { dock.deviceActivated(id) }
            onMenuRequested: function (id, x, y) { dock.deviceMenuRequested(id, x, y) }
        }

        // Where you keep coming back to.
        //
        // A folder, a tag and a saved query are one list and not three: each of
        // them is a way of asking that somebody wanted back, and which kind it
        // is only matters at the moment it opens. Above the vocabulary because
        // it is a shorter list somebody chose, where the vocabulary is a long
        // one the index grew.
        // Always here, empty or not: a panel that showed only once something
        // was saved would hide where saving lives from whoever has not yet.
        FocusableControl {
            handCursor: false
            pointerFocus: dock.viewsFocusTarget
            tabFocusable: Views.count === 0
            Accessible.role: Accessible.Grouping
            Accessible.name: "Saved views"
            onActiveFocusChanged: if (activeFocus) dock.focusEntered()
            id: viewsHeading
            width: parent.width
            height: 14

            Label { quiet: true; text: "views" }
            Label {
                anchors.right: parent.right
                quiet: true
                color: dock.walkingViews ? Theme.accent : Theme.dim
                text: dock.walkingViews ? "esc leaves" : "v"
            }
        }

        ListView {
            highlightMoveDuration: 0
            highlightResizeDuration: 0
            reuseItems: true
            id: viewsList
            objectName: "sidebarViews"
            activeFocusOnTab: count > 0
            Keys.onPressed: function (event) { dock.keyRequested(event) }
            onActiveFocusChanged: dock.focusList(viewsList)
            width: parent.width
            // Its own contents, and never more than a third of the panel: the
            // vocabulary underneath has to stay reachable however many views
            // somebody saves.
            height: Math.min(contentHeight, Math.max(25, scroll.height * 0.34))
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
                parent: viewsList
                anchors.fill: parent
                color: "transparent"
                border.width: viewsList.activeFocus ? 1 : 0
                border.color: Theme.accent
                z: 2
            }

            delegate: Rectangle {
                id: viewRow
                objectName: "viewRow"
                required property string name
                required property string kind
                required property string asks
                required property int index
                readonly property bool onCursor: dock.walkingViews
                                                 && viewsList.currentIndex === index

                width: ListView.view.width
                height: 25
                Accessible.role: Accessible.ListItem
                Accessible.name: viewRow.name
                Accessible.description: viewRow.asks
                radius: Theme.rounding
                color: viewRow.onCursor ? Theme.fill(Theme.foreground, 0.10)
                     : viewHover.hovered ? Theme.fill(Theme.foreground, 0.06)
                                         : "transparent"
                border.width: 1
                border.color: viewRow.onCursor ? Theme.fill(Theme.accent, 0.45) : "transparent"

                Icon {
                    id: viewMark
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    // The icon says which of the two it is, so a name that does
                    // not is still readable at a glance.
                    kind: viewRow.kind === "folder" ? "folder" : "tag"
                    ink: Theme.dim
                }
                Label {
                    anchors.left: viewMark.right
                    anchors.leftMargin: 8
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: viewRow.name
                    opacity: 0.85
                }

                HoverHandler { id: viewHover; cursorShape: Qt.PointingHandCursor }
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
            width: parent.width
            visible: Views.count === 0
            quiet: true
            text: "nothing saved yet · m to save one"
        }

        Item { width: 1; height: 2 }

        FocusableControl {
            handCursor: false
            pointerFocus: dock.tagsFocusTarget
            tabFocusable: IndexModel.tagRowCount() === 0
            Accessible.role: Accessible.Grouping
            Accessible.name: "Tags"
            onActiveFocusChanged: if (activeFocus) dock.focusEntered()
            id: tagsHeading
            objectName: "tagsHeading"
            width: parent.width
            height: 24
            visible: dock.showTags

            Label { quiet: true; text: "tags"; anchors.verticalCenter: parent.verticalCenter }
            Chip {
                objectName: "editTagsButton"
                anchors.horizontalCenter: parent.horizontalCenter
                label: "+"
                caption: "Add a tag to the row or marked files"
                key: "t a"
                onClicked: dock.tagEditRequested()
                usable: dock.entry && (dock.entry.kind === "file" || dock.entry.kind === "dir")
            }
            Label {
                anchors.right: parent.right
                quiet: true
                color: dock.walking ? Theme.accent : Theme.dim
                text: dock.walking ? "esc leaves" : "t"
            }
        }

        ListView {
            highlightMoveDuration: 0
            highlightResizeDuration: 0
            reuseItems: true
            id: tagsList
            objectName: "sidebarTags"
            activeFocusOnTab: count > 0
            Keys.onPressed: function (event) { dock.keyRequested(event) }
            onActiveFocusChanged: dock.focusList(tagsList)
            width: parent.width
            visible: dock.showTags
            height: dock.showTags ? Math.min(contentHeight,
                Math.max(25, scroll.height - tagsHeading.height - column.spacing)) : 0
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
                parent: tagsList
                anchors.fill: parent
                color: "transparent"
                border.width: tagsList.activeFocus ? 1 : 0
                border.color: Theme.accent
                z: 2
            }

            delegate: Rectangle {
                id: tagRow
                objectName: "tagRow"
                required property string tag
                required property int count
                required property int index
                readonly property bool chosen: dock.tab && dock.tab.selectedTag === tag
                readonly property bool onCursor: dock.walking && tagsList.currentIndex === index

                width: ListView.view.width
                height: 25
                Accessible.role: Accessible.ListItem
                Accessible.name: tagRow.tag
                radius: Theme.rounding
                color: chosen ? Theme.fill(Theme.accent, 0.16)
                     : onCursor ? Theme.fill(Theme.foreground, 0.10)
                     : tagHover.hovered ? Theme.fill(Theme.foreground, 0.06)
                                        : "transparent"
                border.width: 1
                border.color: chosen ? Theme.fill(Theme.accent, 0.55)
                            : onCursor ? Theme.fill(Theme.accent, 0.45)
                                       : "transparent"

                Icon {
                    id: tagMark
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    kind: "tag"
                    ink: tagRow.chosen ? Theme.accent : Theme.dim
                }
                Label {
                    anchors.left: tagMark.right
                    anchors.leftMargin: 8
                    anchors.right: tagCount.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: tagRow.tag
                    opacity: tagRow.chosen ? 1 : 0.85
                }
                Label {
                    id: tagCount
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    quiet: true
                    text: tagRow.count
                }

                HoverHandler { id: tagHover; cursorShape: Qt.PointingHandCursor }
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
