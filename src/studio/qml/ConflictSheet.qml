pragma ComponentBehavior: Bound

import QtQuick
import "Focus.js" as Focus
import omafiles

Loader {
    id: sheet
    objectName: "conflictSheet"
    active: false
    readonly property ConflictPanel loaded: item as ConflictPanel
    visible: false
    readonly property Item focusTarget: loaded !== null ? loaded.focusTarget : null
    readonly property bool applyAll: loaded !== null && loaded.applyAll

    function openFor(source, destination) {
        active = true
        visible = true
        loaded.openFor(source, destination)
    }
    function close() { if (loaded !== null) loaded.close() }

    Connections {
        target: IndexModel
        function onConflictRequested(from, to) { sheet.openFor(from, to) }
    }

    sourceComponent: Component { ConflictPanel {} }

    component ConflictPanel: Rectangle {
        id: panel
        Accessible.ignored: true
        readonly property Item focusTarget: skipButton
        property string sourcePath: ""
        property string destinationPath: ""
        property bool applyAll: false
        visible: false
        color: Theme.fill(Theme.background, 0.55)
        z: 90

        function openFor(source, destination) {
            sourcePath = source
            destinationPath = destination
            applyAll = false
            scroll.contentY = 0
            visible = true
            focusTarget.forceActiveFocus()
        }

        function choose(policy) {
            visible = false
            sheet.visible = false
            IndexModel.resolveConflict(policy, applyAll)
        }

        function close() { choose("cancel") }


        MouseArea { anchors.fill: parent; onClicked: panel.close() }
        FocusScope {
            anchors.fill: parent
            focus: panel.visible
            Keys.onTabPressed: function (event) { Focus.move(event, skipButton, cancelButton) }
            Keys.onBacktabPressed: function (event) { Focus.move(event, cancelButton) }
            Keys.onPressed: function (event) {
                if (scroll.handleKey(event)) return
                if (event.key === Qt.Key_Escape) {
                    panel.close()
                    event.accepted = true
                }
            }
            OverlayPanel {
                preferredWidth: 540
                preferredHeight: column.implicitHeight + 28
                Accessible.role: Accessible.Dialog
                Accessible.name: "Destination already exists"
                MouseArea { anchors.fill: parent; onClicked: function (mouse) { mouse.accepted = true } }
                OverlayScroll {
                    id: scroll
                    anchors.fill: parent
                    anchors.margins: 14
                    focusOnScroll: true
                    contentHeight: column.implicitHeight
                    Keys.onTabPressed: function (event) { Focus.move(event, skipButton, cancelButton) }
                    Keys.onBacktabPressed: function (event) { Focus.move(event, cancelButton) }
                    Column {
                        id: column
                        width: parent.width
                        spacing: 12
                        Label { width: parent.width; wrapMode: Text.Wrap; font.bold: true; text: "Destination already exists" }
                        Label { width: parent.width; wrapMode: Text.WrapAnywhere; text: "Incoming: " + panel.sourcePath }
                        Label { width: parent.width; wrapMode: Text.WrapAnywhere; text: "Existing: " + panel.destinationPath }
                        Label {
                            width: parent.width
                            wrapMode: Text.Wrap
                            quiet: true
                            text: "Replace keeps the old entry for Undo. Keep both gives the incoming file a numbered name. Nonempty folders cannot be replaced."
                        }
                        Chip {
                            id: allButton
                            objectName: "conflictAll"
                        Accessible.role: Accessible.CheckBox
                        Accessible.checkable: true
                        Accessible.checked: panel.applyAll
                            label: (panel.applyAll ? "✓ " : "") + "apply to all"
                            Keys.onTabPressed: function (event) { Focus.move(event, replaceButton, cancelButton) }
                            Keys.onBacktabPressed: function (event) { Focus.move(event, cancelButton) }
                            onClicked: panel.applyAll = !panel.applyAll
                        }
                        Flow {
                            width: parent.width
                            spacing: 8
                            Chip {
                                id: replaceButton
                                objectName: "conflictReplace"
                                label: "replace"
                                Keys.onTabPressed: function (event) { Focus.move(event, skipButton, allButton) }
                                Keys.onBacktabPressed: function (event) { Focus.move(event, allButton) }
                                onClicked: panel.choose("replace")
                            }
                            Chip {
                                id: skipButton
                                objectName: "conflictSkip"
                                label: "skip"
                                Keys.onTabPressed: function (event) { Focus.move(event, keepButton, replaceButton) }
                                Keys.onBacktabPressed: function (event) { Focus.move(event, replaceButton) }
                                onClicked: panel.choose("skip")
                            }
                            Chip {
                                id: keepButton
                                objectName: "conflictKeep"
                                label: "keep both"
                                Keys.onTabPressed: function (event) { Focus.move(event, cancelButton, skipButton) }
                                Keys.onBacktabPressed: function (event) { Focus.move(event, skipButton) }
                                onClicked: panel.choose("keep")
                            }
                            Chip {
                                id: cancelButton
                                objectName: "conflictCancel"
                                label: "cancel operation"
                                Keys.onTabPressed: function (event) { Focus.move(event, allButton, keepButton) }
                                Keys.onBacktabPressed: function (event) { Focus.move(event, keepButton) }
                                onClicked: panel.close()
                            }
                        }
                    }
                }
            }
        }
    }

}
