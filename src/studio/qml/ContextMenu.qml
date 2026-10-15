pragma ComponentBehavior: Bound

import QtQuick
import omafiles

Item {
    id: menu

    property var actions: []
    property ListView list: null
    property Rectangle panel: null
    property TextMetrics labelRuler: null
    property TextMetrics capRuler: null
    readonly property Item focusTarget: menu.list
    signal chosen(string id)

    // Compare hover in client coordinates so scrolling cannot select a stationary pointer.
    property point lastPointer: Qt.point(-1, -1)

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

    onWidthChanged: clampPanel()
    onHeightChanged: clampPanel()

    function clampPanel() {
        if (!menu.panel)
            return
        panel.width = Math.min(panelWidth(), Math.max(0, menu.width - 16))
        const margin = 8
        panel.x = Math.max(margin, Math.min(panel.x, menu.width - panel.width - margin))
        panel.y = Math.max(margin, Math.min(panel.y, menu.height - panel.height - margin))
    }

    // Measure labels without instantiating offscreen delegates.
    function panelWidth() {
        let widest = 0
        for (let i = 0; i < menu.shown.length; i++) {
            labelRuler.text = menu.shown[i].label
            capRuler.text = menu.shown[i].key || ""
            widest = Math.max(widest, labelRuler.advanceWidth
                              + (capRuler.text === "" ? 0 : capRuler.advanceWidth + 18))
        }
        return Math.max(200, Math.ceil(widest) + 24)
    }

    function openAt(x, y) {
        content.active = true
        panel.width = menu.panelWidth()
        panel.x = x
        panel.y = y
        menu.lastPointer = Qt.point(-1, -1)
        list.currentIndex = menu.shown.length > 0 ? 0 : -1
        visible = true
        clampPanel()
        focusTarget.forceActiveFocus()
    }

    function close() {
        visible = false
    }

    function chooseCurrent() {
        if (list.currentIndex < 0 || list.currentIndex >= menu.shown.length)
            return
        menu.chosen(menu.shown[list.currentIndex].id)
        menu.close()
    }

    Loader {
        id: content
        anchors.fill: parent
        active: false
        focus: menu.visible
        sourceComponent: FocusScope {
            id: focusScope
            Component.onCompleted: {
                menu.labelRuler = labelRuler
                menu.capRuler = capRuler
                menu.panel = panel
                menu.list = list
            }

            TextMetrics {
                id: labelRuler
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }

            TextMetrics {
                id: capRuler
                font.family: Theme.fontFamily
                font.pixelSize: 11
            }

            MouseArea {
                anchors.fill: parent
                visible: menu.visible
                hoverEnabled: true
                onPositionChanged: function (mouse) {
                    if (mouse.buttons === Qt.NoButton) menu.lastPointer = Qt.point(mouse.x, mouse.y)
                }
                onClicked: menu.close()
            }


            anchors.fill: parent
            focus: menu.visible

            Keys.onPressed: function (event) {
                if (list.handleKey(event))
                    return
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
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                           || event.key === Qt.Key_Space) {
                    menu.chooseCurrent()
                    event.accepted = true
                }
            }

            Rectangle {
                id: panel
                z: 1
                width: 200
                height: Math.max(0, Math.min(menu.height - 16, menu.shown.length * 28 + 8))
                onHeightChanged: menu.clampPanel()
                radius: Theme.rounding
                color: Theme.panel
                border.width: 1
                border.color: Theme.accent

                Accessible.role: Accessible.PopupMenu
                Accessible.name: "Menu"
                // Blank panel space absorbs a click without dismissing the menu.
                MouseArea {
                    anchors.fill: parent
                    onClicked: function (mouse) { mouse.accepted = true }
                }

                OverlayList {
                    highlightMoveDuration: 0
                    highlightResizeDuration: 0
                    reuseItems: true
                    id: list
                    anchors.fill: parent
                    anchors.margins: 4
                    clip: true
                    model: menu.visible ? menu.shown : []
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
                            anchors.right: shortcut.left
                            anchors.rightMargin: 8
                            elide: Text.ElideRight
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            text: itemRow.modelData.label
                            color: Theme.foreground
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                        Text {
                            id: shortcut
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            text: itemRow.modelData.key || ""
                            color: Theme.dim
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                        }
                        // Hover selection follows actual movement, leaving touch drags to the list.
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onPositionChanged: function (mouse) {
                                if (mouse.buttons !== Qt.NoButton || list.moving)
                                    return
                                const here = itemRow.mapToItem(menu, mouse.x, mouse.y)
                                const unseen = menu.lastPointer.x < 0
                                const still = Math.abs(here.x - menu.lastPointer.x) < 1
                                           && Math.abs(here.y - menu.lastPointer.y) < 1
                                menu.lastPointer = here
                                // Opening under a still pointer does not select its row.
                                if (unseen || still)
                                    return
                                list.currentIndex = itemRow.index
                            }
                            onClicked: {
                                list.currentIndex = itemRow.index
                                menu.chosen(itemRow.modelData.id)
                                menu.close()
                            }
                        }
                    }
                }
            }
        }
    }
}
