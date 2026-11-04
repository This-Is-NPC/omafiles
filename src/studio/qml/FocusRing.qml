import QtQuick
import QtQuick.Window
import omafiles

// The one focus ring: `weight` px of accent laid just outside a box, by
// default Theme.focusRing. The box is the parent, shrunk by `inset` where the
// parent fills a clipped pane: that region keeps its content `inset` from its
// edge and the ring sits there. A list's cursor row is its own box: a 1 px
// ring on its edge. Hidden after a pointer press until the next key.
Rectangle {
    id: ring

    property bool focused: false
    property real inset: 0
    property real weight: Theme.focusRing

    objectName: "focusRing"
    anchors.fill: parent
    anchors.margins: ring.inset - ring.weight
    radius: Theme.rounding
    color: "transparent"
    border.width: ring.weight
    border.color: Theme.accent
    visible: ring.focused && ring.Window.window !== null
             && !ring.Window.window.PointerActivity.pointing
    z: 3
    Accessible.ignored: true
}
