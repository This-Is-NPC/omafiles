pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The nine rwx bits as `ls -l` spells them, each one a toggle, with the octal
// beside them. It changes nothing itself: `toggled` hands back the bits with
// one flipped, and the owner of the control decides what to run.
Row {
    id: bits

    /// The nine bits, 0644 and the like.
    property int value: 0
    property bool editable: true
    /// The cell the keyboard is on, 0 to 8, or -1 where nothing walks it.
    property int cursor: -1
    readonly property string octal: bits.octalOf(bits.value)
    signal toggled(int value)

    function octalOf(value) {
        return ("00" + value.toString(8)).slice(-3)
    }

    function flip(cell) {
        if (bits.editable && cell >= 0 && cell < 9)
            bits.toggled(bits.value ^ (1 << (8 - cell)))
    }

    spacing: 6

    Repeater {
        model: ["owner", "group", "others"]

        Row {
            id: group
            required property string modelData
            required property int index
            spacing: 1

            Repeater {
                model: ["read", "write", "execute"]

                Rectangle {
                    id: cell
                    required property string modelData
                    required property int index
                    readonly property int at: group.index * 3 + cell.index
                    readonly property bool on: (bits.value & (1 << (8 - cell.at))) !== 0

                    objectName: "modeBit" + cell.at
                    width: 15
                    height: 18
                    radius: Math.max(2, Theme.rounding - 2)
                    color: cell.on && bits.editable ? Theme.fill(Theme.accent, 0.16)
                                                    : "transparent"
                    border.width: 1
                    border.color: bits.cursor === cell.at ? Theme.accent
                                : area.containsMouse ? Theme.fill(Theme.foreground, 0.3)
                                : "transparent"

                    Accessible.role: Accessible.CheckBox
                    Accessible.name: group.modelData + " " + cell.modelData
                    Accessible.checkable: true
                    Accessible.checked: cell.on

                    Text {
                        anchors.centerIn: parent
                        text: cell.on ? "rwx".charAt(cell.index) : "-"
                        color: bits.editable ? Theme.foreground : Theme.dim
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        renderType: Text.NativeRendering
                    }

                    MouseArea {
                        id: area
                        anchors.fill: parent
                        enabled: bits.editable
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: bits.flip(cell.at)
                    }
                }
            }
        }
    }

    Label {
        anchors.verticalCenter: parent.verticalCenter
        text: bits.octal
        color: bits.editable ? Theme.foreground : Theme.dim
        font.pixelSize: 11
    }
}
