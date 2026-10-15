pragma ComponentBehavior: Bound

import QtQuick

TapHandler {
    id: tap
    acceptedButtons: Qt.LeftButton | Qt.RightButton
    signal clicked(point position, int modifiers)
    signal contextRequested(point position)
    signal activated()

    onTapped: function (eventPoint, button) {
        if (button === Qt.RightButton)
            contextRequested(eventPoint.position)
        else
            clicked(eventPoint.position, point.modifiers)
    }
    onDoubleTapped: function (eventPoint, button) {
        if (button !== Qt.RightButton) activated()
    }
    onLongPressed: {
        if (point.device && (point.device.type === PointerDevice.TouchScreen
                || point.device.type === PointerDevice.Stylus))
            contextRequested(point.position)
    }
}
