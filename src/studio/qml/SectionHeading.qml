pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The heading of a panel section, one row tall: the name on the left at the
// row icons' edge, and on the right the section's actions and the key that
// walks it, every part on the row's centre line.
FocusableControl {
    id: heading

    property string text: ""
    property string key: ""
    /// The keyboard is walking the section, so the key's place says how to leave.
    property bool walking: false
    property alias actions: actionRow.data

    handCursor: false
    height: Theme.rowHeight
    ringInset: Theme.focusRing

    Label {
        objectName: "headingCaption"
        anchors.left: parent.left
        anchors.leftMargin: Theme.inset
        anchors.verticalCenter: parent.verticalCenter
        quiet: true
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
        Label {
            objectName: "headingKey"
            anchors.verticalCenter: parent.verticalCenter
            visible: heading.key !== ""
            quiet: true
            color: heading.walking ? Theme.accent : Theme.dim
            text: heading.walking ? "esc leaves" : heading.key
        }
    }
}
