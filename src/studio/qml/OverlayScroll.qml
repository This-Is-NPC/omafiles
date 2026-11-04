pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import QtQuick.Window
import omafiles

Flickable {
    id: viewport
    objectName: "overlayScroll"
    property bool focusOnScroll: false
    /// Room either side of the content for the focus ring of a control at its
    /// edge; a panel whose rows fill it keeps their rings inside them instead.
    property real gutter: Theme.focusRing
    clip: true
    leftMargin: gutter
    // The gutter, then the strip the scroll bar keeps.
    rightMargin: gutter + Theme.scrollBarWidth
    contentWidth: width - 2 * gutter - Theme.scrollBarWidth
    boundsBehavior: Flickable.StopAtBounds

    function revealItem(item) {
        if (!item)
            return
        const rectangle = item.height > height && item.cursorRectangle !== undefined
            ? item.cursorRectangle : Qt.rect(0, 0, item.width, item.height)
        revealRect(item, rectangle)
    }
    function revealRect(item, rectangle) {
        if (!item || !item.visible)
            return
        let ancestor = item
        while (ancestor && ancestor !== contentItem)
            ancestor = ancestor.parent
        if (!ancestor)
            return
        // The gutter above and below too, where the item's focus ring is,
        // when both fit.
        const room = rectangle.height + 2 * gutter <= height ? gutter : 0
        const point = item.mapToItem(contentItem, rectangle.x, rectangle.y - room)
        const extent = rectangle.height + 2 * room
        let next = contentY
        if (point.y < next)
            next = point.y
        else if (point.y + extent > next + height)
            next = point.y + Math.min(extent, height) - height
        contentY = Math.max(0, Math.min(next, Math.max(0, contentHeight - height)))
    }
    function revealFocus() {
        const window = viewport.Window.window
        if (window)
            revealItem(window.activeFocusItem)
    }
    function handleKey(event) {
        if (event.modifiers !== Qt.NoModifier || contentHeight <= height)
            return false
        let next = contentY
        if (event.key === Qt.Key_PageDown)
            next += height * 0.9
        else if (event.key === Qt.Key_PageUp)
            next -= height * 0.9
        else if (event.key === Qt.Key_Down)
            next += 28
        else if (event.key === Qt.Key_Up)
            next -= 28
        else if (event.key === Qt.Key_Home)
            next = 0
        else if (event.key === Qt.Key_End)
            next = contentHeight
        else
            return false
        const window = viewport.Window.window
        let ancestor = window ? window.activeFocusItem : null
        while (ancestor && ancestor !== contentItem)
            ancestor = ancestor.parent
        if (focusOnScroll && ancestor)
            forceActiveFocus()
        cancelFlick()
        contentY = Math.max(0, Math.min(next, Math.max(0, contentHeight - height)))
        event.accepted = true
        return true
    }
    Keys.onPressed: function (event) { viewport.handleKey(event) }
    onWidthChanged: if (visible) Qt.callLater(revealFocus)
    onHeightChanged: if (visible) Qt.callLater(revealFocus)
    onContentHeightChanged: if (visible) Qt.callLater(revealFocus)
    onVisibleChanged: if (visible) Qt.callLater(revealFocus)
    Connections {
        objectName: "overlayFocusObserver"
        target: viewport.Window.window
        enabled: viewport.visible
        function onActiveFocusItemChanged() { Qt.callLater(viewport.revealFocus) }
    }
    ImmediateWheel { anchors.fill: parent; view: viewport; step: 28 }
    FocusRing {
        parent: viewport
        focused: viewport.activeFocus
        inset: Theme.focusRing
    }
    T.ScrollBar.vertical: ScrollBar {}
}
