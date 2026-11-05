pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// One row of a panel list: inset | icon | gap | text | trailing, all on the
// row's centre line. The wash stops short of the panel edge and the icon sits
// at the inset, where the section heading above it starts. A row for a tag
// leads with the tag's colour square in place of the icon. The keyboard's
// cursor row carries the list's ring, 1 px on the wash's edge.
Item {
    id: row

    property alias iconKind: mark.kind
    /// The tag the row names; it leads with the tag's colour instead of an icon.
    property string tag: ""
    /// DemiBold text.
    property bool strong: false
    /// Whether the list's ring is on this row.
    property bool ringed: false
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

        FocusRing {
            focused: row.ringed
            weight: 1
            inset: 1
        }
    }

    Icon {
        id: mark
        objectName: "rowIcon"
        visible: row.tag === ""
        anchors.left: parent.left
        anchors.leftMargin: Theme.inset
        anchors.verticalCenter: parent.verticalCenter
        ink: Theme.dim
    }
    Rectangle {
        objectName: "tagSwatch"
        visible: row.tag !== ""
        anchors.centerIn: mark
        width: Theme.spacingS
        height: Theme.spacingS
        radius: Theme.rounding
        color: Theme.tagColours[row.tag] ?? Theme.dim
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
        font.weight: row.strong ? Font.DemiBold : Font.Normal
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
