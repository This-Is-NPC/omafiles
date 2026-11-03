pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// Text in the window's voice. `quiet` is the difference between a value and the
// word naming it: a caption at Theme.fontSmall, a value at Theme.fontBody.
Text {
    property bool quiet: false
    color: quiet ? Theme.dim : Theme.foreground
    font.family: Theme.fontFamily
    font.pixelSize: quiet ? Theme.fontSmall : Theme.fontBody
    elide: Text.ElideRight
    renderType: Text.NativeRendering
}
