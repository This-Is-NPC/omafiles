pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The heading of a panel section: its name in DemiBold caption type at the row
// icons' edge, and at the right edge the section's actions, then the key that
// walks it. Text, not a control: Tab passes it by. It takes the keyboard only
// when its key walks a section with no row to stand on, and a click on it
// sends the keyboard to the section's `target`.
Item {
    id: heading

    property string text: ""
    property string key: ""
    property Item target: heading
    property alias actions: actionRow.data

    height: Theme.headingHeight
    Accessible.role: Accessible.Heading
    Accessible.name: heading.text

    Label {
        objectName: "headingCaption"
        anchors.left: parent.left
        anchors.leftMargin: Theme.inset
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        verticalAlignment: Text.AlignVCenter
        color: Theme.dim
        font.pixelSize: Theme.fontHeading
        font.weight: Font.DemiBold
        text: heading.text
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.inset
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spacingS

        Row {
            id: actionRow
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingXs
        }
        KeyCaps {
            anchors.verticalCenter: parent.verticalCenter
            visible: heading.key !== ""
            keys: heading.key
        }
    }

    TapHandler { onTapped: heading.target.forceActiveFocus() }

    FocusRing {
        focused: heading.activeFocus
        inset: Theme.focusRing
    }
}
