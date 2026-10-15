pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The criteria narrowing the rows on screen, one chip each, and a way to drop
// them all. Drawn only while there is one, so a short list says why it is short.
Rectangle {
    id: bar

    property var criteria: ({})
    signal removed(string key, var value)
    signal cleared

    /// The modification windows offered, Nautilus's `When` presets.
    readonly property var presets: [
        { seconds: 86400, word: "day" },
        { seconds: 604800, word: "week" },
        { seconds: 2592000, word: "month" },
        { seconds: 31536000, word: "year" }
    ]
    /// Each criterion with its value, so a chip never reads a key the map has
    /// just lost.
    readonly property var held: Object.keys(bar.criteria).map(function (key) {
        return { key: key, value: bar.criteria[key] }
    })

    /// A criterion in words. The menus say `Only ` and then this.
    function label(key, value) {
        if (key === "type")
            return value
        if (key === "tag")
            return "#" + value
        if (key === "tagged")
            return value ? "tagged" : "untagged"
        if (key === "within")
            return "modified in the last "
                   + bar.presets.find(function (preset) { return preset.seconds === value }).word
        return value === "dir" ? "folders" : "files"
    }

    height: 30
    color: Theme.panel

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        Label {
            anchors.verticalCenter: parent.verticalCenter
            quiet: true
            text: "only"
        }
        Repeater {
            model: bar.held
            delegate: Chip {
                required property var modelData
                objectName: "criterionChip"
                on: true
                quiet: true
                label: bar.label(modelData.key, modelData.value) + "  ×"
                caption: "Remove this filter"
                onClicked: bar.removed(modelData.key, modelData.value)
            }
        }
        Chip {
            objectName: "clearCriteria"
            anchors.verticalCenter: parent.verticalCenter
            label: "Clear"
            caption: "Clear every filter"
            onClicked: bar.cleared()
        }
    }
}
