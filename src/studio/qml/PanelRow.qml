pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// One row of a panel list: inset | icon | gap | text | trailing, all on the
// row's centre line. The wash stops short of the panel edge and the icon sits
// at the inset, where the section heading above it starts.
Item {
    id: row

    property alias iconKind: mark.kind
    property alias iconInk: mark.ink
    property alias text: caption.text
    /// Whether the wash draws; it is opaque, so a row without one shows the panel.
    property bool washed: false
    property alias color: wash.color
    property alias border: wash.border
    property alias trailing: trail.data
    /// Where the trailing controls start, for a pointer area that must stop there.
    readonly property alias trailingX: trail.x
    readonly property alias hovered: hover.hovered

    height: Theme.rowHeight

    Rectangle {
        id: wash
        anchors.fill: parent
        anchors.leftMargin: Theme.spacingXs
        anchors.rightMargin: Theme.spacingXs
        radius: Theme.rounding
        visible: row.washed
        color: Theme.chrome
        border.width: 1
        border.color: wash.color
    }

    Icon {
        id: mark
        objectName: "rowIcon"
        anchors.left: parent.left
        anchors.leftMargin: Theme.inset
        anchors.verticalCenter: parent.verticalCenter
        ink: Theme.dim
    }
    Label {
        id: caption
        objectName: "rowText"
        anchors.left: mark.right
        anchors.leftMargin: Theme.iconGap
        anchors.right: trail.left
        anchors.rightMargin: trail.width > 0 ? Theme.spacingS : 0
        anchors.verticalCenter: parent.verticalCenter
        elide: Text.ElideRight
    }
    Row {
        id: trail
        anchors.right: parent.right
        anchors.rightMargin: Theme.inset
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spacingXs
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
}
