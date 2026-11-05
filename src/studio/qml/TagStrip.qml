pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The tags on one row, from the start of the tags column, inside a width it is
// not allowed to exceed.
//
// A tag that does not fit is dropped whole rather than sliced through a word,
// and it is dropped with opacity rather than `visible`, because a Row closes
// the gap left by an invisible child: the next tag would slide in, fit, appear,
// push the first one back out, and the row would flicker between two layouts.
// When some tags are hidden, a trailing +N chip states how many.
Item {
    id: strip

    objectName: "tagStrip"

    property var tags: []
    signal activated(string tag)
    /// The widest this is allowed to be. The row decides it, because the row is
    /// what has to give up the space.
    property real limit: 240
    /// How many tags are hidden behind the +N chip.
    property int hiddenCount: 0
    /// How many tag chips are drawn at full opacity.
    property int visibleCount: 0
    /// The +N chip's text and its two side gaps.
    readonly property int chipPadding: 2 * Theme.spacingXs

    implicitWidth: Math.min(row.width, limit)
    width: implicitWidth
    implicitHeight: Theme.chipHeight
    height: implicitHeight
    clip: true

    onWidthChanged: Qt.callLater(recomputeLayout)
    onLimitChanged: Qt.callLater(recomputeLayout)
    onTagsChanged: Qt.callLater(recomputeLayout)
    Component.onCompleted: recomputeLayout()

    TagChip {
        id: chipMetrics
        visible: false
    }

    Text {
        id: overflowMetrics
        visible: false
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontCaption
    }

    function chipWidth(tag) {
        chipMetrics.tag = tag
        return chipMetrics.implicitWidth
    }

    function overflowWidth(count) {
        if (count <= 0)
            return 0
        overflowMetrics.text = "+" + count
        return overflowMetrics.implicitWidth + strip.chipPadding
    }

    function recomputeLayout() {
        const tagList = strip.tags
        const n = tagList.length
        const spacing = row.spacing
        const avail = strip.limit

        if (n === 0 || avail <= 0) {
            strip.hiddenCount = 0
            strip.visibleCount = 0
            return
        }

        const widths = tagList.map(tag => chipWidth(tag))
        for (let vis = n; vis >= 1; --vis) {
            const hidden = n - vis
            let used = 0
            for (let i = 0; i < vis; ++i)
                used += widths[i] + (i > 0 ? spacing : 0)
            if (hidden > 0)
                used += spacing + overflowWidth(hidden)
            if (used <= avail) {
                strip.hiddenCount = hidden
                strip.visibleCount = vis
                return
            }
        }

        strip.hiddenCount = n
        strip.visibleCount = 0
    }

    Row {
        id: row
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spacingXs

        Repeater {
            model: strip.tags

            TagChip {
                id: chip
                objectName: "tagChip"
                required property int index
                required property string modelData
                readonly property bool shown: chip.index < strip.visibleCount
                tag: chip.modelData
                width: shown ? implicitWidth : 0
                opacity: shown ? 1 : 0
                Accessible.role: Accessible.Button
                Accessible.name: "Edit tag " + chip.modelData
                TapHandler { enabled: chip.shown; onTapped: strip.activated(chip.modelData) }
            }
        }

        Rectangle {
            id: overflowTrailing
            objectName: "tagStripOverflowTrailing"
            visible: strip.hiddenCount > 0
            height: Theme.chipHeight
            width: overflowTrailingMark.implicitWidth + strip.chipPadding
            radius: Theme.rounding
            color: Theme.raised
            border.width: 1
            border.color: Theme.line
            Accessible.role: Accessible.StaticText
            Accessible.name: "+" + Counted.of(strip.hiddenCount, "more tag")
            TapHandler { onTapped: strip.activated("") }


            Text {
                id: overflowTrailingMark
                anchors.centerIn: parent
                text: "+" + strip.hiddenCount
                color: Theme.foreground
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
                renderType: Text.NativeRendering
            }
        }
    }
}
