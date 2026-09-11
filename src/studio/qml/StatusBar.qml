pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The bottom line: where the cursor is on the left, what to press on the right.
//
// Keys are drawn as keys rather than set as prose -- "j/k move · l open · ..."
// is the right information in a form you have to parse -- and the list changes
// with what is happening instead of naming file keys while you type a filter.
Rectangle {
    id: bar

    property int cursor: -1
    property int total: 0
    property int marked: 0
    property int tagged: 0
    /// Files on the clipboard waiting for a paste, and whether it will move
    /// them. Said here because a cut that nothing shows is a cut you forget
    /// you made, and the next Ctrl+V moves files you had stopped thinking of.
    property int pastable: 0
    property bool cutting: false
    property string tag: ""
    property string filter: ""
    /// What a view with no folder under it says about itself instead of the
    /// folder counts. A graph tab has no FolderModel, so `0 tagged here` there
    /// is not a small number, it is an answer to a question nobody asked.
    property string note: ""
    /// What the window could not do, from whichever model could not do it.
    /// One line, because a message that shares the line with the key hints is
    /// a message that gets clipped.
    property string message: ""
    readonly property bool alarm: message !== ""
    property var hints: []

    readonly property string spoken: {
        const parts = []
        parts.push(bar.total === 0 || bar.cursor < 0 ? "0/" + bar.total
                                                      : (bar.cursor + 1) + "/" + bar.total)
        parts.push(bar.note !== "" ? bar.note : bar.tagged + " tagged here")
        if (bar.marked > 0)
            parts.push(bar.marked + " marked")
        if (bar.pastable > 0)
            parts.push(bar.pastable + (bar.cutting ? " to move" : " to paste"))
        if (bar.tag !== "")
            parts.push("tag " + bar.tag)
        if (bar.filter !== "")
            parts.push("/" + bar.filter)
        if (bar.alarm) {
            parts.push(bar.message)
        } else {
            for (let i = 0; i < bar.hints.length; i++)
                parts.push(bar.hints[i].key + " " + bar.hints[i].label)
        }
        return parts.join(", ")
    }

    height: 26
    color: Theme.panel

    Accessible.role: Accessible.StaticText
    Accessible.name: bar.spoken
    Accessible.description: bar.spoken
    Row {
        id: facts
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 14

        Label {
            quiet: true
            font.pixelSize: 11
            text: bar.total === 0 || bar.cursor < 0
                  ? "0/" + bar.total
                  : (bar.cursor + 1) + "/" + bar.total
        }
        // The header counts the whole index. This counts the folder, because
        // "11 tagged" above a list where nothing is tagged reads as a promise
        // about that list.
        Label {
            objectName: "statusNote"
            quiet: true
            font.pixelSize: 11
            text: bar.note !== "" ? bar.note : bar.tagged + " tagged here"
        }
        Label {
            visible: bar.marked > 0
            color: Theme.accent
            font.pixelSize: 11
            text: bar.marked + " marked"
        }
        Label {
            visible: bar.pastable > 0
            color: Theme.accent
            font.pixelSize: 11
            text: bar.pastable + (bar.cutting ? " to move" : " to paste")
        }
        Label {
            visible: bar.tag !== ""
            color: Theme.accent
            font.pixelSize: 11
            text: "tag " + bar.tag
        }
        Label {
            visible: bar.filter !== ""
            font.pixelSize: 11
            text: "/" + bar.filter
        }
    }

    // The message takes the whole right side when there is one.
    Label {
        visible: bar.alarm
        anchors.left: facts.right
        anchors.leftMargin: 16
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.urgent
        font.pixelSize: 11
        text: bar.message
        horizontalAlignment: Text.AlignRight
    }

    Hints {
        visible: !bar.alarm
        anchors.left: facts.right
        anchors.leftMargin: 16
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        model: bar.hints
    }
}
