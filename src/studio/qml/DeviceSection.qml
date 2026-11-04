pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import omafiles

Column {
    id: deviceSection
    property real maximumHeight: 160
    property alias list: devices
    readonly property Item focusTarget: devices.count > 0 ? devices : heading
    signal focusEntered()
    signal selectionChanged()
    signal keyRequested(var event)
    signal activated(string id)
    signal menuRequested(string id, real x, real y)
    property string selectedId: ""
    property bool updating: false
    function updateRows() {
        const selected = selectedId
        const focused = devices.activeFocus || heading.activeFocus
        updating = true
        const rows = Devices.entries
        devices.model = rows
        let index = rows.findIndex(function (row) { return row.id === selected })
        if (index < 0) index = rows.length ? Math.min(Math.max(0, devices.currentIndex), rows.length - 1) : -1
        devices.currentIndex = index
        selectedId = index >= 0 ? rows[index].id : ""
        updating = false
        if (focused) focusTarget.forceActiveFocus()
        Qt.callLater(devices.reveal)
    }
    Component.onCompleted: updateRows()
    Connections {
        target: Devices
        function onChanged() { deviceSection.updateRows() }
    }
    spacing: Theme.spacingXs
    SectionHeading {
        id: heading
        objectName: "devicesHeading"
        width: parent.width
        target: deviceSection.focusTarget
        onActiveFocusChanged: if (activeFocus) deviceSection.focusEntered()
        Keys.onPressed: function (event) { deviceSection.keyRequested(event) }
        text: "Devices"
        key: "d"
    }

    ListView {
        id: devices
        T.ScrollBar.vertical: ScrollBar {}
        objectName: "sidebarDevices"
        width: parent.width
        height: Math.min(contentHeight + topMargin + bottomMargin, deviceSection.maximumHeight)
        topMargin: Theme.focusRing
        bottomMargin: Theme.focusRing
        visible: count > 0
        activeFocusOnTab: count > 0 || activeFocus
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: true
        keyNavigationEnabled: false
        highlightMoveDuration: 0
        highlightResizeDuration: 0
        model: []
        currentIndex: count > 0 ? 0 : -1
        Accessible.role: Accessible.List
        Accessible.name: "Devices"
        Keys.onPressed: function (event) { deviceSection.keyRequested(event) }
        function reveal() {
            if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
            deviceSection.selectionChanged()
        }
        onCurrentIndexChanged: {
            if (!deviceSection.updating) {
                const row = Devices.entries[currentIndex]
                deviceSection.selectedId = row ? row.id : ""
                Qt.callLater(reveal)
            }
        }
        onHeightChanged: Qt.callLater(reveal)
        onCountChanged: {
            if (deviceSection.updating) return
            if (currentIndex >= count) currentIndex = count - 1
            if (!count && activeFocus) heading.forceActiveFocus()
            Qt.callLater(reveal)
        }
        onActiveFocusChanged: if (activeFocus) {
            if (currentIndex < 0 && count > 0) currentIndex = 0
            deviceSection.focusEntered()
            reveal()
        }
        ImmediateWheel { anchors.fill: parent; view: devices; step: 32 }
        FocusRing {
            parent: devices
            focused: devices.activeFocus
            inset: Theme.focusRing
        }
        delegate: PanelRow {
            id: row
            objectName: "deviceRow"
            required property var modelData
            required property int index
            width: ListView.view.width - Theme.scrollBarWidth
            readonly property bool onCursor: devices.activeFocus && devices.currentIndex === index
            washed: onCursor || hovered
            color: onCursor ? Theme.cursor : Theme.hoverChrome
            Accessible.role: Accessible.ListItem
            Accessible.name: modelData.name
            Accessible.description: modelData.mounted ? "Mounted" : "Not mounted"
            iconKind: "folder"
            iconInk: row.modelData.mounted ? Theme.accent : Theme.dim
            text: row.modelData.name
            trailing: Chip {
                id: menuButton
                objectName: "deviceMenuButton"
                anchors.verticalCenter: parent.verticalCenter
                label: "⋮"
                caption: "Device actions"
                tabFocusable: false
                onClicked: {
                    devices.currentIndex = row.index
                    const at = menuButton.mapToItem(null, 0, menuButton.height)
                    deviceSection.menuRequested(row.modelData.id, at.x, at.y)
                }
            }
            Item {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: row.trailingX
                RowTapHandler {
                    onClicked: {
                        devices.currentIndex = row.index
                        devices.forceActiveFocus()
                        deviceSection.activated(row.modelData.id)
                    }
                    onContextRequested: function (position) {
                        devices.currentIndex = row.index
                        const at = row.mapToItem(null, position.x, position.y)
                        deviceSection.menuRequested(row.modelData.id, at.x, at.y)
                    }
                }
            }
        }
    }
    Label {
        objectName: "devicesStatus"
        x: Theme.inset
        width: parent.width - 2 * Theme.inset
        visible: Devices.status !== "" || devices.count === 0
        text: Devices.status || "No removable devices"
        wrapMode: Text.Wrap
        quiet: true
    }
    Chip {
        objectName: "cancelDeviceOperation"
        x: Theme.inset
        visible: Devices.busy
        label: "Cancel device operation"
        onClicked: Devices.act("", "cancel")
    }
}
