pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as C
import omafiles

// The name of a control and the key that works it.
//
// It opens below the control rather than beside it, because beside is where the
// next control is: on a toolbar, a tip explaining one button covers the two next
// to it, so pointing at the thing you want hides where it is.
//
// And it waits before opening. A tip that appears the instant the pointer
// crosses a control is a panel that flashes over the window every time you move
// the mouse across the top of it; the pause is what makes it an answer to a
// question rather than an interruption.
C.Popup {
    id: tip

    property string text: ""
    property Item host: null

    padding: 7
    modal: false
    dim: false
    focus: false
    closePolicy: C.Popup.NoAutoClose

    background: Rectangle {
        color: Theme.background
        border.width: 1
        border.color: Theme.fill(Theme.accent, 0.6)
        radius: Theme.rounding
    }

    contentItem: Text {
        text: tip.text
        color: Theme.foreground
        font.family: Theme.fontFamily
        font.pixelSize: 11
        renderType: Text.NativeRendering
    }

    Timer {
        id: hold
        interval: 480
        onTriggered: tip.place()
    }

    /// Ask for the tip. It appears once the pointer has stayed put.
    function request(item) {
        if (!item || tip.text === "")
            return
        tip.host = item
        hold.restart()
    }

    /// Take it back, whether it had appeared yet or not.
    function dismiss() {
        hold.stop()
        tip.close()
    }

    function place() {
        const item = tip.host
        if (!item)
            return
        const overlay = C.Overlay.overlay
        if (overlay)
            parent = overlay
        const target = parent ? parent : item
        const under = item.mapToItem(target, Math.round((item.width - implicitWidth) / 2),
                                      item.height + 6)
        const limit = target.width ? target.width - implicitWidth - 6 : under.x
        x = Math.max(6, Math.min(under.x, limit))
        y = Math.max(0, under.y)
        open()
    }
}
