pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The one text box in the window. The path bar and the filter bar are both
// this, rather than two bare TextInputs styled separately and free to drift;
// where it appears is the caller's business.
//
// The border is the focus ring: for a field, the ring and the frame are the same
// edge, and drawing both would be saying it twice.
Rectangle {
    id: box

    property alias text: entry.text
    property string lead: ""
    property string placeholder: ""
    signal accepted(string text)
    signal cancelled

    // Everything selected, or the first `upTo` characters: a rename offers the
    // stem and leaves the extension, which is the part nobody meant to retype.
    function focusEntry(upTo) {
        entry.forceActiveFocus()
        if (upTo === undefined || upTo < 0 || upTo >= entry.text.length)
            entry.selectAll()
        else
            entry.select(0, upTo)
    }

    implicitHeight: 26
    radius: Theme.rounding
    color: Theme.sunken
    border.width: 1
    border.color: entry.activeFocus ? Theme.accent : Theme.fill(Theme.foreground, 0.18)

    Behavior on border.color { ColorAnimation { duration: 90 } }

    Text {
        id: leadMark
        visible: box.lead !== ""
        anchors.left: parent.left
        anchors.leftMargin: 9
        anchors.verticalCenter: parent.verticalCenter
        text: box.lead
        color: Theme.accent
        font.family: Theme.fontFamily
        font.pixelSize: 12
        renderType: Text.NativeRendering
    }

    TextInput {
        id: entry
        anchors.fill: parent
        anchors.leftMargin: box.lead === "" ? 9 : leadMark.width + 15
        anchors.rightMargin: 9
        verticalAlignment: TextInput.AlignVCenter
        clip: true
        color: Theme.foreground
        selectionColor: Theme.fill(Theme.accent, 0.35)
        selectedTextColor: Theme.foreground
        font.family: Theme.fontFamily
        font.pixelSize: 12
        renderType: Text.NativeRendering

        Accessible.role: Accessible.EditableText
        Accessible.name: box.placeholder

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: entry.text === "" && !entry.activeFocus
            text: box.placeholder
            color: Theme.dim
            font.family: Theme.fontFamily
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }

        Keys.onPressed: function (event) {
            if (event.key === Qt.Key_Escape) {
                box.cancelled()
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                box.accepted(entry.text)
                event.accepted = true
            }
        }
    }
}
