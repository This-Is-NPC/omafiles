pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The right-hand panel: what the cursor is on, where you keep coming back to,
// and the tag vocabulary.
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
    signal tagActivated(string tag)
    signal viewActivated(int row)
    signal viewMenuRequested(int row, real x, real y)

    color: Theme.panel

    Column {
        id: column
        anchors.fill: parent
        anchors.margins: 12
        spacing: 9

        Label { quiet: true; text: "details" }

        Details {
            width: parent.width
            entry: dock.entry
        }

        Item { width: 1; height: 2 }

        Rule {}

        Item { width: 1; height: 2 }

        // Where you keep coming back to.
        //
        // A folder, a tag and a saved query are one list and not three: each of
        // them is a way of asking that somebody wanted back, and which kind it
        // is only matters at the moment it opens. Above the vocabulary because
        // it is a shorter list somebody chose, where the vocabulary is a long
        // one the index grew.
        // Always here, empty or not: a panel that showed only once something
        // was saved would hide where saving lives from whoever has not yet.
        Item {
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
            id: viewsList
            width: parent.width
            // Its own contents, and never more than a third of the panel: the
            // vocabulary underneath has to stay reachable however many views
            // somebody saves.
            height: Math.min(contentHeight, column.height * 0.34)
            visible: Views.count > 0
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: Views
            keyNavigationEnabled: false
            currentIndex: -1
            spacing: 1
            Accessible.role: Accessible.List
            Accessible.name: "Views"

            delegate: Rectangle {
                id: viewRow
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
                TapHandler {
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onTapped: function (point, button) {
                        if (button === Qt.RightButton) {
                            const at = viewRow.mapToItem(null, point.position.x,
                                                         point.position.y)
                            dock.viewMenuRequested(viewRow.index, at.x, at.y)
                            return
                        }
                        dock.viewActivated(viewRow.index)
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

        Item {
            width: parent.width
            height: 14
            visible: dock.showTags

            Label { quiet: true; text: "tags" }
            Label {
                anchors.right: parent.right
                quiet: true
                color: dock.walking ? Theme.accent : Theme.dim
                text: dock.walking ? "esc leaves" : "t"
            }
        }

        ListView {
            id: tagsList
            width: parent.width
            visible: dock.showTags
            height: dock.showTags ? Math.max(0, column.height - y) : 0
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: IndexModel.tags
            keyNavigationEnabled: false
            currentIndex: -1
            spacing: 1
            Accessible.role: Accessible.List
            Accessible.name: "Tags"

            delegate: Rectangle {
                id: tagRow
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
                TapHandler { onTapped: dock.tagActivated(tagRow.tag) }
            }
        }
    }
}
