.pragma library

function move(event, target, previous) {
    event.accepted = false
    const reverse = event.key === Qt.Key_Backtab || (event.modifiers & Qt.ShiftModifier) !== 0
    if (previous !== undefined && reverse)
        target = previous
    if (!target)
        return
    if (!target.visible || !target.enabled)
        target = target.nextItemInFocusChain(!reverse)
    target.forceActiveFocus(reverse ? Qt.BacktabFocusReason : Qt.TabFocusReason)
    event.accepted = true
}

function modifiedTextNavigation(event) {
    return event.modifiers !== Qt.NoModifier
        && (event.key === Qt.Key_Up || event.key === Qt.Key_Down
            || event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown
            || event.key === Qt.Key_Home || event.key === Qt.Key_End)
}
