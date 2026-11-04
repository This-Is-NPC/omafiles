pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The window's one button look (docs/design.md §3, controls): `raised` with a
// `line` border at rest, `hoverRaised` under the pointer, `cursor` with an
// `accent` border while pressed or on, and dim text while it cannot be used.
//
// Uses FocusableControl for tab focus, keyboard activation, pointer handling,
// and the focus ring.
FocusableControl {
    id: chip

    property string label: ""
    property string caption: ""
    property string key: ""
    property bool on: false
    /// Draws the key as caps after the label, for a control whose key is part
    /// of what it says; otherwise the key is in the tooltip only.
    property bool showsKey: false
    /// The ink of the label and of a drawn mark.
    readonly property color ink: chip.usable ? Theme.foreground : Theme.dim
    signal clicked

    tooltipCaption: chip.caption
    tooltipKey: chip.key

    onActivated: clicked()

    Accessible.role: Accessible.Button
    Accessible.name: label
    Accessible.description: caption
    Accessible.checkable: on
    Accessible.checked: on

    implicitWidth: Math.max(Theme.controlHeight, face.implicitWidth + 2 * Theme.spacingS)
    implicitHeight: Theme.controlHeight
    width: implicitWidth
    height: implicitHeight
    radius: Theme.rounding

    readonly property bool lit: chip.usable && (chip.on || chip.pressed)
    hasChrome: true
    color: chip.lit ? Theme.cursor
         : chip.usable && chip.hovered ? Theme.hoverRaised
                                       : Theme.raised
    border.width: 1
    border.color: chip.lit ? Theme.accent : Theme.line

    Row {
        id: face
        anchors.centerIn: parent
        spacing: Theme.spacingS

        Text {
            objectName: "chipLabel"
            anchors.verticalCenter: parent.verticalCenter
            text: chip.label
            textFormat: Text.PlainText
            color: chip.ink
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontBody
            renderType: Text.NativeRendering
        }
        KeyCaps {
            anchors.verticalCenter: parent.verticalCenter
            visible: chip.showsKey && chip.key !== ""
            keys: chip.key
        }
    }
}
