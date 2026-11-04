pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// Text in the window's voice. `quiet` is the difference between a value and the
// word naming it: a caption at Theme.fontCaption in dim, a value at
// Theme.fontBody. A label does not elide: each caller states how its text
// gives way (docs/design.md §3, eliding).
Text {
    property bool quiet: false
    color: quiet ? Theme.dim : Theme.foreground
    font.family: Theme.fontFamily
    font.pixelSize: quiet ? Theme.fontCaption : Theme.fontBody
    renderType: Text.NativeRendering
}
