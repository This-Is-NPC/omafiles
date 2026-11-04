pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "Focus.js" as Focus
import omafiles

FocusScope {
    id: trash
    objectName: "trashView"
    readonly property TrashModel model: contents
    readonly property Item focusTarget: list
    readonly property int cursor: list.currentIndex
    property string cursorPath: ""
    property int lastCursor: 0
    function restoreCursor() {
        const found = contents.rowForPath(cursorPath)
        list.currentIndex = found >= 0 ? found : Math.min(lastCursor, contents.count - 1)
        if (list.currentIndex >= 0) {
            cursorPath = contents.pathAt(list.currentIndex)
            lastCursor = list.currentIndex
        }
        Qt.callLater(list.revealCurrent)
    }
    signal menuRequested(real x, real y)
    signal emptyRequested()
    signal globalKey(var event)
    signal receiptRequested(string text)

    function showDetails() { if (contents.count > 0) receiptRequested(contents.details(list.currentIndex)) }
    function markCurrent() { contents.toggleMark(list.currentIndex) }
    function restore() {
        if (!IndexModel.operationBusy) contents.restore(list.currentIndex)
    }
    function handleKey(event) {
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier)) return
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) return
        if (list.handleKey(event)) return
        const text = event.text
        if (event.key === Qt.Key_Down || text === "j") list.currentIndex = Math.min(contents.count - 1, list.currentIndex + 1)
        else if (event.key === Qt.Key_Up || text === "k") list.currentIndex = Math.max(0, list.currentIndex - 1)
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) showDetails()
        else if (event.key === Qt.Key_Space) contents.toggleMark(list.currentIndex)
        else if (text === "a") contents.markAll()
        else if (text === "A") contents.clearMarks()
        else if (text === "r") restore()
        else if (text === "e") emptyRequested()
        else if (text === "m" || event.key === Qt.Key_Menu
                 || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) menuRequested(12, 40)
        else return
        event.accepted = true
    }

    TrashModel {
        id: contents
        objectName: "trashModel"
        onOperationFinished: IndexModel.reload()
        onChanged: if (!loading) trash.restoreCursor()
    }

    Keys.onPressed: function (event) {
        event.accepted = false
        trash.handleKey(event)
        if (!event.accepted) trash.globalKey(event)
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.inset
        spacing: Theme.spacingS
        Flow {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            spacing: Theme.spacingS
            Label {
                text: "Trash"
                font.bold: true
                font.pixelSize: Theme.fontTitle
                height: Theme.controlHeight
                verticalAlignment: Text.AlignVCenter
            }
            Chip {
                id: restoreButton
                objectName: "trashRestore"
                label: "restore"
                key: "r"
                usable: contents.count > 0 && !contents.busy && !IndexModel.operationBusy
                onClicked: trash.restore()
                Keys.onTabPressed: function (event) { Focus.move(event, emptyButton, list) }
                Keys.onBacktabPressed: function (event) { Focus.move(event, list) }
            }
            Chip {
                id: emptyButton
                objectName: "trashEmpty"
                label: "empty…"
                key: "e"
                usable: contents.count > 0 && !contents.busy && !IndexModel.operationBusy
                onClicked: trash.emptyRequested()
                Keys.onTabPressed: function (event) { Focus.move(event, refreshButton, restoreButton) }
                Keys.onBacktabPressed: function (event) { Focus.move(event, restoreButton) }
            }
            Chip {
                id: refreshButton
                objectName: "trashRefresh"
                label: "refresh"
                key: "F5"
                usable: !contents.busy
                onClicked: contents.reload()
                Keys.onTabPressed: function (event) { Focus.move(event, receiptButton.visible ? receiptButton : list, emptyButton) }
                Keys.onBacktabPressed: function (event) { Focus.move(event, emptyButton) }
            }
            Chip {
                id: receiptButton
                objectName: "trashReceipt"
                visible: contents.message !== ""
                label: "details"
                onClicked: trash.receiptRequested(contents.message)
                Keys.onTabPressed: function (event) { Focus.move(event, list, refreshButton) }
                Keys.onBacktabPressed: function (event) { Focus.move(event, refreshButton) }
            }
        }
        OverlayList {
            id: list
            objectName: "trashList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            activeFocusOnTab: true
            focus: true
            model: contents
            refreshing: contents.loading
            onCurrentIndexChanged: {
                if (!contents.loading && currentIndex >= 0 && currentIndex < contents.count) {
                    trash.cursorPath = contents.pathAt(currentIndex)
                    trash.lastCursor = currentIndex
                }
                Qt.callLater(revealCurrent)
            }
            topMargin: Theme.focusRing
            bottomMargin: Theme.focusRing
            FocusRing {
                parent: list
                focused: list.activeFocus || (list.currentItem !== null && list.currentItem.activeFocus)
                inset: Theme.focusRing
            }
            Keys.onTabPressed: function (event) { Focus.move(event, restoreButton, receiptButton.visible ? receiptButton : refreshButton) }
            Keys.onBacktabPressed: function (event) { Focus.move(event, receiptButton.visible ? receiptButton : refreshButton) }
            delegate: Rectangle {
                id: row
                objectName: "trashRow"
                required property int index
                required property string name
                required property string original
                required property string deleted
                required property string problem
                required property bool marked
                x: Theme.focusRing
                width: list.rowWidth - 2 * Theme.focusRing
                height: rowLabels.implicitHeight + 2 * Theme.spacingS
                radius: Theme.rounding
                color: row.ListView.isCurrentItem ? Theme.cursor
                     : row.marked ? Theme.marked
                                  : Theme.base
                Accessible.role: Accessible.ListItem
                Accessible.name: name + "; Original: " + original + "; Deleted: " + deleted + "; " + problem
                Column {
                    id: rowLabels
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: Theme.spacingS
                    y: Theme.spacingS
                    spacing: Theme.spacingXs
                    Label { width: parent.width; text: (row.marked ? "✓ " : "") + row.name; elide: Text.ElideMiddle }
                    Label { width: parent.width; text: "Original: " + (row.original || "unavailable"); quiet: true; elide: Text.ElideMiddle }
                    Label { width: parent.width; text: "Deleted: " + (row.deleted || "unavailable"); quiet: true; elide: Text.ElideMiddle }
                    Label { width: parent.width; visible: row.problem !== ""; text: row.problem; elide: Text.ElideMiddle }
                }
                RowTapHandler {
                    onClicked: function (position, modifiers) {
                        list.currentIndex = row.index
                        list.forceActiveFocus()
                        if (modifiers & Qt.ControlModifier) contents.toggleMark(row.index)
                    }
                    onContextRequested: function (position) {
                        list.currentIndex = row.index
                        list.forceActiveFocus()
                        const pos = row.mapToItem(trash, position.x, position.y)
                        trash.menuRequested(pos.x, pos.y)
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                text: contents.loading ? "Reading Trash…" : contents.error !== "" ? "No Trash entries listed" : "Trash is empty"
                visible: contents.count === 0
            }
        }
    }
}
