pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// A Chip that carries a drawn mark instead of a word: the same look, keyboard
// and focus ring, only the contents differ.
Chip {
    id: button

    property string iconKind: ""
    /// The mark's ink: the label's, unless the host marks a chosen one.
    property color iconInk: button.ink

    Accessible.name: caption

    width: Theme.controlHeight
    height: Theme.controlHeight

    Icon {
        anchors.centerIn: parent
        kind: button.iconKind
        ink: button.iconInk
    }
}
