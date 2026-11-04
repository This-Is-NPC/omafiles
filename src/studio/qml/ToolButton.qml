pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// A Chip that carries a drawn mark instead of a word. Same states, same
// keyboard, same focus ring -- only the contents differ, so the two never drift
// into looking like controls from two different programs.
//
// Uses FocusableControl for tab focus, keyboard activation, pointer handling,
// and the inner focus ring.
FocusableControl {
    id: button

    property string iconKind: ""
    property string key: ""
    property string caption: ""
    property bool on: false
    signal clicked

    tooltipCaption: button.caption
    tooltipKey: button.key

    opacity: usable ? 1 : 0.3
    onActivated: clicked()

    Accessible.role: Accessible.Button
    Accessible.name: caption

    width: Theme.controlHeight
    height: Theme.controlHeight
    radius: Theme.rounding
    hasChrome: button.usable && (button.on || button.hovered)
    color: button.on ? Theme.cursor : Theme.hoverChrome
    border.width: 1
    border.color: button.on ? Theme.accent : Theme.line

    Icon {
        anchors.centerIn: parent
        kind: button.iconKind
        ink: button.on || button.hovered || button.activeFocus ? Theme.foreground : Theme.dim
    }
}
