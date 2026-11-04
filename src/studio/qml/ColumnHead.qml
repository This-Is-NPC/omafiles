pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// A heading in the details table: a click sorts by it, a right-click asks for
// the column menu.
//
// A heading is flush left, or flush right over a numeric column such as `size`,
// as the cells under it are.
//
// Not a Tab stop: sorting has `s` / `S`, and the column menu is in the command
// palette.
Item {
    id: head

    property string label: ""
    property string role: ""
    property string sortRole: ""
    property bool ascending: true
    property bool alignRight: false
    signal picked
    signal menuAsked(real x, real y)

    readonly property bool active: head.role !== "" && head.role === head.sortRole

    Accessible.role: Accessible.ColumnHeader
    Accessible.name: head.label

    Text {
        anchors.fill: parent
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: head.alignRight ? Text.AlignRight : Text.AlignLeft
        text: head.label + (head.active ? (head.ascending ? " ↑" : " ↓") : "")
        color: head.active ? Theme.foreground : Theme.dim
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontCaption
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
