pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// A drawn mark, in a box of any size.
//
// Every path below is written in a 16-unit square and then scaled to whatever
// the item is. Without the scale the glyph draws at 16 pixels in the corner of
// the box it was given, so an icon asked for at 30 across sits up and to the
// left of the name centred under it.
Item {
    id: icon

    property string kind: "file"
    property color ink: Theme.foreground

    implicitWidth: 16
    implicitHeight: 16
    width: implicitWidth
    height: implicitHeight

    Canvas {
        id: canvas
        anchors.fill: parent
        antialiasing: true
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.scale(width / 16, height / 16)
            ctx.strokeStyle = icon.ink
            ctx.fillStyle = icon.ink
            ctx.lineWidth = 1.25
            ctx.lineJoin = "round"
            ctx.lineCap = "round"

            function strokeClosed(pts) {
                ctx.beginPath()
                ctx.moveTo(pts[0][0], pts[0][1])
                for (let i = 1; i < pts.length; ++i)
                    ctx.lineTo(pts[i][0], pts[i][1])
                ctx.closePath()
                ctx.stroke()
            }

            const k = icon.kind
            if (k === "folder" || k === "parent") {
                ctx.beginPath()
                ctx.moveTo(1.5, 5.2)
                ctx.lineTo(1.5, 4.2)
                ctx.quadraticCurveTo(1.5, 2.6, 3.1, 2.6)
                ctx.lineTo(6.6, 2.6)
                ctx.lineTo(8.1, 5.2)
                ctx.lineTo(13.2, 5.2)
                ctx.quadraticCurveTo(14.5, 5.2, 14.5, 6.6)
                ctx.lineTo(14.5, 13.1)
                ctx.quadraticCurveTo(14.5, 14.4, 13.1, 14.4)
                ctx.lineTo(2.9, 14.4)
                ctx.quadraticCurveTo(1.5, 14.4, 1.5, 13.1)
                ctx.closePath()
                ctx.stroke()
                if (k === "parent") {
                    ctx.beginPath()
                    ctx.moveTo(8, 8.1)
                    ctx.lineTo(8, 12.2)
                    ctx.moveTo(5.6, 9.8)
                    ctx.lineTo(8, 7.6)
                    ctx.lineTo(10.4, 9.8)
                    ctx.stroke()
                }
            } else if (k === "file") {
                ctx.beginPath()
                ctx.moveTo(4, 1.6)
                ctx.lineTo(9.2, 1.6)
                ctx.lineTo(12.4, 4.8)
                ctx.lineTo(12.4, 14.4)
                ctx.lineTo(3.6, 14.4)
                ctx.lineTo(3.6, 1.6)
                ctx.closePath()
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(9.2, 1.6)
                ctx.lineTo(9.2, 4.8)
                ctx.lineTo(12.4, 4.8)
                ctx.stroke()
            } else if (k === "home") {
                ctx.beginPath()
                ctx.moveTo(1.8, 8.2)
                ctx.lineTo(8, 2.4)
                ctx.lineTo(14.2, 8.2)
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(3.6, 7.6)
                ctx.lineTo(3.6, 14.2)
                ctx.lineTo(12.4, 14.2)
                ctx.lineTo(12.4, 7.6)
                ctx.stroke()
            } else if (k === "up") {
                ctx.beginPath()
                ctx.moveTo(8, 3.2)
                ctx.lineTo(8, 13.4)
                ctx.moveTo(4.2, 7)
                ctx.lineTo(8, 3.2)
                ctx.lineTo(11.8, 7)
                ctx.stroke()
            } else if (k === "graph") {
                ctx.beginPath()
                ctx.arc(4.2, 11.6, 1.6, 0, Math.PI * 2)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(8, 4.4, 1.6, 0, Math.PI * 2)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(12.2, 10.2, 1.6, 0, Math.PI * 2)
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(5.4, 10.6)
                ctx.lineTo(6.8, 5.6)
                ctx.moveTo(9.4, 5.6)
                ctx.lineTo(10.8, 8.8)
                ctx.stroke()
            } else if (k === "reload") {
                ctx.beginPath()
                ctx.arc(8, 8, 4.6, 0.4, Math.PI * 1.6)
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(12.4, 4.2)
                ctx.lineTo(13.2, 7.4)
                ctx.lineTo(9.8, 7)
                ctx.stroke()
            } else if (k === "split") {
                ctx.beginPath()
                ctx.rect(2.2, 2.6, 5, 10.8)
                ctx.stroke()
                ctx.beginPath()
                ctx.rect(8.8, 2.6, 5, 10.8)
                ctx.stroke()
            } else if (k === "plus") {
                ctx.beginPath()
                ctx.moveTo(8, 3.4)
                ctx.lineTo(8, 12.6)
                ctx.moveTo(3.4, 8)
                ctx.lineTo(12.6, 8)
                ctx.stroke()
            } else if (k === "tag") {
                ctx.beginPath()
                ctx.moveTo(2.4, 3.4)
                ctx.lineTo(9.2, 3.4)
                ctx.lineTo(14.2, 8.2)
                ctx.lineTo(8.4, 14)
                ctx.lineTo(2.4, 8.2)
                ctx.closePath()
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(6, 6.2, 0.9, 0, Math.PI * 2)
                ctx.fill()
            }
        }
    }

    onKindChanged: canvas.requestPaint()
    onInkChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
    Component.onCompleted: canvas.requestPaint()
}
