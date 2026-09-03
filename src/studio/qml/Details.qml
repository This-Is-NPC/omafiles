pragma ComponentBehavior: Bound

import QtQuick
import omafiles

// What the row under the cursor is.
//
// The tag vocabulary answers "what tags exist" and never "what am I looking
// at". Everything here is already on the row or one stat call away; the point
// of the panel is that you do not have to widen a column, switch to the details
// view, or open the file to read it.
//
// Nothing here takes the keyboard. It is a readout, and a Tab stop that only
// ever reports is a Tab stop in the way of the next one.
Item {
    id: details

    property var entry: ({})
    readonly property bool has: entry && entry.path !== undefined

    implicitHeight: body.implicitHeight

    Accessible.role: Accessible.StaticText
    Accessible.name: "Details"
    Accessible.description: details.has ? details.entry.name : "no row"
    Accessible.ignored: false
    Column {
        id: body
        width: parent.width
        spacing: 8

        Row {
            width: parent.width
            spacing: 7

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                kind: !details.has ? "file"
                    : details.entry.kind === "file" ? "file"
                    : details.entry.kind === "parent" ? "parent" : "folder"
                ink: !details.has ? Theme.dim
                   : details.entry.kind === "file" ? Theme.dim : Theme.accent
                opacity: details.has ? 1 : 0.4
            }
            Label {
                width: parent.width - 24
                anchors.verticalCenter: parent.verticalCenter
                text: details.has ? details.entry.name : "no row"
                color: details.has ? Theme.foreground : Theme.dim
                font.bold: details.has
            }
        }

        // The path is the one field worth wrapping: it is the answer to "which
        // of the four files with this name is it", and an elided one is not.
        Text {
            width: parent.width
            visible: details.has
            text: details.has ? details.entry.path : ""
            color: Theme.dim
            font.family: Theme.fontFamily
            font.pixelSize: 10
            wrapMode: Text.WrapAnywhere
            maximumLineCount: 3
            elide: Text.ElideRight
            renderType: Text.NativeRendering
        }

        Grid {
            width: parent.width
            visible: details.has
            columns: 2
            columnSpacing: 10
            rowSpacing: 4

            // `mime` and not `type`: the table's type column carries the
            // suffix, and one word for two different values is how you end up
            // reading `ods` in one place and `application/vnd.oasis…` in
            // another and believing they disagree.
            Label { quiet: true; text: "mime" }
            Label {
                width: body.width - 58
                text: details.has ? details.entry.mime : ""
                font.pixelSize: 11
            }

            Label { quiet: true; text: "size"; visible: sizeValue.visible }
            Label {
                id: sizeValue
                width: body.width - 58
                visible: details.has && !details.entry.directory
                text: details.has && details.entry.sizeText ? details.entry.sizeText : ""
                font.pixelSize: 11
            }

            // A folder has no size, and the number worth having in its place is
            // what is in it. It is counted for this row only -- the table does
            // not carry it, because a column of them is an open() per folder on
            // every rebuild, and on a big tree or a network mount that is the
            // list going slow to fill a column nobody asked for.
            Label { quiet: true; text: "items"; visible: itemsValue.visible }
            Label {
                id: itemsValue
                width: body.width - 58
                visible: details.has && details.entry.directory
                         && details.entry.items !== undefined && details.entry.items >= 0
                text: details.has && details.entry.items !== undefined
                      ? (details.entry.items === 1 ? "1 item"
                                                    : details.entry.items + " items")
                      : ""
                font.pixelSize: 11
            }

            Label { quiet: true; text: "changed"; visible: timeValue.visible }
            Label {
                id: timeValue
                width: body.width - 58
                visible: details.has && details.entry.mtimeText !== ""
                text: details.has ? details.entry.mtimeText : ""
                font.pixelSize: 11
            }

            Label { quiet: true; text: "marked"; visible: markValue.visible }
            Label {
                id: markValue
                visible: details.has && details.entry.marked === true
                text: "yes"
                color: Theme.accent
                font.pixelSize: 11
            }
        }

        Item {
            width: parent.width
            visible: details.has && details.entry.kind === "file"
            implicitHeight: tagWrap.implicitHeight

            Flow {
                id: tagWrap
                width: parent.width
                spacing: 4

                Label {
                    quiet: true
                    visible: details.has && details.entry.tags.length === 0
                    text: "no tags"
                    height: 18
                    verticalAlignment: Text.AlignVCenter
                }

                Repeater {
                    model: details.has ? details.entry.tags : []
                    Rectangle {
                        id: tagChip
                        required property string modelData
                        height: 18
                        width: mark.implicitWidth + 10
                        radius: Theme.rounding
                        color: Theme.fill(Theme.accent, 0.16)
                        border.width: 1
                        border.color: Theme.fill(Theme.accent, 0.45)
                        Text {
                            id: mark
                            anchors.centerIn: parent
                            text: tagChip.modelData
                            color: Theme.foreground
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                            renderType: Text.NativeRendering
                        }
                    }
                }
            }
        }
    }
}
