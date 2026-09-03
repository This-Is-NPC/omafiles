pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// A heading in the details table, which is also the control that sorts by it.
//
// Every heading is flush left, `size` included, and so is every cell under it.
// The convention is the other way -- a numeric column and its heading both go
// right, so the digits line up on their units -- but here that reads as one
// column out of five refusing to start where the rest do, and a table you
// notice the alignment of is a table arguing with the names in it.
//
// Not a Tab stop. The five fields already have five chips at the top of the
// pane and two keys (`s` and `S`), all reachable from the keyboard; putting the
// headings in the ring as well would add five more stops that do exactly what
// the five before them did. A heading is the mouse's way in, and the arrow on
// it is how both ways say which field is in charge.
Item {
    id: head

    property string label: ""
    property string role: ""
    property string sortRole: ""
    property bool ascending: true
    signal picked

    readonly property bool active: head.role !== "" && head.role === head.sortRole

    Accessible.role: Accessible.ColumnHeader
    Accessible.name: head.label

    Text {
        anchors.fill: parent
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignLeft
        text: head.label + (head.active ? (head.ascending ? " ↑" : " ↓") : "")
        color: head.active ? Theme.foreground : Theme.dim
        opacity: head.active ? 0.9 : (hover.hovered ? 0.9 : 1)
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 10
        renderType: Text.NativeRendering

        Behavior on color { ColorAnimation { duration: 90 } }
    }

    HoverHandler {
        id: hover
        enabled: head.role !== ""
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        enabled: head.role !== ""
        onTapped: head.picked()
    }
}
