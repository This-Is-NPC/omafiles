pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// One tab: the name, a close mark of its own, and before the name a folder or
// graph mark, or the colour square of the tag it lists. The current tab is
// `base`, so it joins the list under it; the others are `chrome` with dim
// names. The focused pane's current tab sets its name in DemiBold.
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
    /// The tab is the current one of the focused pane.
    property bool focusedPane: false
    /// The tag a tag listing shows, whose colour stands in for the mark.
    property string tag: ""
    signal activated
    signal closed
    signal menuRequested(real x, real y)

    readonly property int closeColumn: Theme.controlHeight
    // The narrowest a tab gets when the strip is short of room.
    readonly property int minimumWidth: 96

    implicitWidth: Math.max(minimumWidth, Math.min(220, label.implicitWidth + Theme.spacingS + Theme.iconSize
                                              + Theme.iconGap + closeColumn))
    implicitHeight: Theme.stripHeight
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
        objectName: "tabBody"
        focusRingSource: handle
        pointerFocus: handle
        anchors.fill: parent
        tabFocusable: false
        usable: true
        // Inside: the tab fills its clipped strip.
        ringInset: Theme.focusRing
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        tapped: function (point, button) {
            if (button === Qt.MiddleButton)
                handle.closed()
            else if (button === Qt.RightButton)
                handle.menuRequested(point.x, point.y)
            else
                handle.activated()
        }

        hasChrome: true
        color: handle.current ? Theme.base
             : body.hovered ? Theme.hoverChrome
                            : Theme.chrome

        Icon {
            id: mark
            visible: handle.tag === ""
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacingS
            anchors.verticalCenter: parent.verticalCenter
            kind: handle.kind === "graph" || handle.kind === "tag" ? handle.kind : "folder"
            ink: handle.current ? Theme.foreground : Theme.dim
        }

        Rectangle {
            objectName: "tabSwatch"
            visible: handle.tag !== ""
            anchors.centerIn: mark
            width: Theme.tagSwatch
            height: Theme.tagSwatch
            color: {
                // tagColour is no binding dependency; `raised` changes with
                // the theme, so reading it redraws the square on a theme switch.
                Theme.raised
                return Theme.tagColour(handle.tag)
            }
        }

        Text {
            id: label
            objectName: "tabTitle"
            anchors.left: mark.right
            anchors.leftMargin: Theme.iconGap
            anchors.right: parent.right
            anchors.rightMargin: handle.closeColumn
            anchors.verticalCenter: parent.verticalCenter
            text: handle.title
            color: handle.current ? Theme.foreground : Theme.dim
            elide: Text.ElideMiddle
            font.family: Theme.fontFamily
            font.weight: handle.current && handle.focusedPane ? Font.DemiBold : Font.Normal
            font.pixelSize: Theme.fontBody
            renderType: Text.NativeRendering
        }

        // Its own column, always there. A close mark that appears on hover is a
        // control the keyboard and the eye both have to guess at.
        Item {
            id: closeMark
            objectName: "tabClose"
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: handle.closeColumn

            Accessible.role: Accessible.Button
            Accessible.name: "Close tab"

            Text {
                objectName: "tabCloseMark"
                anchors.centerIn: parent
                text: "×"
                color: closeHover.hovered ? Theme.urgent : handle.current ? Theme.foreground : Theme.dim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                renderType: Text.NativeRendering
            }

            HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: handle.closed() }
        }

        // The edge between two tabs on the strip.
        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1
            color: Theme.line
        }
    }
}
