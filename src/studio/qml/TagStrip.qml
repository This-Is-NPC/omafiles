pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// The tags on one row, right-aligned, inside a width it is not allowed to
// exceed.
//
// The strip ends at the row's right margin and grows leftwards, and the name is
// anchored to the strip, so the two share the row instead of one of them
// winning: drawn from wherever the name ends, a row with six tags runs the last
// of them off the right edge with no way to scroll to it.
//
// A tag that does not fit is dropped whole rather than sliced through a word,
// and it is dropped with opacity rather than `visible`, because a Row closes
// the gap left by an invisible child: the next tag would slide in, fit, appear,
// push the first one back out, and the row would flicker between two layouts.
// When some tags are hidden, a trailing or leading +N chip states how many.
Item {
    id: strip

    objectName: "tagStrip"

    property var tags: []
    /// The widest this is allowed to be. The row decides it, because the row is
    /// what has to give up the space.
    property real limit: 240
    /// Packed against the right edge in the list, where the strip floats at the
    /// end of the row; against the left in the details table, where it is a
    /// column and a column starts where its heading does.
    property bool alignRight: true
    /// How many tags are hidden behind the +N chip.
    property int hiddenCount: 0
    /// How many tag chips are drawn at full opacity.
    property int visibleCount: 0

    property int firstVisible: 0
    property int lastVisible: -1

    implicitWidth: Math.min(row.width, limit)
    width: implicitWidth
    implicitHeight: 17
    height: implicitHeight
    clip: true

    onWidthChanged: Qt.callLater(recomputeLayout)
    onLimitChanged: Qt.callLater(recomputeLayout)
    onTagsChanged: Qt.callLater(recomputeLayout)
    onAlignRightChanged: Qt.callLater(recomputeLayout)
    Component.onCompleted: recomputeLayout()

    Text {
        id: chipMetrics
        visible: false
        font.family: Theme.fontFamily
        font.pixelSize: 10
    }

    Text {
        id: overflowMetrics
        visible: false
        font.family: Theme.fontFamily
        font.pixelSize: 10
    }

    function chipWidth(text) {
        chipMetrics.text = text
        return chipMetrics.implicitWidth + 10
    }

    function overflowWidth(count) {
        if (count <= 0)
            return 0
        overflowMetrics.text = "+" + count
        return overflowMetrics.implicitWidth + 10
    }

    function recomputeLayout() {
        const tagList = strip.tags
        const n = tagList.length
        const spacing = row.spacing
        const avail = strip.limit

        if (n === 0 || avail <= 0) {
            strip.hiddenCount = 0
            strip.visibleCount = 0
            strip.firstVisible = 0
            strip.lastVisible = -1
            return
        }

        const widths = []
        let allWidth = 0
        for (let i = 0; i < n; ++i) {
            widths.push(chipWidth(tagList[i]))
            allWidth += widths[i] + (i > 0 ? spacing : 0)
        }

        if (allWidth <= avail) {
            strip.hiddenCount = 0
            strip.visibleCount = n
            strip.firstVisible = 0
            strip.lastVisible = n - 1
            return
        }

        if (strip.alignRight) {
            for (let vis = n; vis >= 1; --vis) {
                const hidden = n - vis
                const start = hidden
                let used = overflowWidth(hidden)
                if (hidden > 0)
                    used += spacing
                for (let i = start; i < n; ++i)
                    used += widths[i] + (i > start ? spacing : 0)
                if (used <= avail) {
                    strip.hiddenCount = hidden
                    strip.visibleCount = vis
                    strip.firstVisible = start
                    strip.lastVisible = n - 1
                    return
                }
            }
        } else {
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
                    strip.firstVisible = 0
                    strip.lastVisible = vis - 1
                    return
                }
            }
        }

        if (overflowWidth(n) <= avail) {
            strip.hiddenCount = n
            strip.visibleCount = 0
            strip.firstVisible = strip.alignRight ? n : 0
            strip.lastVisible = -1
            return
        }

        strip.hiddenCount = n
        strip.visibleCount = 0
        strip.firstVisible = strip.alignRight ? n : 0
        strip.lastVisible = -1
    }

    Row {
        id: row
        anchors.right: strip.alignRight ? parent.right : undefined
        anchors.left: strip.alignRight ? undefined : parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4

        Rectangle {
            id: overflowLeading
            objectName: "tagStripOverflowLeading"
            visible: strip.alignRight && strip.hiddenCount > 0
            height: 17
            width: overflowLeadingMark.implicitWidth + 10
            radius: Theme.rounding
            color: Theme.fill(Theme.accent, 0.14)
            border.width: 1
            border.color: Theme.fill(Theme.accent, 0.35)
            Accessible.role: Accessible.StaticText
            Accessible.name: "+" + strip.hiddenCount + " more tags"


            Text {
                id: overflowLeadingMark
                anchors.centerIn: parent
                text: "+" + strip.hiddenCount
                color: Theme.foreground
                opacity: 0.85
                font.family: Theme.fontFamily
                font.pixelSize: 10
                renderType: Text.NativeRendering
            }
        }

        Repeater {
            model: strip.tags

            Rectangle {
                id: chip
                required property int index
                required property string modelData
                readonly property bool shown: strip.hiddenCount === 0
                    || (strip.alignRight ? chip.index >= strip.firstVisible
                                         : chip.index <= strip.lastVisible)
                height: 17
                width: shown ? mark.implicitWidth + 10 : 0
                radius: Theme.rounding
                color: Theme.fill(Theme.accent, 0.14)
                border.width: 1
                border.color: Theme.fill(Theme.accent, 0.35)
                opacity: shown ? 1 : 0

                Text {
                    id: mark
                    anchors.centerIn: parent
                    text: chip.modelData
                    color: Theme.foreground
                    opacity: 0.85
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    renderType: Text.NativeRendering
                }
            }
        }

        Rectangle {
            id: overflowTrailing
            objectName: "tagStripOverflowTrailing"
            visible: !strip.alignRight && strip.hiddenCount > 0
            height: 17
            width: overflowTrailingMark.implicitWidth + 10
            radius: Theme.rounding
            color: Theme.fill(Theme.accent, 0.14)
            border.width: 1
            border.color: Theme.fill(Theme.accent, 0.35)
            Accessible.role: Accessible.StaticText
            Accessible.name: "+" + strip.hiddenCount + " more tags"


            Text {
                id: overflowTrailingMark
                anchors.centerIn: parent
                text: "+" + strip.hiddenCount
                color: Theme.foreground
                opacity: 0.85
                font.family: Theme.fontFamily
                font.pixelSize: 10
                renderType: Text.NativeRendering
            }
        }
    }
}
