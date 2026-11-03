import QtQuick
import omafiles

TextEdit {
    id: edit
    property Item focusFrame: edit
    readOnly: true
    textFormat: TextEdit.PlainText
    activeFocusOnTab: true
    selectByKeyboard: true
    persistentSelection: true
    selectByMouse: mouse.active
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontBody
    color: Theme.foreground

    PointHandler {
        id: mouse
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    }
    // The frame is a scrolling viewport the text fills; its ring sits on the
    // frame's edge and the padding keeps the lines clear of it.
    topPadding: Theme.focusRing
    bottomPadding: Theme.focusRing

    FocusRing {
        parent: edit.focusFrame
        focused: edit.activeFocus
        inset: Theme.focusRing
    }
}
