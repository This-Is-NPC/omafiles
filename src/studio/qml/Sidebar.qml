pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import omafiles

// The left-hand panel: where to go. Places, removable devices, tags and saved
// views, one panel for the window that acts on the focused pane.
Rectangle {
    id: sidebar

    property var tab: null
    property var entry: ({})
    /// True while the keyboard is walking the tag list rather than the files.
    property bool walking: false
    /// The same, for the places.
    property bool walkingPlaces: false
    /// The same, for the saved views.
    property bool walkingViews: false
    property alias tagsList: tagsListView
    property alias viewsList: viewsListView
    property alias placesList: placesGrid
    readonly property Item tagFind: tagFindField.focusTarget
    property alias devicesList: devices.list
    readonly property Item devicesFocusTarget: devices.focusTarget
    property bool walkingDevices: false
    signal deviceMenuRequested(string id, real x, real y)
    signal deviceActivated(string id)
    property bool expanded: false
    readonly property Item tagsFocusTarget: tagsListView.count > 0 ? tagsListView : tagsHeading
    readonly property Item viewsFocusTarget: viewsListView.count > 0 ? viewsListView : viewsHeading
    signal placeActivated(var place)
    signal tagActivated(string tag)
    signal tagMenuRequested(int row, real x, real y)
    signal tagEditRequested()
    signal viewActivated(int row)
    signal viewMenuRequested(int row, real x, real y)
    signal keyRequested(var event)
    /// Escape in the tag field: back to the files, as from the list.
    signal leaveRequested()

    /// The cursor row's tags, which the tag rows draw in DemiBold.
    readonly property var entryTags: entry && entry.tags ? entry.tags : []
    /// The places, read again whenever the tab moves, so a folder made or
    /// removed since shows the next time.
    property var places: Workspace.places()
    Connections {
        target: sidebar.tab
        function onCwdChanged() { sidebar.places = Workspace.places() }
    }

    color: Theme.chrome

    /// The track of a tag's count bar; the bar is count / largest count of it.
    readonly property real countBarWidth: 4 * Theme.spacingL

    function focusList(list) {
        if (!list.activeFocus) return
        if (list.currentIndex < 0) list.currentIndex = 0
        revealSelection()
    }

    // A model reset keeps a cursor past the rows it left: back to the last one.
    function keepCursor(list) {
        if (list.currentIndex >= list.count) list.currentIndex = list.count - 1
    }

    function focusDevices() {
        devices.hold()
    }

    function focusTagFind() {
        tagFindField.focusEntry()
    }

    function revealSelection() {
        const list = walking ? tagsListView : walkingViews ? viewsListView
                   : walkingDevices ? devices.list : walkingPlaces ? placesGrid : null
        const section = walking ? tagsSection : walkingDevices ? devices
                      : walkingPlaces ? placesSection : viewsSection
        if (!list)
            return
        if (list.currentIndex >= 0)
            list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        scroll.revealRect(section, Qt.rect(0, 0, section.width, section.height))
    }
    onWalkingChanged: Qt.callLater(revealSelection)
    onWalkingViewsChanged: Qt.callLater(revealSelection)
    onWalkingDevicesChanged: Qt.callLater(revealSelection)
    onWalkingPlacesChanged: Qt.callLater(revealSelection)

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

            // Where the tree starts: home, the desktop's user folders that
            // exist, and the Trash, as buttons two to a row.
            Column {
                id: placesSection
                width: parent.width
                spacing: Theme.spacingXs

                SectionHeading {
                    id: placesHeading
                    objectName: "placesHeading"
                    width: parent.width
                    target: placesGrid
                    text: "Places"
                    key: "p"
                }

                GridView {
                    id: placesGrid
                    objectName: "sidebarPlaces"
                    x: Theme.inset
                    width: parent.width - 2 * Theme.inset
                    height: Math.ceil(count / 2) * cellHeight
                    cellWidth: width / 2
                    cellHeight: Theme.controlHeight + Theme.spacingXs
                    interactive: false
                    keyNavigationEnabled: false
                    highlightMoveDuration: 0
                    activeFocusOnTab: true
                    model: sidebar.places
                    currentIndex: -1
                    Keys.onPressed: function (event) { sidebar.keyRequested(event) }
                    onActiveFocusChanged: sidebar.focusList(placesGrid)
                    onCountChanged: sidebar.keepCursor(placesGrid)
                    Accessible.role: Accessible.List
                    Accessible.name: "Places"

                    delegate: Item {
                        id: place
                        objectName: "placeCell"
                        required property var modelData
                        required property int index
                        readonly property bool onCursor: sidebar.walkingPlaces
                                                         && placesGrid.currentIndex === index
                        width: placesGrid.cellWidth
                        height: placesGrid.cellHeight

                        Chip {
                            id: placeButton
                            objectName: "placeButton"
                            x: place.index % 2 === 0 ? 0 : Theme.spacingXs
                            width: parent.width - Theme.spacingXs
                            height: Theme.controlHeight
                            tabFocusable: false
                            pointerFocus: placesGrid
                            caption: place.modelData.path || "The Trash"
                            // The folder the tab stands in.
                            on: place.modelData.path !== "" && !!sidebar.tab
                                && sidebar.tab.kind === "files" && sidebar.tab.cwd === place.modelData.path
                            Accessible.name: place.modelData.name
                            onClicked: {
                                placesGrid.currentIndex = place.index
                                sidebar.placeActivated(place.modelData)
                            }

                            Icon {
                                id: placeIcon
                                anchors.left: parent.left
                                anchors.leftMargin: Theme.spacingS
                                anchors.verticalCenter: parent.verticalCenter
                                kind: place.modelData.icon
                                ink: Theme.dim
                            }
                            Label {
                                objectName: "placeName"
                                anchors.left: placeIcon.right
                                anchors.leftMargin: Theme.iconGap
                                anchors.right: parent.right
                                anchors.rightMargin: Theme.spacingS
                                anchors.verticalCenter: parent.verticalCenter
                                elide: Text.ElideRight
                                text: place.modelData.name
                            }
                            FocusRing {
                                focused: place.onCursor && placesGrid.activeFocus
                                weight: 1
                                inset: 1
                            }
                        }
                    }
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
                    actions: [
                        Chip {
                            objectName: "editTagsButton"
                            readonly property bool hasRow: !!sidebar.entry
                                && (sidebar.entry.kind === "file" || sidebar.entry.kind === "dir")
                            label: "+ Tag"
                            caption: hasRow ? "Add a tag to the row or marked files" : "Select a file to tag it"
                            key: "t a"
                            onClicked: sidebar.tagEditRequested()
                            usable: hasRow
                        },
                        Chip {
                            objectName: "tagOrderButton"
                            label: IndexModel.tagsByCount ? "by count" : "by name"
                            caption: IndexModel.tagsByCount ? "Order the tags by name" : "Order the tags by count"
                            key: "t s"
                            onClicked: IndexModel.tagsByCount = !IndexModel.tagsByCount
                        },
                        KeyCaps {
                            anchors.verticalCenter: parent.verticalCenter
                            keys: "s"
                        }
                    ]
                }

                Entry {
                    id: tagFindField
                    objectName: "tagFind"
                    x: Theme.inset
                    width: parent.width - 2 * Theme.inset
                    placeholder: "Find a tag"
                    text: IndexModel.tagFilter
                    onTextChanged: {
                        IndexModel.tagFilter = text
                        if (tagsListView.count > 0) tagsListView.currentIndex = 0
                    }
                    // Enter takes the tag the cursor is on, as Enter in the list.
                    onAccepted: {
                        const tag = IndexModel.tagAt(tagsListView.currentIndex)
                        if (tag !== "") sidebar.tagActivated(tag)
                    }
                    onCancelled: sidebar.leaveRequested()
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
                                     Math.max(Theme.rowHeight, scroll.height - tagsHeading.height
                                                               - tagFindField.height - 2 * tagsSection.spacing))
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

                    // The count column is as wide as the largest count, so the
                    // bars beside it line up.
                    TextMetrics {
                        id: countWidth
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                        text: IndexModel.largestTagCount
                    }

                    delegate: PanelRow {
                        id: tagRow
                        objectName: "tagRow"
                        required tag
                        required property int count
                        required property int index
                        readonly property bool chosen: sidebar.tab && sidebar.tab.selectedTag === tag
                        readonly property bool onCursor: sidebar.walking && tagsListView.currentIndex === index

                        width: ListView.view.width - Theme.scrollBarWidth
                        Accessible.role: Accessible.ListItem
                        Accessible.name: tagRow.tag
                        Accessible.description: tagRow.count
                        washed: chosen || onCursor || hovered
                        ringed: onCursor && tagsListView.activeFocus
                        color: chosen ? Theme.cursor
                             : onCursor ? Theme.raised
                                        : Theme.hoverChrome
                        // On the cursor the ring draws the accent edge.
                        border.color: chosen && !ringed ? Theme.accent : color
                        strong: sidebar.entryTags.indexOf(tagRow.tag) >= 0
                        text: tagRow.tag
                        trailing: [
                            Rectangle {
                                objectName: "tagBarTrack"
                                anchors.verticalCenter: parent.verticalCenter
                                width: sidebar.countBarWidth
                                height: Theme.spacingXs
                                radius: Theme.rounding
                                color: Theme.base

                                Rectangle {
                                    objectName: "tagBar"
                                    height: parent.height
                                    radius: Theme.rounding
                                    width: parent.width * tagRow.count / Math.max(1, IndexModel.largestTagCount)
                                    color: Theme.tagColours[tagRow.tag] ?? Theme.dim
                                }
                            },
                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                width: countWidth.advanceWidth
                                horizontalAlignment: Text.AlignRight
                                quiet: true
                                text: tagRow.count
                            }
                        ]

                        RowTapHandler {
                            onClicked: sidebar.tagActivated(tagRow.tag)
                            onContextRequested: function (position) {
                                const at = tagRow.mapToItem(null, position.x, position.y)
                                sidebar.tagMenuRequested(tagRow.index, at.x, at.y)
                            }
                        }
                    }
                }

                // Which rows are bold, and why.
                Label {
                    objectName: "tagsBoldCaption"
                    x: Theme.inset
                    width: parent.width - 2 * Theme.inset
                    visible: sidebar.entryTags.length > 0
                    quiet: true
                    elide: Text.ElideMiddle
                    text: "Bold: on " + (sidebar.entry ? sidebar.entry.name : "")
                }
            }

            // Where you keep coming back to.
            //
            // A folder, a tag and a saved query are one list and not three: each of
            // them is a way of asking that somebody wanted back, and which kind it
            // is only matters at the moment it opens.
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
                    // Its own contents, and never more than a third of the panel,
                    // however many views somebody saves.
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
                        ringed: viewRow.onCursor && viewsListView.activeFocus
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
        }
    }
}
