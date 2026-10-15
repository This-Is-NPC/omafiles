import QtQuick

MouseArea {
    id: wheelArea
    required property var view
    required property real step
    readonly property var styleHints: Qt.styleHints
    acceptedButtons: Qt.NoButton
    scrollGestureEnabled: false

    onWheel: function (event) {
        if (event.phase !== Qt.NoScrollPhase || event.angleDelta.y === 0) {
            event.accepted = false
            return
        }
        const view = wheelArea.view
        const low = view.originY
        const high = Math.max(low, low + view.contentHeight - view.height)
        const before = view.contentY
        view.cancelFlick()
        view.contentY = Math.max(low, Math.min(high, before
            - event.angleDelta.y / 120 * wheelArea.styleHints.wheelScrollLines * wheelArea.step))
        event.accepted = view.contentY !== before
    }
}
