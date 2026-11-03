pragma ComponentBehavior: Bound

import QtQuick
import omafiles

Item {
    id: menu

    property var actions: []
    property ListView list: null
    property Rectangle panel: null
    property TextMetrics labelRuler: null
    property Column keyRuler: null
    readonly property Item focusTarget: menu.list
    signal chosen(string id)

    // Compare hover in client coordinates so scrolling cannot select a stationary pointer.
    property point lastPointer: Qt.point(-1, -1)

    visible: false
    z: 90

    // An action with `opensGroup` starts a group. The first entry shown from
    // a group after another group carries `separated`, and its row draws the
    // separator, so a separator is never a row of its own.
    readonly property var shown: {
        const out = []
        let separated = false
        for (let i = 0; i < menu.actions.length; i++) {
            const item = menu.actions[i]
            separated = separated || item.opensGroup === true
            if (item.when === undefined || item.when === true) {
                out.push(Object.assign({}, item, { separated: separated && out.length > 0 }))
                separated = false
            }
        }
        return out
    }

    onWidthChanged: clampPanel()
    onHeightChanged: clampPanel()

    function clampPanel() {
        if (!menu.panel)
            return
        const margin = Theme.inset
        panel.width = Math.min(panelWidth(), Math.max(0, menu.width - 2 * margin))
        panel.x = Math.max(margin, Math.min(panel.x, menu.width - panel.width - margin))
        panel.y = Math.max(margin, Math.min(panel.y, menu.height - panel.height - margin))
    }

    // Measure labels without instantiating offscreen delegates; the keys are
    // measured by the key ruler, which holds only their caps.
    function panelWidth() {
        let widest = 0
        for (let i = 0; i < menu.shown.length; i++) {
            labelRuler.text = menu.shown[i].label
            widest = Math.max(widest, labelRuler.advanceWidth)
        }
        const keys = keyRuler.implicitWidth
        return Math.max(200, Math.ceil(widest + (keys > 0 ? keys + Theme.spacingL : 0))
                             + 2 * (Theme.spacingXs + Theme.spacingS))
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

    // Opens below the span from top to bottom, a row in this item's
    // coordinates, or above it when the panel does not fit below.
    function openBelow(x, top, bottom) {
        openAt(x, bottom)
        if (bottom + panel.height > menu.height - Theme.inset) {
            panel.y = top - panel.height
            clampPanel()
        }
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
                menu.keyRuler = keyRuler
                menu.panel = panel
                menu.list = list
            }

            TextMetrics {
                id: labelRuler
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
            }

            // Laid out, so it has a width, and never seen.
            Column {
                id: keyRuler
                opacity: 0
                onImplicitWidthChanged: menu.clampPanel()
                Repeater {
                    model: menu.visible ? menu.shown : []
                    KeyCaps {
                        required property var modelData
                        keys: modelData.key || ""
                    }
                }
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
                height: list.wholeRows(menu.height - 2 * (Theme.inset + Theme.spacingXs))
                        + 2 * Theme.spacingXs
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
                    anchors.margins: Theme.spacingXs
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
                        height: Theme.rowHeight
                        radius: Theme.rounding
                        color: list.currentIndex === itemRow.index
                               ? Theme.fill(Theme.accent, 0.18) : "transparent"
                        Accessible.role: Accessible.MenuItem
                        Accessible.name: itemRow.modelData.label

                        Rule {
                            objectName: "menuSeparator"
                            visible: itemRow.modelData.separated
                            anchors.top: parent.top
                        }

                        Text {
                            objectName: "menuLabel"
                            anchors.right: shortcut.left
                            // The gap panelWidth() reserves before a row's keys.
                            anchors.rightMargin: shortcut.keys === "" ? 0 : Theme.spacingL
                            elide: Text.ElideRight
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.spacingS
                            text: itemRow.modelData.label
                            color: Theme.foreground
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontBody
                        }
                        KeyCaps {
                            id: shortcut
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.spacingS
                            keys: itemRow.modelData.key || ""
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
