pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// Every command the window has, by name, with the key beside it.
//
// The id is `sheet` and not `palette` on purpose: every Item in Qt 6 has a
// `palette` grouped property, so an id by that name is one resolution rule away
// from silently meaning the colour palette instead of this object.
Rectangle {
    id: sheet

    // The scrim is visual only; the dialog panel carries the role.
    Accessible.ignored: true

    property var commands: []
    property var returnFocus: function () {}
    signal chosen(string id)

    visible: false
    color: Theme.fill(Theme.background, 0.55)
    z: 80

    function open() {
        query.text = ""
        visible = true
        list.currentIndex = 0
        query.forceActiveFocus()
    }

    function close() {
        visible = false
        returnFocus()
    }

    readonly property var shown: {
        const needle = query.text.trim().toLowerCase()
        if (needle.length === 0)
            return sheet.commands
        return sheet.commands.filter(function (cmd) {
            return cmd.label.toLowerCase().indexOf(needle) >= 0
                || cmd.id.toLowerCase().indexOf(needle) >= 0
        })
    }

    MouseArea {
        anchors.fill: parent
        onClicked: sheet.close()
    }

    Rectangle {
        width: Math.min(520, parent.width - 48)
        height: Math.min(380, 68 + list.contentHeight)
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 80
        radius: Theme.rounding
        color: Theme.panel
        border.width: 1
        border.color: Theme.accent

        Accessible.role: Accessible.Dialog
        Accessible.name: "Commands"
        MouseArea {
            anchors.fill: parent
            onClicked: function (mouse) { mouse.accepted = true }
        }


        Column {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            Rectangle {
                width: parent.width
                height: 28
                radius: Theme.rounding
                color: Theme.sunken
                border.width: 1
                border.color: Theme.fill(Theme.accent, 0.45)

                Text {
                    id: lead
                    anchors.left: parent.left
                    anchors.leftMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    text: ":"
                    color: Theme.accent
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                    Accessible.ignored: true
                }

                Text {
                    anchors.left: lead.right
                    anchors.leftMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    visible: query.text === ""
                    text: "run a command"
                    color: Theme.dim
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                    Accessible.ignored: true
                }

            TextInput {
                id: query
                objectName: "paletteQuery"
                anchors.fill: parent
                anchors.leftMargin: lead.width + 15
                anchors.rightMargin: 9
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.foreground
                selectionColor: Theme.fill(Theme.accent, 0.35)
                selectedTextColor: Theme.foreground
                font.family: Theme.fontFamily
                font.pixelSize: 13
                renderType: Text.NativeRendering
                clip: true
                Accessible.name: "command query"
                Keys.onPressed: function (event) {
                    if (event.key === Qt.Key_Escape) {
                        sheet.close()
                        event.accepted = true
                    // The arrows, and the readline pair. Not `j` and `k`: this
                    // is a text field, and a field that eats two letters is a
                    // field you cannot type `kill` or `json` into. The list
                    // below is walked with the keys every other prompt uses.
                    } else if (event.key === Qt.Key_Down
                               || (event.modifiers === Qt.ControlModifier
                                   && event.key === Qt.Key_N)) {
                        list.incrementCurrentIndex()
                        event.accepted = true
                    } else if (event.key === Qt.Key_Up
                               || (event.modifiers === Qt.ControlModifier
                                   && event.key === Qt.Key_P)) {
                        list.decrementCurrentIndex()
                        event.accepted = true
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        if (list.currentIndex >= 0 && list.currentIndex < sheet.shown.length)
                            sheet.chosen(sheet.shown[list.currentIndex].id)
                        event.accepted = true
                    }
                }
            }
            }

            ListView {
                id: list
                width: parent.width
                height: parent.height - 36
                clip: true
                interactive: false
                model: sheet.shown
                currentIndex: 0
                onModelChanged: currentIndex = 0
                Accessible.role: Accessible.List

                delegate: Rectangle {
                    id: entryRow
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 28
                    radius: Theme.rounding
                    color: list.currentIndex === entryRow.index
                           ? Theme.fill(Theme.accent, 0.18) : "transparent"

                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 9
                        anchors.right: shortcut.left
                        anchors.rightMargin: 8
                        text: entryRow.modelData.label
                    }
                    KeyCap {
                        id: shortcut
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        visible: (entryRow.modelData.key || "") !== ""
                        key: entryRow.modelData.key || ""
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        preventStealing: true
                        onEntered: list.currentIndex = entryRow.index
                        onClicked: {
                            list.currentIndex = entryRow.index
                            sheet.chosen(entryRow.modelData.id)
                        }
                    }

                }
            }
        }
    }
}
