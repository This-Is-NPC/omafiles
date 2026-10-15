pragma ComponentBehavior: Bound

import QtQuick
import omafiles

Column {
    id: section
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
        function onChanged() { section.updateRows() }
    }
    spacing: 5

    FocusableControl {
        id: heading
        objectName: "devicesHeading"
        width: parent.width
        height: 24
        handCursor: true
        tabFocusable: devices.count === 0 || activeFocus
        pointerFocus: section.focusTarget
        Accessible.name: "Devices"
        onActiveFocusChanged: if (activeFocus) section.focusEntered()
        onActivated: section.focusTarget.forceActiveFocus()
        Keys.onPressed: function (event) { section.keyRequested(event) }
        Label { anchors.verticalCenter: parent.verticalCenter; quiet: true; text: "devices" }
        Label { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; quiet: true; text: "d" }
    }

    ListView {
        id: devices
        objectName: "sidebarDevices"
        width: parent.width
        height: Math.min(contentHeight, section.maximumHeight)
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
        Keys.onPressed: function (event) { section.keyRequested(event) }
        function reveal() {
            if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
            section.selectionChanged()
        }
        onCurrentIndexChanged: {
            if (!section.updating) {
                const row = Devices.entries[currentIndex]
                section.selectedId = row ? row.id : ""
                Qt.callLater(reveal)
            }
        }
        onHeightChanged: Qt.callLater(reveal)
        onCountChanged: {
            if (section.updating) return
            if (currentIndex >= count) currentIndex = count - 1
            if (!count && activeFocus) heading.forceActiveFocus()
            Qt.callLater(reveal)
        }
        onActiveFocusChanged: if (activeFocus) {
            if (currentIndex < 0 && count > 0) currentIndex = 0
            section.focusEntered()
            reveal()
        }
        ImmediateWheel { anchors.fill: parent; view: devices; step: 32 }
        Rectangle {
            parent: devices
            anchors.fill: parent
            color: "transparent"
            border.width: devices.activeFocus ? 1 : 0
            border.color: Theme.accent
            z: 2
        }
        delegate: Rectangle {
            id: row
            objectName: "deviceRow"
            required property var modelData
            required property int index
            width: ListView.view.width
            height: 32
            radius: Theme.rounding
            color: devices.activeFocus && devices.currentIndex === index ? Theme.fill(Theme.accent, 0.18)
                 : hover.hovered ? Theme.fill(Theme.foreground, 0.06) : "transparent"
            Accessible.role: Accessible.ListItem
            Accessible.name: modelData.name
            Accessible.description: modelData.mounted ? "Mounted" : "Not mounted"
            Icon {
                id: icon
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                kind: "folder"
                ink: row.modelData.mounted ? Theme.accent : Theme.dim
            }
            Label {
                anchors.left: icon.right
                anchors.leftMargin: 8
                anchors.right: menuButton.left
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.name
            }
            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
            Item {
                anchors.left: parent.left
                anchors.right: menuButton.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                RowTapHandler {
                    onClicked: {
                        devices.currentIndex = row.index
                        devices.forceActiveFocus()
                        section.activated(row.modelData.id)
                    }
                    onContextRequested: function (position) {
                        devices.currentIndex = row.index
                        const at = row.mapToItem(null, position.x, position.y)
                        section.menuRequested(row.modelData.id, at.x, at.y)
                    }
                }
            }
            Chip {
                id: menuButton
                objectName: "deviceMenuButton"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 32
                height: 32
                label: "⋮"
                caption: "Device actions"
                tabFocusable: false
                onClicked: {
                    devices.currentIndex = row.index
                    const at = menuButton.mapToItem(null, 0, menuButton.height)
                    section.menuRequested(row.modelData.id, at.x, at.y)
                }
            }
        }
    }
    Label {
        objectName: "devicesStatus"
        width: parent.width
        visible: Devices.status !== "" || devices.count === 0
        text: Devices.status || "No removable devices"
        wrapMode: Text.Wrap
        elide: Text.ElideNone
        quiet: true
    }
    Chip {
        objectName: "cancelDeviceOperation"
        visible: Devices.busy
        label: "Cancel device operation"
        onClicked: Devices.act("", "cancel")
    }
}
