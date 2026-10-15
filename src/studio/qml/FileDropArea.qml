pragma ComponentBehavior: Bound

import QtQuick
import omafiles

FileDropTarget {
    id: area
    objectName: "fileDropArea"
    property bool blocked: false
    enabled: directory !== "" && !blocked

    Rectangle {
        anchors.fill: parent
        visible: area.containsDrag
        color: Theme.fill(Theme.accent, 0.12)
        border.width: 2
        border.color: Theme.accent
        radius: Theme.rounding
    }
}
