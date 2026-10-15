pragma ComponentBehavior: Bound

import QtQuick
import "Focus.js" as Focus
import omafiles

// Action hints wrap within the available footer width.
Item {
    id: hints
    property var model: []
    property Item nextFocus: null
    property Item previousFocus: null
    property var controls: []
    readonly property Item firstControl: adjacent(-1, 1)
    readonly property Item lastControl: adjacent(controls.length, -1)
    signal requested(string key)

    ListModel { id: actions }
    onModelChanged: {
        for (let i = 0; i < model.length; ++i) {
            const action = { shortcut: model[i].key, caption: model[i].label }
            if (i < actions.count) actions.set(i, action)
            else actions.append(action)
        }
        if (actions.count > model.length) actions.remove(model.length, actions.count - model.length)
    }

    function adjacent(index, direction) {
        for (let at = index + direction; at >= 0 && at < controls.length; at += direction) {
            const item = controls[at]
            if (item && item.usable && item.visible)
                return item
        }
        return direction > 0 ? nextFocus : previousFocus
    }

    implicitHeight: strip.implicitHeight
    clip: true

    Flow {
        id: strip
        width: parent.width
        spacing: 8

        Repeater {
            model: actions
            onItemAdded: function (index, item) {
                const next = hints.controls.slice()
                next[index] = item
                hints.controls = next
            }
            onItemRemoved: function (index) {
                const next = hints.controls.slice()
                next[index] = null
                hints.controls = next
            }

            FocusableControl {
                id: pair
                objectName: "hintAction"
                pointerFocus: pair.Window.activeFocusItem || pair
                required property string shortcut
                required property string caption
                readonly property var modelData: ({ key: shortcut, label: caption })
                required property int index
                Keys.onTabPressed: function (event) { Focus.move(event, hints.adjacent(index, 1), hints.adjacent(index, -1)) }
                Keys.onBacktabPressed: function (event) { Focus.move(event, hints.adjacent(index, -1)) }
                implicitWidth: contents.implicitWidth
                height: 22
                color: hovered ? hoverChromeFill : "transparent"
                radius: Theme.rounding
                tooltipCaption: pair.modelData.label
                tooltipKey: pair.modelData.key
                Accessible.role: Accessible.Button
                Accessible.name: pair.modelData.label + " (" + pair.modelData.key + ")"
                onActivated: hints.requested(pair.modelData.key)

                Row {
                    id: contents
                    anchors.centerIn: parent
                    spacing: 5
                    KeyCap {
                        anchors.verticalCenter: parent.verticalCenter
                        key: pair.modelData.key
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: pair.modelData.label
                        color: Theme.dim
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        renderType: Text.NativeRendering
                    }
                }
            }
        }
    }
}
