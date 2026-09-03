pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The right-hand panel: what the cursor is on, then the tag vocabulary.
//
// It lives beside the pane rather than under the tab strip, so it runs the full
// height of the window. Under the strip, the view and sort controls at the
// right end of that strip sat over the top of it -- two unrelated rows of
// controls in one column, with nothing to say where one ended and the other
// began.
Rectangle {
    id: dock

    property var tab: null
    property var entry: ({})
    /// True while the keyboard is walking the tag list rather than the files.
    property bool walking: false
    property alias tagsList: tagsList
    signal tagActivated(string tag)

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

        Item {
            width: parent.width
            height: 14

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
            height: Math.max(0, column.height - y)
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
