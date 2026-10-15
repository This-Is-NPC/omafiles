import QtQuick
import omafiles

Rectangle {
    property real preferredWidth: 460
    property real preferredHeight: 300
    property real margin: 16
    anchors.centerIn: parent
    width: Math.max(0, Math.min(preferredWidth, parent.width - 2 * margin))
    height: Math.max(0, Math.min(preferredHeight, parent.height - 2 * margin))
    radius: Theme.rounding
    color: Theme.panel
    border.width: 1
    border.color: Theme.accent
}
