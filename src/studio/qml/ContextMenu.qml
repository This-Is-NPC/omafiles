pragma ComponentBehavior: Bound

import QtQuick
import omafiles

Item {
    id: menu

    property var actions: []
    property var returnFocus: function () {}
    signal chosen(string id)

    visible: false
    z: 90

    readonly property var shown: {
        const out = []
        for (let i = 0; i < menu.actions.length; i++) {
            const item = menu.actions[i]
            if (item.when === undefined || item.when === true)
                out.push(item)
        }
        return out
    }

    function clampPanel() {
        const margin = 8
        panel.x = Math.max(margin, Math.min(panel.x, parent.width - panel.width - margin))
        panel.y = Math.max(margin, Math.min(panel.y, parent.height - panel.height - margin))
    }

    function openAt(x, y) {
        panel.x = x
        panel.y = y
        list.currentIndex = menu.shown.length > 0 ? 0 : -1
        visible = true
        clampPanel()
        focusScope.forceActiveFocus()
    }

    function close() {
        visible = false
        returnFocus()
    }

    function chooseCurrent() {
        if (list.currentIndex < 0 || list.currentIndex >= menu.shown.length)
            return
        menu.chosen(menu.shown[list.currentIndex].id)
        menu.close()
    }

    MouseArea {
        anchors.fill: parent
        visible: menu.visible
        onClicked: menu.close()
    }

    FocusScope {
        id: focusScope
        focus: menu.visible

        Keys.onPressed: function (event) {
            if (event.key === Qt.Key_Escape) {
                menu.close()
                event.accepted = true
            } else if (event.key === Qt.Key_Down
                       || (!event.modifiers && event.text === "j")) {
                if (menu.shown.length > 0)
                    list.currentIndex = Math.min(list.currentIndex + 1, menu.shown.length - 1)
                event.accepted = true
            } else if (event.key === Qt.Key_Up
                       || (!event.modifiers && event.text === "k")) {
                if (menu.shown.length > 0)
                    list.currentIndex = Math.max(list.currentIndex - 1, 0)
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                menu.chooseCurrent()
                event.accepted = true
            }
        }

        Rectangle {
            id: panel
            z: 1
            width: Math.max(200, list.contentWidth + 20)
            height: Math.min(320, list.contentHeight + 8)
            radius: Theme.rounding
            color: Theme.panel
            border.width: 1
            border.color: Theme.accent

            Accessible.role: Accessible.PopupMenu
            Accessible.name: "Menu"
            MouseArea {
                anchors.fill: parent
                onClicked: function (mouse) { mouse.accepted = true }
            }

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                model: menu.shown
                currentIndex: 0
                onModelChanged: currentIndex = menu.shown.length > 0 ? 0 : -1
                onContentHeightChanged: menu.clampPanel()
                Accessible.role: Accessible.List

                delegate: Rectangle {
                    id: itemRow
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 28
                    radius: Theme.rounding
                    color: list.currentIndex === itemRow.index
                           ? Theme.fill(Theme.accent, 0.18) : "transparent"
                    Accessible.role: Accessible.MenuItem
                    Accessible.name: itemRow.modelData.label


                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        text: itemRow.modelData.label
                        color: Theme.foreground
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        text: itemRow.modelData.key || ""
                        color: Theme.dim
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            menu.chosen(itemRow.modelData.id)
                            menu.close()
                        }
                    }
                }
            }
        }
    }
}
