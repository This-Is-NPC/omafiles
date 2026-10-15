pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as C

ListView {
    id: list
    objectName: "overlayList"
    clip: true
    reuseItems: true
    boundsBehavior: Flickable.StopAtBounds
    highlightMoveDuration: 0
    highlightResizeDuration: 0
    highlightFollowsCurrentItem: false

    property bool refreshing: false
    property bool positioning: false
    function revealCurrent() {
        positioning = true
        if (currentIndex >= 0)
            positionViewAtIndex(currentIndex, ListView.Contain)
        positioning = false
    }
    function adoptVisible() {
        if (refreshing || positioning || !currentItem)
            return
        const top = currentItem.y - contentY
        if (top < 0 || top + currentItem.height > height) {
            const index = indexAt(1, contentY + Math.min(28, height / 2))
            if (index >= 0)
                currentIndex = index
        }
    }
    onContentYChanged: if (!moving) adoptVisible()
    onMovementEnded: adoptVisible()
    function handleKey(event) {
        const page = Math.max(1, Math.floor(height / 28) - 1)
        let next = currentIndex
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            const reverse = event.key === Qt.Key_Backtab || (event.modifiers & Qt.ShiftModifier)
            currentIndex = count ? (currentIndex + (reverse ? count - 1 : 1)) % count : -1
            revealCurrent()
            event.accepted = true
            return true
        }
        if (event.key === Qt.Key_PageDown)
            next += page
        else if (event.key === Qt.Key_PageUp)
            next -= page
        else if (event.key === Qt.Key_Home)
            next = 0
        else if (event.key === Qt.Key_End)
            next = count - 1
        else
            return false
        currentIndex = count ? Math.max(0, Math.min(count - 1, next)) : -1
        revealCurrent()
        event.accepted = true
        return true
    }
    onCurrentIndexChanged: if (visible) Qt.callLater(revealCurrent)
    onHeightChanged: if (visible) Qt.callLater(revealCurrent)
    onCountChanged: if (visible) Qt.callLater(revealCurrent)
    onVisibleChanged: if (visible) Qt.callLater(revealCurrent)
    ImmediateWheel { anchors.fill: parent; view: list; step: 28 }
    C.ScrollBar.vertical: C.ScrollBar {}
}
