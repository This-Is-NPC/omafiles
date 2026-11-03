pragma ComponentBehavior: Bound

import QtQuick
import "Focus.js" as Focus
import omafiles

// Searchable commands with keyboard and pointer navigation.
Rectangle {
    id: sheet

    // The scrim is visual only; the dialog panel carries the role.
    Accessible.ignored: true

    property point lastPointer: Qt.point(-1, -1)
    property var commands: []
    property TextInput query: null
    property ListView list: null
    readonly property Item focusTarget: sheet.query
    signal chosen(string id)

    visible: false
    color: Theme.fill(Theme.background, 0.55)
    z: 80

    function open() {
        content.active = true
        lastPointer = Qt.point(-1, -1)
        query.text = ""
        visible = true
        list.currentIndex = 0
        query.forceActiveFocus()
    }

    function close() {
        visible = false
    }

    readonly property var shown: {
        const needle = sheet.query ? sheet.query.text.trim().toLowerCase() : ""
        if (needle.length === 0)
            return sheet.commands
        return sheet.commands.filter(function (cmd) {
            return cmd.label.toLowerCase().indexOf(needle) >= 0
                || cmd.id.toLowerCase().indexOf(needle) >= 0
        })
    }

    Loader {
        id: content
        anchors.fill: parent
        active: false
        focus: sheet.visible
        sourceComponent: Item {
            anchors.fill: parent
            Component.onCompleted: { sheet.query = query; sheet.list = list }
            MouseArea {
                anchors.fill: parent
                onClicked: sheet.close()
            }

            OverlayPanel {
                preferredWidth: 520
                readonly property real chrome: 2 * Theme.inset + Theme.controlHeight + Theme.spacingS
                preferredHeight: chrome + list.wholeRows(Math.min(380, parent.height - 2 * margin) - chrome)

                Accessible.role: Accessible.Dialog
                Accessible.name: "Commands"
                MouseArea {
                    anchors.fill: parent
                    onClicked: function (mouse) { mouse.accepted = true }
                }


                Column {
                    anchors.fill: parent
                    anchors.margins: Theme.inset
                    spacing: Theme.spacingS

                    Rectangle {
                        width: parent.width
                        height: Theme.controlHeight
                        radius: Theme.rounding
                        color: Theme.sunken
                        border.width: 1
                        border.color: Theme.fill(Theme.foreground, 0.18)

                        FocusRing {
                            focused: query.activeFocus
                            inset: Theme.focusRing
                        }

                        Text {
                            id: lead
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.spacingS
                            anchors.verticalCenter: parent.verticalCenter
                            text: ":"
                            color: Theme.accent
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontBody
                            renderType: Text.NativeRendering
                            Accessible.ignored: true
                        }

                        Text {
                            anchors.left: lead.right
                            anchors.leftMargin: Theme.spacingS
                            anchors.verticalCenter: parent.verticalCenter
                            visible: query.text === ""
                            text: "run a command"
                            color: Theme.dim
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontBody
                            renderType: Text.NativeRendering
                            Accessible.ignored: true
                        }

                    TextInput {
                        id: query
                        persistentSelection: true
                        objectName: "paletteQuery"
                        anchors.fill: parent
                        anchors.leftMargin: lead.width + 2 * Theme.spacingS
                        anchors.rightMargin: Theme.spacingS
                        verticalAlignment: TextInput.AlignVCenter
                        color: Theme.foreground
                        selectionColor: Theme.fill(Theme.accent, 0.35)
                        selectedTextColor: Theme.foreground
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                        renderType: Text.NativeRendering
                        clip: true
                        Accessible.name: "command query"
                        Keys.onPressed: function (event) {
                            const documentCommand = event.modifiers === Qt.ControlModifier
                                && (event.key === Qt.Key_Home || event.key === Qt.Key_End)
                            if (Focus.modifiedTextNavigation(event) && !documentCommand)
                                return
                            if ((event.key !== Qt.Key_Home && event.key !== Qt.Key_End
                                    || event.modifiers === Qt.ControlModifier) && list.handleKey(event))
                                return
                            if (event.key === Qt.Key_Escape) {
                                sheet.close()
                                event.accepted = true
                            // Readline navigation leaves letters available to the query.
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

                    OverlayList {
                        highlightMoveDuration: 0
                        highlightResizeDuration: 0
                        reuseItems: true
                        id: list
                        width: parent.width
                        height: parent.height - Theme.controlHeight - parent.spacing
                        clip: true
                        model: sheet.visible ? sheet.shown : []
                        currentIndex: 0
                        onModelChanged: currentIndex = count > 0 ? 0 : -1
                        Accessible.role: Accessible.List

                        delegate: Rectangle {
                            id: entryRow
                            required property var modelData
                            required property int index
                            width: ListView.view.width
                            height: Theme.rowHeight
                            radius: Theme.rounding
                            color: list.currentIndex === entryRow.index
                                   ? Theme.fill(Theme.accent, 0.18) : "transparent"

                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: Theme.spacingS
                                anchors.right: shortcut.left
                                anchors.rightMargin: Theme.spacingS
                                text: entryRow.modelData.label
                            }
                            KeyCaps {
                                id: shortcut
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.right: parent.right
                                anchors.rightMargin: Theme.spacingS
                                keys: entryRow.modelData.key || ""
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                onPositionChanged: function (mouse) {
                                    if (mouse.buttons !== Qt.NoButton || list.moving)
                                        return
                                    const here = entryRow.mapToItem(sheet, mouse.x, mouse.y)
                                    const moved = sheet.lastPointer.x >= 0
                                        && (Math.abs(here.x - sheet.lastPointer.x) >= 1
                                            || Math.abs(here.y - sheet.lastPointer.y) >= 1)
                                    sheet.lastPointer = here
                                    if (moved)
                                        list.currentIndex = entryRow.index
                                }
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
    }
}
