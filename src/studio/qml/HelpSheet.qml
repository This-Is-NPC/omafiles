pragma ComponentBehavior: Bound

import QtQuick
import "Focus.js" as Focus
import omafiles

// Render the window key map as a scrollable reference.
Rectangle {
    id: sheet
    objectName: "helpSheet"

    // The scrim is visual only; the dialog panel carries the role.
    Accessible.ignored: true

    readonly property Item focusTarget: scroll
    readonly property var hints: [{ key: "Esc", label: "close" }, { key: "PgDn PgUp", label: "page" }, { key: "↑ ↓", label: "scroll" }]
    property var groups: []

    property OverlayScroll scroll: null

    // One row of the sheet: the keys in a column of the section's width, then
    // the description, its first line centred on the caps.
    component HelpLine: Row {
        id: line
        property alias keys: caps.keys
        property alias label: entry.text
        property real keyColumn: 0
        readonly property real keysWidth: caps.implicitWidth
        spacing: Theme.spacingS

        Item {
            width: line.keyColumn
            height: caps.implicitHeight
            KeyCaps {
                id: caps
            }
        }
        Label {
            id: entry
            objectName: "helpEntry"
            width: line.width - line.keyColumn - line.spacing
            topPadding: (Theme.chipHeight - entryMetrics.height) / 2
            // Reference entries wrap so every binding remains readable.
            elide: Text.ElideNone
            wrapMode: Text.WordWrap
            FontMetrics {
                id: entryMetrics
                font: entry.font
            }
        }
    }

    visible: false
    color: Theme.fill(Theme.background, 0.72)
    z: 95

    function open() {
        content.active = true
        scroll.contentY = 0
        visible = true
        scroll.forceActiveFocus()
    }

    function close() {
        visible = false
    }

    Loader {
        id: content
        objectName: "sheetContentLoader"
        anchors.fill: parent
        active: false
        focus: sheet.visible
        sourceComponent: Item {
            anchors.fill: parent
            Component.onCompleted: {
                sheet.scroll = scroll
            }

            MouseArea {
                anchors.fill: parent
                onClicked: sheet.close()
            }

            FocusScope {
                id: scope
                anchors.fill: parent
                focus: sheet.visible

                Keys.onPressed: function (event) {
                    if (scroll.handleKey(event))
                        return
                    if (event.key === Qt.Key_Escape) {
                        sheet.close()
                        event.accepted = true
                    }
                }

                OverlayPanel {
                    preferredWidth: 1140
                    preferredHeight: columns.implicitHeight + heading.height + 3 * Theme.inset - 2 * Theme.focusRing
                    margin: Theme.spacingXl

                    Accessible.role: Accessible.Dialog
                    Accessible.name: heading.text
                    MouseArea {
                        anchors.fill: parent
                        onClicked: function (mouse) { mouse.accepted = true }
                    }

                    Label {
                        id: heading
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: Theme.inset
                        text: "keys"
                        font.bold: true
                        font.pixelSize: Theme.fontTitle
                    }
                    Label {
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.inset
                        quiet: true
                        text: "↑ ↓ PgUp PgDn scroll · Esc closes"
                    }

                    OverlayScroll {
                        id: scroll
                        activeFocusOnTab: true
                        Keys.onTabPressed: function (event) { Focus.move(event, scroll, scroll) }
                        Keys.onBacktabPressed: function (event) { Focus.move(event, scroll) }
                        anchors.fill: parent
                        anchors.margins: Theme.inset - Theme.focusRing
                        anchors.topMargin: heading.height + 2 * Theme.inset - Theme.focusRing
                        contentHeight: columns.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds

                        Flow {
                            id: columns
                            topPadding: Theme.focusRing
                            bottomPadding: Theme.focusRing
                            readonly property int across: width >= 1000 ? 3 : width >= 600 ? 2 : 1
                            width: parent.width
                            spacing: Theme.spacingXl

                            Repeater {
                                model: sheet.visible ? sheet.groups : []

                                Column {
                                    id: group
                                    required property var modelData
                                    readonly property var spec: group.modelData
                                    width: (columns.width - (columns.across - 1) * columns.spacing)
                                           / columns.across
                                    spacing: Theme.spacingXs
                                    // The key column is as wide as the widest keys in the
                                    // section, so every description starts on one edge.
                                    readonly property real keyColumn: {
                                        let widest = 0
                                        for (let i = 0; i < lines.count; ++i) {
                                            const line = lines.itemAt(i) as HelpLine
                                            if (line)
                                                widest = Math.max(widest, line.keysWidth)
                                        }
                                        return widest
                                    }

                                    Label {
                                        quiet: true
                                        text: group.spec.title
                                        bottomPadding: Theme.spacingXs
                                    }

                                    Repeater {
                                        id: lines
                                        model: group.spec.keys

                                        HelpLine {
                                            required property var modelData
                                            width: group.width
                                            keyColumn: group.keyColumn
                                            keys: modelData.key
                                            label: modelData.label
                                        }
                                    }

                                    Item { width: 1; height: Theme.spacingXs }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
