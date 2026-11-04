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
        // An outline: the files under it stay readable while a drag hovers.
        color: "transparent"
        border.width: 2
        border.color: Theme.accent
        radius: Theme.rounding
    }
}
