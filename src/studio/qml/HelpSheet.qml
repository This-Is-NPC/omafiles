pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The key map, on the window, under `?`.
//
// A program driven by the keyboard has to be able to say what its keys are
// without sending anyone to a README.
//
// This renders `win.keymap` verbatim, so the sheet cannot drift from it. The
// hint bar and the command palette keep spellings of their own and nothing
// cross-checks the three; a test walks the keymap and requires every key on it
// to do something, which catches the sheet promising what nothing answers but
// not the reverse.
Rectangle {
    id: sheet

    // The scrim is visual only; the dialog panel carries the role.
    Accessible.ignored: true

    property var returnFocus: function () {}
    property var groups: []

    visible: false
    color: Theme.fill(Theme.background, 0.72)
    z: 95

    function open() {
        visible = true
        scope.forceActiveFocus()
    }

    function close() {
        visible = false
        returnFocus()
    }

    MouseArea {
        anchors.fill: parent
        onClicked: sheet.close()
    }

    FocusScope {
        id: scope
        anchors.fill: parent
        focus: sheet.visible

        // Any key closes it. It is a thing you glance at, so needing to
        // remember a second key to dismiss the list of keys would be a joke at
        // the reader's expense.
        Keys.onPressed: function (event) {
            sheet.close()
            event.accepted = true
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(940, parent.width - 48)
            height: Math.min(columns.implicitHeight + 66, parent.height - 64)
            radius: Theme.rounding
            color: Theme.panel
            border.width: 1
            border.color: Theme.accent

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
                anchors.margins: 18
                text: "keys"
                font.bold: true
            }
            Label {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 18
                quiet: true
                text: "any key closes"
            }

            Flickable {
                anchors.fill: parent
                anchors.margins: 18
                anchors.topMargin: heading.height + 26
                contentHeight: columns.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Flow {
                    id: columns
                    readonly property int across: width >= 700 ? 4 : 3
                    width: parent.width
                    spacing: 26

                    Repeater {
                        model: sheet.groups

                        Column {
                            id: group
                            required property var modelData
                            readonly property var spec: group.modelData
                            // Four across when the window is wide enough for
                            // it. Three columns do not fit the map's eight
                            // groups, and the third row falls off the bottom --
                            // a reference that quietly stops at `Tab` is worse
                            // than no reference, because you stop looking.
                            width: (columns.width - (columns.across - 1) * 26)
                                   / columns.across
                            spacing: 6

                            Label {
                                quiet: true
                                text: group.spec.title
                                bottomPadding: 2
                            }

                            Repeater {
                                model: group.spec.keys

                                Row {
                                    id: line
                                    required property var modelData
                                    readonly property var binding: line.modelData
                                    width: group.width
                                    spacing: 7

                                    Row {
                                        id: caps
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 3
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
                                        width: line.width - caps.width - 7
                                        text: line.binding.label
                                        font.pixelSize: 11
                                        // Nothing on this sheet may be cut
                                        // short. Elided, the sort entry reads
                                        // `name, tags, type, size, mo...`,
                                        // which answers with the half you could
                                        // already guess. A reference wraps
                                        // rather than elides.
                                        elide: Text.ElideNone
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }

                            Item { width: 1; height: 6 }
                        }
                    }
                }
            }
        }
    }
}
