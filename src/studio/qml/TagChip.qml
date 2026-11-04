pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// One tag: a raised chip with the tag's colour square before its name.
Rectangle {
    id: chip

    property string tag: ""

    // From the name's width and not a Row's, which is laid out only at polish:
    // TagStrip measures a chip as soon as it sets the tag.
    implicitWidth: name.implicitWidth + Theme.tagSwatch + 3 * Theme.spacingXs
    implicitHeight: Theme.chipHeight
    height: implicitHeight
    radius: Theme.rounding
    color: Theme.raised
    border.width: 1
    border.color: Theme.line

    Rectangle {
        id: swatch
        objectName: "tagSwatch"
        x: Theme.spacingXs
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.tagSwatch
        height: Theme.tagSwatch
        color: {
            // tagColour is no binding dependency; `raised` changes with the
            // theme, so reading it redraws the square on a theme switch.
            Theme.raised
            return Theme.tagColour(chip.tag)
        }
    }

    Text {
        id: name
        x: swatch.x + swatch.width + Theme.spacingXs
        anchors.verticalCenter: parent.verticalCenter
        text: chip.tag
        textFormat: Text.PlainText
        color: Theme.foreground
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSmall
        renderType: Text.NativeRendering
    }
}
