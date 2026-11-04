pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// A control, in omarchy's shape: a raised surface with a hairline border.
//
// Two weights. The default carries its border at rest, for a control that has
// to be findable on its own -- `+`, `×`, the palette key. `quiet` keeps only
// the active state, which is what a row of related controls wants.
//
// Uses FocusableControl for tab focus, keyboard activation, pointer handling,
// and the inner focus ring.
FocusableControl {
    id: chip

    property string label: ""
    property string caption: ""
    property string key: ""
    property bool on: false
    property bool quiet: false
    property color role: Theme.accent
    signal clicked

    tooltipCaption: chip.caption
    tooltipKey: chip.key

    opacity: usable ? 1 : 0.35
    onActivated: clicked()

    Accessible.role: Accessible.Button
    Accessible.name: label
    Accessible.description: caption
    Accessible.checkable: on
    Accessible.checked: on

    implicitWidth: Math.max(Theme.controlHeight,
                            captionText.implicitWidth + (quiet ? Theme.spacingM : Theme.spacingL))
    implicitHeight: Theme.controlHeight
    width: implicitWidth
    height: implicitHeight
    radius: Theme.rounding

    hasChrome: !quiet || (chip.usable && (chip.on || chip.hovered))
    color: chip.on && chip.usable ? Theme.cursor
         : !chip.hovered || !chip.usable ? Theme.raised
         : quiet ? Theme.hoverChrome
                 : Theme.hoverRaised

    border.width: 1
    border.color: chip.on && chip.usable ? chip.role : Theme.line

    Text {
        id: captionText
        anchors.centerIn: parent
        text: chip.label
        textFormat: Text.PlainText
        color: Theme.foreground
        // Dimmed at rest so a quiet row reads as one line of text with one word
        // lit, rather than as six equal buttons.
        opacity: chip.on ? 1 : (chip.hovered || chip.activeFocus ? 0.9 : 0.62)
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
        renderType: Text.NativeRendering
        Behavior on opacity { NumberAnimation { duration: 90 } }
    }
}
