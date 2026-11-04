pragma ComponentBehavior: Bound

import QtQuick
import "Focus.js" as Focus
import omafiles

// Action hints on one line, right-aligned. The model lists them most important
// first; those that do not fit are dropped from the end, the first is always
// shown.
Item {
    id: hints
    property var model: []
    property Item nextFocus: null
    property Item previousFocus: null
    property var controls: []
    readonly property Item firstControl: adjacent(-1, 1)
    readonly property Item lastControl: adjacent(controls.length, -1)
    // How many hints, from the first, fit in the width.
    readonly property int shown: {
        let used = 0
        let count = 0
        for (; count < controls.length && controls[count]; ++count) {
            used += controls[count].implicitWidth + (count > 0 ? strip.spacing : 0)
            if (count > 0 && used > width - 2 * Theme.focusRing)
                break
        }
        return count
    }
    // The width the first hint needs, its focus ring included.
    readonly property real firstWidth: controls.length > 0 && controls[0]
        ? controls[0].implicitWidth + 2 * Theme.focusRing : 0
    signal requested(string key)

    ListModel { id: actions }
    // Rebuilt once the change that asked for it is done: the model is computed
    // from where focus is, and removing the focused hint inside that
    // computation would move focus while it is still being read.
    onModelChanged: Qt.callLater(hints.rebuild)
    function rebuild() {
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

    // The clip keeps a wide first hint off the status text; the margin keeps
    // a hint's focus ring inside it.
    implicitHeight: Theme.controlHeight + 2 * Theme.focusRing
    clip: true

    Row {
        id: strip
        anchors.right: parent.right
        anchors.rightMargin: Theme.focusRing
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spacingS

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
                visible: index < hints.shown
                Keys.onTabPressed: function (event) { Focus.move(event, hints.adjacent(index, 1), hints.adjacent(index, -1)) }
                Keys.onBacktabPressed: function (event) { Focus.move(event, hints.adjacent(index, -1)) }
                implicitWidth: contents.implicitWidth
                height: Theme.controlHeight
                hasChrome: hovered
                color: Theme.hoverChrome
                radius: Theme.rounding
                tooltipCaption: pair.modelData.label
                tooltipKey: pair.modelData.key
                Accessible.role: Accessible.Button
                Accessible.name: pair.modelData.label + " (" + pair.modelData.key + ")"
                onActivated: hints.requested(pair.modelData.key)

                Row {
                    id: contents
                    anchors.centerIn: parent
                    spacing: Theme.spacingXs
                    KeyCaps {
                        anchors.verticalCenter: parent.verticalCenter
                        keys: pair.modelData.key
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: pair.modelData.label
                        color: Theme.dim
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSmall
                        renderType: Text.NativeRendering
                    }
                }
            }
        }
    }
}
