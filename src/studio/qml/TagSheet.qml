pragma ComponentBehavior: Bound

import QtQuick
import "Focus.js" as Focus
import omafiles

Rectangle {
    id: sheet
    objectName: "tagSheet"
    property var paths: []
    property var vocabulary: []
    property var existing: []
    property string operation: "add"
    property string refusal: ""
    property bool waiting: false
    readonly property Item focusTarget: source ? source.editor : null
    property TagField source: null
    property TagField destination: null
    property Item closeButton: null

    visible: false
    z: 85
    color: Theme.fill(Theme.background, 0.55)
    Accessible.ignored: true

    function openFor(paths, operation, tag) {
        if (paths.length === 0 || IndexModel.tagEditing)
            return
        content.active = true
        sheet.paths = paths.slice()
        sheet.existing = IndexModel.tagChoices(paths)
        const words = []
        for (let i = 0; i < IndexModel.tagRowCount(); ++i)
            words.push(IndexModel.tagAt(i))
        sheet.vocabulary = words
        sheet.operation = operation
        source.text = tag || ""
        destination.text = ""
        sheet.refusal = ""
        sheet.waiting = false
        sheet.visible = true
        source.focusEntry()
    }
    function close() {
        sheet.visible = false
    }
    function pickOperation(operation) {
        sheet.operation = operation
        sheet.refusal = ""
        Qt.callLater(source.focusEntry)
    }
    function apply() {
        if (sheet.waiting || source.text === ""
                || (sheet.operation === "rename" && destination.text === ""))
            return
        closeButton.forceActiveFocus()
        sheet.waiting = true
        sheet.refusal = ""
        if (sheet.operation === "add")
            IndexModel.tagAdd(sheet.paths, source.text)
        else if (sheet.operation === "remove")
            IndexModel.tagRemove(sheet.paths, source.text)
        else
            IndexModel.tagRename(sheet.paths, source.text, destination.text)
    }
    Connections {
        target: IndexModel
        function onTagEditFinished(ok, receipt) {
            if (!sheet.waiting)
                return
            sheet.waiting = false
            if (!sheet.visible)
                return
            if (ok)
                sheet.close()
            else {
                sheet.refusal = receipt
                sheet.source.focusEntry()
            }
        }
    }
    Loader {
        id: content
        objectName: "sheetContentLoader"
        anchors.fill: parent
        active: false
        focus: sheet.visible
        sourceComponent: Item {
            anchors.fill: parent
            Component.onCompleted: {
                sheet.source = source
                sheet.destination = destination
                sheet.closeButton = closeButton
            }

            MouseArea { anchors.fill: parent; onClicked: sheet.close() }
            FocusScope {
                anchors.fill: parent
                focus: sheet.visible
                Keys.onEscapePressed: function (event) { sheet.close(); event.accepted = true }
                OverlayPanel {
                    preferredWidth: 460
                    preferredHeight: column.implicitHeight + 28
                    Accessible.role: Accessible.Dialog
                    Accessible.name: "Edit tags"
                    MouseArea { anchors.fill: parent; onClicked: function (mouse) { mouse.accepted = true } }
                    OverlayScroll {
                        id: scroll
                        focusOnScroll: true
                        anchors.fill: parent
                        anchors.margins: 14
                        contentHeight: column.implicitHeight
                        Keys.onTabPressed: function (event) { Focus.move(event, modes.controls[0] || null, closeButton) }
                        Keys.onBacktabPressed: function (event) { Focus.move(event, closeButton) }
                        clip: true
                        Column {
                            id: column
                            width: parent.width
                            spacing: 10
                            Label {
                                width: parent.width
                                font.bold: true
                                text: sheet.paths.length === 1
                                    ? "Tags on " + String(sheet.paths[0]).split("/").pop()
                                    : "Tags on " + sheet.paths.length + " selected files"
                            }
                            Row {
                                spacing: 8
                                enabled: !sheet.waiting
                                Repeater {
                                    id: modes
                                    property var controls: []
                                    onItemAdded: function (index, item) {
                                        const next = controls.slice()
                                        next[index] = item
                                        controls = next
                                    }
                                    model: ["add", "remove", "rename"]
                                    Chip {
                                        required property string modelData
                                        required property int index
                                        objectName: "tagMode-" + modelData
                                        Keys.onTabPressed: function (event) { Focus.move(event, index < 2 ? modes.controls[index + 1] || null : source.editor, index > 0 ? modes.controls[index - 1] || null : closeButton) }
                                        Keys.onBacktabPressed: function (event) { Focus.move(event, index > 0 ? modes.controls[index - 1] || null : closeButton) }
                                        label: modelData.charAt(0).toUpperCase() + modelData.slice(1)
                                        on: sheet.operation === modelData
                                        onClicked: sheet.pickOperation(modelData)
                                    }
                                }
                            }
                            TagField {
                                id: source
                                objectName: "tagSource"
                                width: parent.width
                                viewportHeight: scroll.height
                                enabled: !sheet.waiting
                                onScrollKey: function (event) { scroll.handleKey(event) }
                                label: sheet.operation === "add" ? "New or existing tag" : "Existing tag"
                                candidates: sheet.operation === "add" ? sheet.vocabulary : sheet.existing
                                nextFocus: sheet.operation === "rename" ? destination.editor : applyButton
                                previousFocus: modes.controls[2] || null
                                onAccepted: {
                                    if (sheet.operation === "rename")
                                        destination.focusEntry()
                                    else
                                        sheet.apply()
                                }
                                onCancelled: sheet.close()
                                onReveal: function (item) { scroll.revealItem(item) }
                            }
                            TagField {
                                id: destination
                                objectName: "tagDestination"
                                width: parent.width
                                visible: sheet.operation === "rename"
                                viewportHeight: scroll.height
                                onScrollKey: function (event) { scroll.handleKey(event) }
                                enabled: !sheet.waiting
                                label: "Replacement tag"
                                candidates: sheet.vocabulary
                                nextFocus: applyButton
                                previousFocus: source.editor
                                onAccepted: sheet.apply()
                                onCancelled: sheet.close()
                                onReveal: function (item) { scroll.revealItem(item) }
                            }
                            Text {
                                width: parent.width
                                text: sheet.waiting ? "Writing tags…" : sheet.refusal
                                visible: text !== ""
                                wrapMode: Text.WrapAnywhere
                                color: sheet.refusal !== "" ? Theme.accent : Theme.dim
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                            Label {
                                width: parent.width
                                text: "↑ ↓ choose · Tab completes · Enter applies · Esc closes"
                                quiet: true
                                font.pixelSize: 10
                            }
                            Row {
                                spacing: 8
                                Chip {
                                    id: applyButton
                                    objectName: "tagApply"
                                    Keys.onTabPressed: function (event) { Focus.move(event, closeButton, sheet.operation === "rename" ? destination.editor : source.editor) }
                                    Keys.onBacktabPressed: function (event) { Focus.move(event, sheet.operation === "rename" ? destination.editor : source.editor) }
                                    label: sheet.operation === "add" ? "Add tag"
                                        : sheet.operation === "remove" ? "Remove tag" : "Rename tag"
                                    usable: !sheet.waiting && source.text !== ""
                                        && (sheet.operation !== "rename" || destination.text !== "")
                                    onClicked: sheet.apply()
                                }
                                Chip {
                                    id: closeButton
                                    objectName: "tagClose"
                                    Keys.onTabPressed: function (event) { Focus.move(event, sheet.waiting ? closeButton : modes.controls[0] || null, sheet.waiting ? closeButton : applyButton) }
                                    Keys.onBacktabPressed: function (event) { Focus.move(event, sheet.waiting ? closeButton : applyButton) }
                                    label: "Close"
                                    onClicked: sheet.close()
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
