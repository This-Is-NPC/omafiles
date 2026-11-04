pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The view mode of a files tab: one segmented group of three icons. It is one
// Tab stop, and Left and Right move the checked segment inside it; 1 2 3 are
// the window's keys for the same modes.
FocusScope {
    id: modes
    objectName: "viewModes"

    property var tab: null
    readonly property var choices: [{ mode: "list", caption: "List view", key: "1" },
                                    { mode: "details", caption: "Details view", key: "2" },
                                    { mode: "icons", caption: "Icons view", key: "3" }]
    readonly property int chosen: {
        const mode = modes.tab ? modes.tab.viewMode : ""
        return modes.choices.findIndex(choice => choice.mode === mode)
    }

    function choose(at) {
        if (modes.tab && at >= 0 && at < modes.choices.length)
            modes.tab.viewMode = modes.choices[at].mode
    }

    width: segments.width
    height: Theme.controlHeight

    Accessible.role: Accessible.Grouping
    Accessible.name: "View mode"
    activeFocusOnTab: true
    Keys.onLeftPressed: function (event) { modes.choose(modes.chosen - 1); event.accepted = true }
    Keys.onRightPressed: function (event) { modes.choose(modes.chosen + 1); event.accepted = true }

    Row {
        id: segments
        Repeater {
            model: modes.choices
            ToolButton {
                id: segment
                required property var modelData
                required property int index
                objectName: "viewMode"
                tabFocusable: false
                pointerFocus: modes
                iconKind: "view-" + segment.modelData.mode
                caption: segment.modelData.caption
                key: segment.modelData.key
                on: modes.chosen === segment.index
                iconInk: on ? Theme.accent : ink
                Accessible.checkable: true
                Accessible.checked: on
                onClicked: modes.choose(segment.index)
            }
        }
    }

    FocusRing { focused: modes.activeFocus }
}
