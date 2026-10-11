pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// `chmod` over the row or the marked set: the bits, the octal they come to and
// how many files it lands on, and nothing runs until Enter says so.
Rectangle {
    id: sheet

    // The scrim is visual only; the dialog panel carries the role.
    Accessible.ignored: true

    property var returnFocus: function () {}
    property var paths: []
    property int value: 0
    /// Why the bits cannot be set from here, or empty where they can.
    property string refusal: ""

    visible: false
    color: Theme.fill(Theme.background, 0.55)
    z: 85

    // Starts from the bits of `entry`, the row the command was asked over. One
    // row that is a link or not yours is only read out, as in the details panel;
    // over a set the verb answers for each file.
    function openFor(paths, entry) {
        sheet.paths = paths
        sheet.value = entry.permissions !== undefined ? entry.permissions : 0
        sheet.refusal = paths.length > 1 ? ""
            : entry.link === true
              ? "a symlink: chmod would change its target, so ask over that instead"
            : entry.mine !== true ? "not yours: only its owner or root may change it"
            : ""
        bits.cursor = 0
        visible = true
        scope.forceActiveFocus()
    }

    function close() {
        visible = false
        returnFocus()
    }

    function apply() {
        if (sheet.refusal !== "")
            return
        const paths = sheet.paths
        const mode = bits.octal
        sheet.close()
        IndexModel.changeMode(paths, mode)
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
            const text = event.modifiers ? "" : event.text
            if (event.key === Qt.Key_Escape)
                sheet.close()
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                sheet.apply()
            else if (event.key === Qt.Key_Space)
                bits.flip(bits.cursor)
            else if (event.key === Qt.Key_Left || text === "h")
                bits.cursor = Math.max(0, bits.cursor - 1)
            else if (event.key === Qt.Key_Right || text === "l")
                bits.cursor = Math.min(8, bits.cursor + 1)
            event.accepted = true
        }

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 80
            width: Math.min(420, parent.width - 48)
            height: column.implicitHeight + 28
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

            Column {
                id: column
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 14
                spacing: 10

                Label {
                    id: heading
                    width: parent.width
                    font.bold: true
                    text: sheet.paths.length === 1
                          ? "permissions of " + String(sheet.paths[0]).split("/").pop()
                          : "permissions of " + sheet.paths.length + " files"
                }

                Row {
                    spacing: 12
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        quiet: true
                        text: "owner · group · others"
                    }
                    ModeBits {
                        id: bits
                        objectName: "sheetMode"
                        value: sheet.value
                        editable: sheet.refusal === ""
                        onToggled: function (value) { sheet.value = value }
                    }
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    quiet: true
                    text: sheet.refusal !== "" ? sheet.refusal
                        : "sets " + bits.octal + " on "
                          + (sheet.paths.length === 1 ? "it" : sheet.paths.length + " files")
                          + "; setuid, setgid and sticky stay as each file has them"
                }

                Row {
                    spacing: 8
                    Chip {
                        label: "apply"
                        key: "Enter"
                        usable: sheet.refusal === ""
                        onClicked: sheet.apply()
                    }
                    Chip {
                        label: "cancel"
                        key: "Esc"
                        quiet: true
                        onClicked: sheet.close()
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        quiet: true
                        text: "h l move · space flips"
                    }
                }
            }
        }
    }
}
