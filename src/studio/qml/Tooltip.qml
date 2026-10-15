pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as C
import omafiles

// Delayed, nonfocusable help for a control.
C.ToolTip {
    id: tip

    property Item host: null
    objectName: "controlTooltip"

    popupType: C.Popup.Item
    width: Math.max(0, Math.min(420, ruler.advanceWidth + 14, parent ? parent.width - 12 : 420))
    height: Math.max(0, Math.min(label.implicitHeight + 14, parent ? parent.height - 12 : 300))
    onWidthChanged: if (opened) position()
    onHeightChanged: if (opened) position()
    padding: 7
    modal: false
    dim: false
    focus: false
    closePolicy: C.Popup.NoAutoClose
    delay: 480

    background: Rectangle {
        color: Theme.background
        border.width: 1
        border.color: Theme.fill(Theme.accent, 0.6)
        radius: Theme.rounding
    }

    TextMetrics {
        id: ruler
        text: tip.text
        font.family: Theme.fontFamily
        font.pixelSize: 11
    }
    contentItem: OverlayScroll {
        id: scroll
        contentHeight: label.implicitHeight
        HoverHandler {
            id: tipHover
            parent: scroll
            onHoveredChanged: {
                if (hovered)
                    exitDelay.stop()
                else
                    tip.dismiss()
            }
        }
        Text {
            id: label
            width: scroll.width
            text: tip.text
            wrapMode: Text.WrapAnywhere
            color: Theme.foreground
            font.family: Theme.fontFamily
            font.pixelSize: 11
            renderType: Text.NativeRendering
        }
    }
    function handleKey(event) {
        return tip.opened && scroll.handleKey(event)
    }
    Connections {
        target: tip.parent
        function onWidthChanged() { if (tip.opened) tip.position() }
        function onHeightChanged() { if (tip.opened) tip.position() }
    }

    Connections {
        target: tip.host
        function onXChanged() { if (tip.opened) tip.position() }
        function onYChanged() { if (tip.opened) tip.position() }
    }

    Timer {
        id: exitDelay
        interval: 200
        onTriggered: if (!tipHover.hovered) tip.close()
    }

    /// Ask for the tip. It appears once the pointer has stayed put.
    function request(item) {
        if (!item || tip.text === "")
            return
        exitDelay.stop()
        tip.host = item
        tip.place()
    }

    /// Take it back, whether it had appeared yet or not.
    function cancel() {
        exitDelay.stop()
        close()
    }

    function dismiss() {
        if (!tip.opened)
            tip.close()
        else if (!tipHover.hovered)
            exitDelay.restart()
    }

    function place() {
        const item = tip.host
        if (!item)
            return
        const overlay = C.Overlay.overlay
        if (overlay)
            parent = overlay
        scroll.contentY = 0
        position()
        open()
    }
    function position() {
        const item = tip.host
        const target = tip.parent
        if (!item || !target)
            return
        const under = item.mapToItem(target, Math.round((item.width - width) / 2), item.height + 6)
        x = Math.max(6, Math.min(under.x, target.width - width - 6))
        y = Math.max(6, Math.min(under.y, target.height - height - 6))
    }
}
