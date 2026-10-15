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
    font.pixelSize: 12
    color: Theme.foreground

    PointHandler {
        id: mouse
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    }
    Rectangle {
        objectName: "readOnlyFocusRing"
        parent: edit.focusFrame
        anchors.fill: parent
        anchors.margins: 2
        color: "transparent"
        border.width: 1
        border.color: Theme.accent
        visible: edit.activeFocus
        z: 2
        Accessible.ignored: true
    }
}
