pragma ComponentBehavior: Bound

import QtQuick
import "Focus.js" as Focus
import omafiles

// Edits a mode draft for the row or marked files. Apply submits it through chmod.
Rectangle {
    id: sheet

    // The scrim is visual only; the dialog panel carries the role.
    Accessible.ignored: true

    readonly property Item focusTarget: bits && bits.editable ? bits.firstControl : cancelButton
    property var paths: []
    property int value: 0
    /// Why the bits cannot be set from here, or empty where they can.
    property string refusal: ""

    property ModeBits bits: null
    property OverlayScroll scroll: null
    property Item cancelButton: null

    visible: false
    color: Theme.fill(Theme.background, 0.55)
    z: 85

    // The row supplies initial bits; chmod reports each selected file separately.
    function openFor(paths, entry) {
        content.active = true
        sheet.paths = paths
        sheet.value = entry.permissions !== undefined ? entry.permissions : 0
        sheet.refusal = paths.length > 1 ? ""
            : entry.link === true
              ? "a symlink: chmod would change its target, so ask over that instead"
            : entry.mine !== true ? "not yours: only its owner or root may change it"
            : ""
        bits.cursor = 0
        scroll.contentY = 0
        visible = true
        focusTarget.forceActiveFocus()
    }

    function close() {
        visible = false
    }

    function apply() {
        if (sheet.refusal !== "")
            return
        const paths = sheet.paths
        const mode = bits.octal
        sheet.close()
        IndexModel.changeMode(paths, mode)
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
                sheet.bits = bits
                sheet.scroll = scroll
                sheet.cancelButton = cancelButton
            }

            MouseArea {
                anchors.fill: parent
                onClicked: sheet.close()
            }

            FocusScope {
                id: scope
                anchors.fill: parent
                focus: sheet.visible
                Keys.onTabPressed: function (event) { Focus.move(event, bits.editable ? bits.firstControl : cancelButton, cancelButton) }
                Keys.onBacktabPressed: function (event) { Focus.move(event, cancelButton) }

                Keys.onPressed: function (event) {
                    if (scroll.handleKey(event))
                        return
                    const text = event.modifiers ? "" : event.text
                    if (event.key === Qt.Key_Escape)
                        sheet.close()
                    else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                        sheet.apply()
                    else if (event.key === Qt.Key_Space)
                        bits.flip(bits.cursor)
                    else if (event.key === Qt.Key_Left || text === "h")
                        bits.focusCell(bits.cursor - 1)
                    else if (event.key === Qt.Key_Right || text === "l")
                        bits.focusCell(bits.cursor + 1)
                    else
                        return
                    scroll.revealItem(bits)
                    event.accepted = true
                }

                OverlayPanel {
                    preferredWidth: 420
                    preferredHeight: column.implicitHeight + 28

                    Accessible.role: Accessible.Dialog
                    Accessible.name: heading.text
                    MouseArea {
                        anchors.fill: parent
                        onClicked: function (mouse) { mouse.accepted = true }
                    }

                    OverlayScroll {
                        id: scroll
                        focusOnScroll: true
                        Keys.onTabPressed: function (event) { Focus.move(event, bits.editable ? bits.firstControl : cancelButton, cancelButton) }
                        Keys.onBacktabPressed: function (event) { Focus.move(event, cancelButton) }
                        anchors.fill: parent
                        anchors.margins: 14
                        contentHeight: column.implicitHeight
                    Column {
                        id: column
                        width: parent.width
                        spacing: 10

                        Label {
                            id: heading
                            width: parent.width
                            font.bold: true
                            text: sheet.paths.length === 1
                                  ? "permissions of " + String(sheet.paths[0]).split("/").pop()
                                  : "permissions of " + sheet.paths.length + " files"
                        }

                        Flow {
                            width: parent.width
                            spacing: 12
                            Label {
                                quiet: true
                                text: "owner · group · others"
                            }
                            ModeBits {
                                id: bits
                                objectName: "sheetMode"
                                value: sheet.value
                                editable: sheet.refusal === ""
                                nextFocus: applyButton
                                previousFocus: cancelButton
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

                        Flow {
                            width: parent.width
                            spacing: 8
                            Chip {
                                id: applyButton
                                objectName: "modeApply"
                                Keys.onTabPressed: function (event) { Focus.move(event, cancelButton, bits.lastControl) }
                                Keys.onBacktabPressed: function (event) { Focus.move(event, bits.lastControl) }
                                label: "apply"
                                key: "Enter"
                                usable: sheet.refusal === ""
                                onClicked: sheet.apply()
                            }
                            Chip {
                                id: cancelButton
                                objectName: "modeCancel"
                                Keys.onTabPressed: function (event) { Focus.move(event, bits.editable ? bits.firstControl : cancelButton, applyButton) }
                                Keys.onBacktabPressed: function (event) { Focus.move(event, applyButton) }
                                label: "cancel"
                                key: "Esc"
                                quiet: true
                                onClicked: sheet.close()
                            }
                            Label {
                                quiet: true
                                text: "Tab moves · Enter / Space flips · Apply saves"
                            }
                        }
                    }
                    }
                }
            }
        }
    }
}
