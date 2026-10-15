pragma ComponentBehavior: Bound

import QtQuick
import "Focus.js" as Focus
import omafiles

Column {
    id: field

    property alias text: input.text
    property alias editor: input
    property Item nextFocus: null
    property Item previousFocus: null
    property string label: "Tag"
    property var candidates: []
    required property real viewportHeight
    signal scrollKey(var event)
    signal accepted()
    signal cancelled()
    signal reveal(Item item)
    spacing: 4

    readonly property var suggestions: {
        const prefix = field.text.trim().toLowerCase()
        const out = []
        for (const tag of field.candidates) {
            if (tag !== prefix && tag.startsWith(prefix))
                out.push(tag)
            if (out.length === 6)
                break
        }
        return out
    }

    function revealSelection() {
        if (!input.activeFocus)
            return
        if (choices.currentIndex >= 0)
            choices.positionViewAtIndex(choices.currentIndex, ListView.Contain)
        field.reveal(field)
    }
    onHeightChanged: Qt.callLater(revealSelection)
    onWidthChanged: Qt.callLater(revealSelection)

    function focusEntry() { input.forceActiveFocus(); input.selectAll() }
    function complete() {
        if (suggestions.length === 0)
            return false
        input.text = suggestions[Math.max(0, choices.currentIndex)]
        input.forceActiveFocus()
        return true
    }

    Label { id: heading; text: field.label }
    Rectangle {
        width: parent.width
        height: 28
        radius: Theme.rounding
        color: Theme.sunken
        border.width: 1
        border.color: input.activeFocus ? Theme.accent : Theme.fill(Theme.foreground, 0.18)
        TextInput {
            id: input
            persistentSelection: true
            objectName: "tagInput"
            anchors.fill: parent
            anchors.margins: 6
            color: Theme.foreground
            selectionColor: Theme.fill(Theme.accent, 0.35)
            selectedTextColor: Theme.foreground
            font.family: Theme.fontFamily
            font.pixelSize: 12
            clip: true
            activeFocusOnTab: true
            Accessible.name: field.label
            Keys.onBacktabPressed: function (event) { Focus.move(event, field.previousFocus) }
            Keys.onPressed: function (event) {
                if (Focus.modifiedTextNavigation(event))
                    return
                if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp
                        || event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                    field.scrollKey(event)
                    if (!event.accepted)
                        return
                } else if (event.key === Qt.Key_Escape) {
                    field.cancelled()
                } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
                    choices.currentIndex = field.suggestions.length === 0 ? -1
                        : Math.max(0, Math.min(field.suggestions.length - 1,
                            choices.currentIndex + (event.key === Qt.Key_Down ? 1 : -1)))
                } else if (event.key === Qt.Key_Tab && (event.modifiers & Qt.ShiftModifier)) {
                    Focus.move(event, field.previousFocus)
                    return
                } else if (event.key === Qt.Key_Tab && !event.modifiers) {
                    if ((field.text.trim() === "" && choices.currentIndex < 0) || !field.complete()) {
                        Focus.move(event, field.nextFocus)
                        return
                    }
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    if (choices.currentIndex >= 0)
                        field.complete()
                    else
                        field.accepted()
                } else {
                    return
                }
                event.accepted = true
            }
        }
    }
    ListView {
        id: choices
        objectName: "tagSuggestions"
        width: parent.width
        height: input.activeFocus ? Math.min(count * 24,
            Math.max(24, field.viewportHeight - heading.height - 28 - 2 * field.spacing)) : 0
        clip: true
        model: field.suggestions
        currentIndex: -1
        onModelChanged: currentIndex = -1
        onCurrentIndexChanged: Qt.callLater(field.revealSelection)
        highlightMoveDuration: 0
        highlightResizeDuration: 0
        delegate: FocusableControl {
            id: suggestion
            required property string modelData
            required property int index
            width: ListView.view.width
            height: 24
            tabFocusable: false
            color: choices.currentIndex === index ? Theme.fill(Theme.accent, 0.18) : "transparent"
            Accessible.role: Accessible.Button
            Accessible.name: "Complete " + modelData
            onActivated: {
                field.text = suggestion.modelData
                input.forceActiveFocus()
            }
            Label { anchors.fill: parent; anchors.leftMargin: 6; text: suggestion.modelData }
        }
    }
}
