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

    width: 26
    height: 26
    radius: Theme.rounding
    color: !button.usable ? "transparent"
         : button.on ? Theme.fill(Theme.accent, 0.18)
         : button.hovered ? button.hoverChromeFill
                         : "transparent"
    border.width: 1
    border.color: button.on ? Theme.accent
                 : button.hovered ? button.hoverChromeBorder
                                 : "transparent"

    Icon {
        anchors.centerIn: parent
        kind: button.iconKind
        ink: button.on || button.hovered || button.activeFocus ? Theme.foreground : Theme.dim
    }
}
