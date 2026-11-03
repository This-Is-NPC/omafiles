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
    property var groups: []

    property OverlayScroll scroll: null

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
                    preferredHeight: columns.implicitHeight + heading.height + 3 * Theme.inset
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
                        anchors.margins: Theme.inset
                        anchors.topMargin: heading.height + 2 * Theme.inset
                        contentHeight: columns.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds

                        Flow {
                            id: columns
                            readonly property int across: width >= 1000 ? 5 : width >= 700 ? 4 : width >= 500 ? 3 : width >= 300 ? 2 : 1
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

                                    Label {
                                        quiet: true
                                        text: group.spec.title
                                        bottomPadding: Theme.spacingXs
                                    }

                                    Repeater {
                                        model: group.spec.keys

                                        Row {
                                            id: line
                                            required property var modelData
                                            readonly property var binding: line.modelData
                                            width: group.width
                                            spacing: Theme.spacingS

                                            Row {
                                                id: caps
                                                anchors.verticalCenter: parent.verticalCenter
                                                spacing: Theme.spacingXs
                                                Repeater {
                                                    model: line.binding.key.split(" ")
                                                    KeyCap {
                                                        required property string modelData
                                                        key: modelData
                                                    }
                                                }
                                            }
                                            Label {
                                                objectName: "helpEntry"
                                                anchors.verticalCenter: parent.verticalCenter
                                                width: line.width - caps.width - line.spacing
                                                text: line.binding.label
                                                // Reference entries wrap so every binding remains readable.
                                                elide: Text.ElideNone
                                                wrapMode: Text.WordWrap
                                            }
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
