pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The one way a key spelling is drawn (docs/design.md §3, key spelling): each
// space-separated name is a cap, and each " / "-separated alternative follows
// a thin slash. Caps and slash share the cap height, so they line up.
Row {
    id: caps
    objectName: "keyCaps"
    property string keys: ""

    spacing: Theme.spacingXs

    Repeater {
        model: caps.keys === "" ? [] : caps.keys.split(" / ")

        Row {
            id: alternative
            required property string modelData
            required property int index
            spacing: Theme.spacingXs

            Text {
                height: Theme.chipHeight
                verticalAlignment: Text.AlignVCenter
                visible: alternative.index > 0
                text: "/"
                color: Theme.dim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
                renderType: Text.NativeRendering
            }
            Repeater {
                model: alternative.modelData.split(" ")
                KeyCap {
                    required property string modelData
                    key: modelData
                }
            }
        }
    }
}
