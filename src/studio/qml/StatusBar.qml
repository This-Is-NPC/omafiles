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
    /// The receipt of the last action, or what the window could not do, from
    /// whichever model said it. It sits between the facts and the hints and
    /// takes the room of the least important hints; the first hint stays.
    property string message: ""
    property bool editing: false
    readonly property bool alarm: message !== "" && !editing
    property var hints: []
    property alias nextHintFocus: hintStrip.nextFocus
    property alias previousHintFocus: hintStrip.previousFocus
    readonly property Item firstHint: hintStrip.firstControl
    readonly property Item lastHint: hintStrip.lastControl
    signal receiptRequested()
    signal hintRequested(string key)

    readonly property string spoken: {
        const parts = []
        parts.push(bar.total === 0 || bar.cursor < 0 ? "0/" + bar.total
                                                      : (bar.cursor + 1) + "/" + bar.total)
        if (bar.marked > 0)
            parts.push(bar.marked + " marked")
        parts.push(bar.note !== "" ? bar.note : bar.tagged + " tagged here")
        if (bar.pastable > 0)
            parts.push(bar.pastable + (bar.cutting ? " to move" : " to paste"))
        if (bar.tag !== "")
            parts.push("tag " + bar.tag)
        if (bar.filter !== "")
            parts.push("/" + bar.filter)
        if (bar.alarm)
            parts.push(bar.message)
        for (let i = 0; i < bar.hints.length; i++)
            parts.push(bar.hints[i].key + " " + bar.hints[i].label)
        return parts.join(", ")
    }

    height: Theme.stripHeight
    color: Theme.chrome

    Accessible.role: Accessible.StaticText
    Accessible.name: bar.spoken
    Accessible.description: bar.spoken
    Row {
        id: facts
        objectName: "statusFacts"
        readonly property real room: bar.editing ? 0 : Math.max(0, bar.width * 0.35 - Theme.spacingXl)
        // The facts that are drawn: from the first, those that fit whole in
        // the room. The first is always drawn, and elides where it does not.
        readonly property var drawn: {
            const out = []
            let used = 0
            for (const fact of [position, marks, note, paste, chosenTag, filterText]) {
                if (!fact.wanted)
                    continue
                used += (out.length > 0 ? spacing : 0) + fact.implicitWidth
                if (out.length > 0 && used > room)
                    break
                out.push(fact)
            }
            return out
        }
        anchors.left: parent.left
        anchors.leftMargin: Theme.inset
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spacingM
        visible: !bar.editing
        width: Math.min(implicitWidth, room)

        Label {
            id: position
            readonly property bool wanted: true
            visible: facts.drawn.indexOf(position) >= 0
            width: Math.min(implicitWidth, facts.room)
            elide: Text.ElideRight
            quiet: true
            text: bar.total === 0 || bar.cursor < 0
                  ? "0/" + bar.total
                  : (bar.cursor + 1) + "/" + bar.total
        }
        // The header counts the whole index. This counts the folder, because
        // "11 tagged" above a list where nothing is tagged reads as a promise
        // about that list.
        Label {
            id: marks
            objectName: "statusMarked"
            readonly property bool wanted: bar.marked > 0
            visible: facts.drawn.indexOf(marks) >= 0
            color: Theme.accent
            font.pixelSize: Theme.fontCaption
            text: bar.marked + " marked"
        }
        Label {
            id: note
            objectName: "statusNote"
            readonly property bool wanted: true
            visible: facts.drawn.indexOf(note) >= 0
            quiet: true
            text: bar.note !== "" ? bar.note : bar.tagged + " tagged here"
        }
        Label {
            id: paste
            readonly property bool wanted: bar.pastable > 0
            visible: facts.drawn.indexOf(paste) >= 0
            color: Theme.accent
            font.pixelSize: Theme.fontCaption
            text: bar.pastable + (bar.cutting ? " to move" : " to paste")
        }
        Label {
            id: chosenTag
            readonly property bool wanted: bar.tag !== ""
            visible: facts.drawn.indexOf(chosenTag) >= 0
            color: Theme.accent
            font.pixelSize: Theme.fontCaption
            text: "tag " + bar.tag
        }
        Label {
            id: filterText
            readonly property bool wanted: bar.filter !== ""
            visible: facts.drawn.indexOf(filterText) >= 0
            font.pixelSize: Theme.fontCaption
            text: "/" + bar.filter
        }
    }

    FocusableControl {
        id: receipt
        objectName: "statusReceipt"
        visible: bar.alarm
        anchors.left: facts.right
        anchors.leftMargin: Theme.spacingL
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, Math.min(receiptText.implicitWidth, bar.width - Theme.inset - Theme.spacingL
                                    + 2 * Theme.focusRing - hintStrip.firstWidth - x))
        height: Theme.controlHeight
        tooltipCaption: bar.message
        Accessible.name: "Read receipt: " + bar.message
        onActivated: bar.receiptRequested()
        Label {
            id: receiptText
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            color: Theme.urgent
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
            text: bar.message
        }
    }

    Hints {
        id: hintStrip
        anchors.left: bar.alarm ? receipt.right : facts.right
        anchors.leftMargin: Theme.spacingXl - Theme.focusRing
        anchors.right: parent.right
        anchors.rightMargin: Theme.inset - Theme.focusRing
        anchors.verticalCenter: parent.verticalCenter
        model: bar.hints
        onRequested: function (key) { bar.hintRequested(key) }
    }
}
