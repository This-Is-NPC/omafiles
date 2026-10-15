pragma ComponentBehavior: Bound

import QtQuick
import "Focus.js" as Focus
import omafiles

Rectangle {
    id: sheet
    objectName: "archiveSheet"
    Accessible.ignored: true
    readonly property Item focusTarget: IndexModel.archive.busy ? cancelButton : destination ? destination.focusTarget : null
    property bool extracting: false
    property var paths: []
    property string format: "zip"
    property bool submitted: false
    property Entry destination: null
    property OverlayScroll scroll: null
    property Item cancelButton: null

    visible: false
    color: Theme.fill(Theme.background, 0.55)
    z: 85

    function openFor(paths, extracting, directory) {
        content.active = true
        sheet.paths = paths
        sheet.extracting = extracting
        format = "zip"
        submitted = false
        destination.text = extracting ? directory : directory + "/" + (paths.length === 1 ? paths[0].substring(paths[0].lastIndexOf("/") + 1) : "Archive") + ".zip"
        scroll.contentY = 0
        visible = true
        destination.focusEntry()
    }
    function close() {
        if (submitted && IndexModel.archive.busy) IndexModel.cancelArchive()
        else visible = false
    }
    function pickFormat(value) {
        if (destination.text.endsWith("." + format))
            destination.text = destination.text.slice(0, -format.length) + value
        format = value
    }
    function submit() {
        if (IndexModel.archive.busy || destination.text.trim() === "") return
        submitted = true
        if (extracting) IndexModel.extract(paths[0], destination.text)
        else IndexModel.compress(paths, destination.text, format)
        if (IndexModel.archive.busy) cancelButton.forceActiveFocus()
    }
    Connections {
        target: IndexModel.archive
        function onFinished(ok, message) {
            if (sheet.submitted) { sheet.submitted = false; sheet.visible = false }
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
                sheet.destination = destination
                sheet.scroll = scroll
                sheet.cancelButton = cancelButton
            }

            MouseArea { anchors.fill: parent; onClicked: sheet.close() }
            FocusScope {
                anchors.fill: parent
                focus: sheet.visible
                Keys.onTabPressed: function (event) { Focus.move(event, sheet.focusTarget, cancelButton) }
                Keys.onBacktabPressed: function (event) { Focus.move(event, cancelButton) }
                Keys.onPressed: function (event) {
                    if (event.key === Qt.Key_Escape) { sheet.close(); event.accepted = true }
                    else scroll.handleKey(event)
                }
                OverlayPanel {
                    preferredWidth: 540
                    preferredHeight: column.implicitHeight + 28
                    Accessible.role: Accessible.Dialog
                    Accessible.name: sheet.extracting ? "Extract archive" : "Compress files"
                    MouseArea { anchors.fill: parent; onClicked: function (mouse) { mouse.accepted = true } }
                    OverlayScroll {
                        id: scroll
                        anchors.fill: parent
                        anchors.margins: 14
                        contentHeight: column.implicitHeight
                        Column {
                            id: column
                            width: parent.width
                            spacing: 12
                            Label { width: parent.width; wrapMode: Text.Wrap; font.bold: true; text: sheet.extracting ? "Extract archive" : "Compress " + sheet.paths.length + " selected entries" }
                            Label { width: parent.width; wrapMode: Text.Wrap; text: sheet.extracting ? "Existing destination folder" : "Archive path (inside an existing folder)" }
                            Entry {
                                id: destination
                                objectName: "archiveDestination"
                                width: parent.width
                                enabled: !IndexModel.archive.busy
                                placeholder: sheet.extracting ? "Destination folder" : "Archive path"
                                nextFocus: sheet.extracting ? cancelButton : zipButton
                                previousFocus: startButton
                                onAccepted: sheet.submit()
                                onCancelled: sheet.close()
                            }
                            Flow {
                                width: parent.width
                                spacing: 8
                                visible: !sheet.extracting
                                Chip {
                                    id: zipButton
                                    objectName: "archiveZip"
                                    enabled: !IndexModel.archive.busy
                                    label: (sheet.format === "zip" ? "✓ " : "") + "zip"
                                    Keys.onTabPressed: function (event) { Focus.move(event, gzipButton, destination.focusTarget) }
                                    Keys.onBacktabPressed: function (event) { Focus.move(event, destination.focusTarget) }
                                    onClicked: sheet.pickFormat("zip")
                                }
                                Chip {
                                    id: gzipButton
                                    objectName: "archiveGzip"
                                    enabled: !IndexModel.archive.busy
                                    label: (sheet.format === "tar.gz" ? "✓ " : "") + "tar.gz"
                                    Keys.onTabPressed: function (event) { Focus.move(event, zstdButton, zipButton) }
                                    Keys.onBacktabPressed: function (event) { Focus.move(event, zipButton) }
                                    onClicked: sheet.pickFormat("tar.gz")
                                }
                                Chip {
                                    id: zstdButton
                                    objectName: "archiveZstd"
                                    enabled: !IndexModel.archive.busy
                                    label: (sheet.format === "tar.zst" ? "✓ " : "") + "tar.zst"
                                    Keys.onTabPressed: function (event) { Focus.move(event, cancelButton, gzipButton) }
                                    Keys.onBacktabPressed: function (event) { Focus.move(event, gzipButton) }
                                    onClicked: sheet.pickFormat("tar.zst")
                                }
                            }
                            Label {
                                width: parent.width
                                wrapMode: Text.WrapAnywhere
                                text: IndexModel.archive.busy ? IndexModel.archive.progress : "Preparation runs in a helper. Existing names use Replace, Skip or Keep both. Completed entries can be undone."
                            }
                            Flow {
                                width: parent.width
                                spacing: 8
                                Chip {
                                    id: cancelButton
                                    objectName: "archiveCancel"
                                    label: IndexModel.archive.busy ? "cancel operation" : "cancel"
                                    Keys.onTabPressed: function (event) { Focus.move(event, IndexModel.archive.busy ? cancelButton : startButton, IndexModel.archive.busy ? cancelButton : sheet.extracting ? destination.focusTarget : zstdButton) }
                                    Keys.onBacktabPressed: function (event) { Focus.move(event, IndexModel.archive.busy ? cancelButton : sheet.extracting ? destination.focusTarget : zstdButton) }
                                    onClicked: sheet.close()
                                }
                                Chip {
                                    id: startButton
                                    objectName: "archiveStart"
                                    enabled: !IndexModel.archive.busy
                                    label: sheet.extracting ? "extract" : "compress"
                                    Keys.onTabPressed: function (event) { Focus.move(event, destination.focusTarget, cancelButton) }
                                    Keys.onBacktabPressed: function (event) { Focus.move(event, cancelButton) }
                                    onClicked: sheet.submit()
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
