import QtQuick
import omafiles

// Marks the rows a left-button drag over the view draws a rectangle across, the
// way Dolphin does: from anywhere but the header or a row's grip -- the
// delegate's child named "grip", its icon and name. A plain drag replaces the
// marks, Shift adds to them, Ctrl toggles.
Item {
    id: band

    /// A ListView or a GridView. Their shared base has no QML name.
    required property var view
    required property FolderModel folder
    /// The view's cells, in content coordinates from the first row's corner.
    required property real cellWidth
    required property real cellHeight
    property int columns: 1
    /// True from the moment the band is drawn until it is released or cancelled.
    readonly property bool live: band.from !== null

    signal started()

    // `from` is where the drag began, in the view's content, so the band stays
    // on the rows it started on while the view scrolls under it.
    property var from: null
    property point to
    property var before: []
    property var kept: []
    property bool toggle: false

    function cancel() {
        band.folder.markRows([], band.before, false)
        band.from = null
    }

    function begin(pressed, modifiers) {
        if (band.grips(pressed))
            return
        band.before = band.folder.markedPaths()
        band.toggle = (modifiers & Qt.ControlModifier) !== 0
        band.kept = band.toggle || (modifiers & Qt.ShiftModifier) ? band.before : []
        band.from = band.view.contentItem.mapFromItem(band, pressed)
        band.started()
    }

    function grips(point) {
        const header = band.view.headerItem
        if (header && header.contains(header.mapFromItem(band, point)))
            return true
        const at = band.view.contentItem.mapFromItem(band, point)
        const item = band.view.itemAt(at.x, at.y)
        if (!item)
            return false
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (child.objectName === "grip")
                return child.contains(child.mapFromItem(band, point))
        }
        return false
    }

    function follow() {
        if (!band.live)
            return
        const end = band.view.contentItem.mapFromItem(band, band.to)
        const left = Math.min(band.from.x, end.x)
        const right = Math.max(band.from.x, end.x)
        const top = Math.min(band.from.y, end.y)
        const bottom = Math.max(band.from.y, end.y)
        const firstColumn = Math.max(0, Math.floor(left / band.cellWidth))
        const lastColumn = Math.min(band.columns - 1, Math.floor(right / band.cellWidth))
        const rows = []
        for (let line = Math.max(0, Math.floor(top / band.cellHeight));
             line <= Math.floor(bottom / band.cellHeight); ++line) {
            for (let column = firstColumn; column <= lastColumn; ++column) {
                const row = line * band.columns + column
                if (row < band.folder.count)
                    rows.push(row)
            }
        }
        band.folder.markRows(rows, band.kept, band.toggle)
    }

    DragHandler {
        id: drag
        target: null
        acceptedButtons: Qt.LeftButton
        onActiveChanged: {
            band.to = drag.centroid.position
            if (drag.active)
                band.begin(drag.centroid.pressPosition, drag.centroid.modifiers)
            else
                band.from = null
        }
        onCentroidChanged: {
            band.to = drag.centroid.position
            band.follow()
        }
    }

    // How far past an edge the pointer is decides the speed, as in GTK's list
    // views: 30 px of margin, a third of the depth per frame.
    Timer {
        readonly property real depth: band.to.y < 30 ? band.to.y - 30
                                    : band.to.y > band.height - 30 ? band.to.y - band.height + 30
                                    : 0
        interval: 16
        repeat: true
        running: band.live && depth !== 0
        onTriggered: {
            const view = band.view
            const floor = view.originY
            const ceiling = Math.max(floor, view.originY + view.contentHeight - view.height)
            view.contentY = Math.max(floor, Math.min(ceiling, view.contentY + depth / 3))
            band.follow()
        }
    }

    Rectangle {
        visible: band.live
        x: band.live ? Math.min(band.from.x - band.view.contentX, band.to.x) : 0
        y: band.live ? Math.min(band.from.y - band.view.contentY, band.to.y) : 0
        width: band.live ? Math.abs(band.from.x - band.view.contentX - band.to.x) : 0
        height: band.live ? Math.abs(band.from.y - band.view.contentY - band.to.y) : 0
        color: Theme.fill(Theme.accent, 0.16)
        border.width: 1
        border.color: Theme.accent
    }
}
