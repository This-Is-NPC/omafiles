pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// One key, drawn as a key. A word set in inverse video reads as something you
// press; the same word in running text reads as something you are being told.
Rectangle {
    id: cap
    objectName: "keyCap"
    property string key: ""

    width: capText.implicitWidth + Theme.spacingS
    height: Theme.chipHeight
    radius: Math.max(2, Theme.rounding - 2)
    color: Theme.raised
    border.width: 1
    border.color: Theme.line

    Text {
        id: capText
        anchors.centerIn: parent
        text: cap.key
        color: Theme.foreground
        opacity: 0.8
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSmall
        renderType: Text.NativeRendering
    }
}
