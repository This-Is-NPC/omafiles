pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// One tab: a box with the name in it and a close mark of its own.
//
// The close mark keeps its own column at the right, and the title is elided
// before it reaches that column. A mark that floats over the last few letters
// of the name is both an unreadable name and a target you cannot be sure you
// are hitting.
//
// Uses FocusableControl for the inner focus ring and pointer shell; the
// FocusScope keeps PageTab focus and the Delete shortcut.
FocusScope {
    id: handle

    property string title: ""
    property string kind: "files"
    property bool current: false
    signal activated
    signal closed
    signal menuRequested(real x, real y)

    readonly property int closeColumn: 22

    implicitWidth: Math.max(96, Math.min(220, label.implicitWidth + 30 + closeColumn))
    implicitHeight: 24
    width: implicitWidth
    height: implicitHeight

    Accessible.role: Accessible.PageTab
    Accessible.name: handle.title
    Accessible.checked: handle.current
    Accessible.focusable: true

    activeFocusOnTab: true
    Keys.onSpacePressed: function (event) { handle.activated(); event.accepted = true }
    Keys.onReturnPressed: function (event) { handle.activated(); event.accepted = true }
    Keys.onEnterPressed: function (event) { handle.activated(); event.accepted = true }
    Keys.onDeletePressed: function (event) { handle.closed(); event.accepted = true }

    FocusableControl {
        id: body
        focusRingSource: handle
        pointerFocus: handle
        anchors.fill: parent
        tabFocusable: false
        usable: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        tapped: function (point, button) {
            if (button === Qt.MiddleButton)
                handle.closed()
            else if (button === Qt.RightButton)
                handle.menuRequested(point.x, point.y)
            else
                handle.activated()
        }

        radius: Theme.rounding
        color: handle.current ? Theme.fill(Theme.accent, 0.18)
             : body.hovered ? body.hoverChromeFill
                            : Theme.fill(Theme.foreground, 0.04)
        border.width: 1
        border.color: handle.current ? Theme.accent
                    : body.hovered ? body.hoverChromeBorder
                                   : Theme.fill(Theme.foreground, 0.18)

        Icon {
            id: mark
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            kind: handle.kind === "graph" || handle.kind === "tag" ? handle.kind : "folder"
            ink: handle.current ? Theme.foreground : Theme.dim
        }

        Text {
            id: label
            anchors.left: mark.right
            anchors.leftMargin: 6
            anchors.right: parent.right
            anchors.rightMargin: handle.closeColumn
            anchors.verticalCenter: parent.verticalCenter
            text: handle.title
            color: Theme.foreground
            opacity: handle.current ? 1 : 0.75
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 11
            renderType: Text.NativeRendering
        }

        // Its own column, always there. A close mark that appears on hover is a
        // control the keyboard and the eye both have to guess at.
        Item {
            id: closeMark
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: handle.closeColumn

            Text {
                anchors.centerIn: parent
                text: "×"
                color: closeHover.hovered ? Theme.urgent : Theme.foreground
                opacity: closeHover.hovered ? 1 : (handle.current ? 0.7 : 0.45)
                font.family: Theme.fontFamily
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }

            HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: handle.closed() }
        }
    }
}
