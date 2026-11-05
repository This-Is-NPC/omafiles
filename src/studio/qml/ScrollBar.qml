pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import omafiles

// The one scroll bar. Shown whenever the content is taller than the view, in a
// strip the view reserves (`Theme.scrollBarWidth`), so it never covers a row.
// Its trough is `chrome` with a `line` edge, its thumb `dim`, and `foreground`
// under the pointer: Theme keeps `dim` at 4.5:1 on `chrome` in every theme, so
// the thumb stands out on whatever surface holds the bar. Dragging the thumb
// scrolls, a click in the trough pages toward the pointer, and a middle click
// or a Shift click jumps there. It never takes the keyboard.
//
// The pointer is handled here rather than by the template, whose press in the
// trough jumps instead of paging.
T.ScrollBar {
    id: bar
    objectName: "scrollBar"
    readonly property bool lit: pointer.containsMouse || pointer.pressed

    interactive: false
    // Keeps the thumb off the trough's edge line, `scrollThumb` wide at rest.
    horizontalPadding: (Theme.scrollBarWidth - Theme.scrollThumb) / 2
    visible: bar.size < 1
    minimumSize: bar.height > 0 ? Math.min(1, Theme.rowHeight / bar.height) : 0
    implicitWidth: bar.lit ? Theme.scrollBarHover : Theme.scrollBarWidth

    // Moves the thumb's top to `next`, kept inside the track.
    function moveTo(next) {
        bar.position = Math.max(0, Math.min(1 - bar.size, next))
    }

    contentItem: Rectangle {
        objectName: "scrollThumb"
        radius: Theme.rounding
        color: bar.lit ? Theme.foreground : Theme.dim
    }

    background: MouseArea {
        id: pointer
        // Where on the thumb a drag holds it, as a fraction of the track; -1
        // while no drag is on.
        property real grip: -1
        hoverEnabled: true
        // A drag on the thumb stays the bar's, not the pane's under it.
        preventStealing: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        onPressed: function (mouse) {
            const thumb = bar.contentItem
            const at = mouse.y / bar.height
            grip = -1
            if (mouse.button === Qt.MiddleButton || (mouse.modifiers & Qt.ShiftModifier))
                bar.moveTo(at - bar.size / 2)
            else if (mouse.y >= thumb.y && mouse.y < thumb.y + thumb.height)
                grip = at - bar.position
            else
                bar.moveTo(bar.position + (at < bar.position ? -bar.size : bar.size))
        }
        onPositionChanged: function (mouse) {
            if (grip >= 0)
                bar.moveTo(mouse.y / bar.height - grip)
        }
        onReleased: grip = -1

        Rectangle {
            objectName: "scrollTrough"
            anchors.fill: parent
            color: Theme.chrome
            Rule { vertical: true; anchors.left: parent.left }
        }
    }
}
