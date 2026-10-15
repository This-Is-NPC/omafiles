pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// A heading in the details table: a click sorts by it, a right-click asks for
// the column menu.
//
// Every heading is flush left, `size` included, and so is every cell under it,
// so no one column starts somewhere the rest do not.
//
// Not a Tab stop: sorting has its chips and `s` / `S`, and the column menu is in
// the command palette.
Item {
    id: head

    property string label: ""
    property string role: ""
    property string sortRole: ""
    property bool ascending: true
    signal picked
    signal menuAsked(real x, real y)

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
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: function (point) { head.menuAsked(point.position.x, point.position.y) }
    }
}
