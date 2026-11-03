import QtQuick
import QtQuick.Window
import omafiles

// The one focus ring: Theme.focusRing px of accent laid just outside a box.
// The box is the parent, shrunk by `inset` where the parent fills a clipped
// pane: that region keeps its content `inset` from its edge and the ring sits
// there. Hidden after a pointer press until the next key.
Rectangle {
    id: ring

    property bool focused: false
    property real inset: 0

    objectName: "focusRing"
    anchors.fill: parent
    anchors.margins: ring.inset - Theme.focusRing
    radius: Theme.rounding
    color: "transparent"
    border.width: Theme.focusRing
    border.color: Theme.accent
    visible: ring.focused && ring.Window.window !== null
             && !ring.Window.window.PointerActivity.pointing
    z: 3
    Accessible.ignored: true
}
