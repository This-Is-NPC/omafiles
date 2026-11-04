pragma ComponentBehavior: Bound

import QtQuick
import "Focus.js" as Focus
import omafiles

// The one text box in the window. The path bar and the filter bar are both
// this, rather than two bare TextInputs styled separately and free to drift;
// where it appears is the caller's business.
//
// A field's focus ring lies on its frame, inside it: a field fills the width
// of the sheet or bar it sits in, where a ring outside it would be cut.
Rectangle {
    id: box

    readonly property Item focusTarget: entry
    property alias text: entry.text
    property Item nextFocus: null
    property Item previousFocus: null
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

    implicitHeight: Theme.controlHeight
    radius: Theme.rounding
    color: Theme.base
    border.width: 1
    border.color: Theme.line

    FocusRing {
        focused: entry.activeFocus
        inset: Theme.focusRing
    }

    Text {
        id: leadMark
        visible: box.lead !== ""
        anchors.left: parent.left
        anchors.leftMargin: Theme.spacingS
        anchors.verticalCenter: parent.verticalCenter
        text: box.lead
        color: Theme.accent
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
        renderType: Text.NativeRendering
    }

    TextInput {
        id: entry
        activeFocusOnTab: true
        Keys.onTabPressed: function (event) { Focus.move(event, box.nextFocus, box.previousFocus) }
        Keys.onBacktabPressed: function (event) { Focus.move(event, box.previousFocus) }
        persistentSelection: true
        anchors.fill: parent
        anchors.leftMargin: box.lead === "" ? Theme.spacingS : leadMark.width + 2 * Theme.spacingS
        anchors.rightMargin: Theme.spacingS
        verticalAlignment: TextInput.AlignVCenter
        clip: true
        color: Theme.foreground
        selectionColor: Theme.cursor
        selectedTextColor: Theme.foreground
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
        renderType: Text.NativeRendering

        Accessible.role: Accessible.EditableText
        Accessible.name: box.placeholder

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: entry.text === "" && !entry.activeFocus
            text: box.placeholder
            color: Theme.dim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontBody
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
