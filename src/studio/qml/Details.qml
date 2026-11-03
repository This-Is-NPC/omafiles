pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import omafiles

// The right-hand panel: what the row under the cursor in the focused pane is.
//
// The tag vocabulary answers "what tags exist" and never "what am I looking
// at". Everything here is already on the row or one stat call away; the point
// of the panel is that you do not have to widen a column, switch to the details
// view, or open the file to read it.
//
// Readouts do not take Tab; permission bits and tag actions do.
Rectangle {
    id: details

    property var entry: ({})
    signal tagActivated(string tag)
    readonly property bool has: entry && entry.path !== undefined

    color: Theme.panel

    Accessible.role: Accessible.StaticText
    Accessible.name: "Details"
    Accessible.description: details.has ? details.entry.name : "no row"
    Accessible.ignored: false

    component Fact: Label {
        quiet: true
        Layout.alignment: Qt.AlignLeft | Qt.AlignBaseline
    }
    component Value: Label {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignLeft | Qt.AlignBaseline
    }

    OverlayScroll {
        id: scroll
        objectName: "detailsScroll"
        focusOnScroll: true
        anchors.fill: parent
        anchors.topMargin: Theme.inset
        anchors.bottomMargin: Theme.inset
        contentHeight: column.implicitHeight

        Column {
            id: column
            width: scroll.width

            SectionHeading { width: parent.width; usable: false; text: "details" }

            Column {
                id: body
                x: Theme.inset
                width: parent.width - 2 * Theme.inset
                spacing: Theme.spacingS

                Row {
                    width: parent.width
                    spacing: Theme.iconGap

                    Icon {
                        objectName: "detailsIcon"
                        anchors.verticalCenter: parent.verticalCenter
                        kind: !details.has ? "file"
                            : details.entry.kind === "file" ? "file"
                            : details.entry.kind === "parent" ? "parent" : "folder"
                        ink: !details.has ? Theme.dim
                           : details.entry.kind === "file" ? Theme.dim : Theme.accent
                        opacity: details.has ? 1 : 0.4
                    }
                    Label {
                        objectName: "detailsName"
                        width: parent.width - Theme.iconSize - Theme.iconGap
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
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WrapAnywhere
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    renderType: Text.NativeRendering
                }

                // The name column is as wide as its widest name, each name sits on its
                // value's baseline, and a value too long for the rest elides.
                GridLayout {
                    width: parent.width
                    visible: details.has
                    columns: 2
                    columnSpacing: Theme.spacingS
                    rowSpacing: Theme.spacingXs

                    // `mime` and not `type`: the table's type column carries the
                    // suffix, and one word for two different values is how you end up
                    // reading `ods` in one place and `application/vnd.oasis…` in
                    // another and believing they disagree.
                    Fact { text: "mime" }
                    Value {
                        text: details.has ? details.entry.mime : ""
                    }

                    Fact { text: "size"; visible: sizeValue.visible }
                    Value {
                        id: sizeValue
                        visible: details.has && !details.entry.directory
                        text: details.has && details.entry.sizeText ? details.entry.sizeText : ""
                    }

                    // A folder has no size, and the number worth having in its place is
                    // what is in it. It is counted for this row only -- the table does
                    // not carry it, because a column of them is an open() per folder on
                    // every rebuild, and on a big tree or a network mount that is the
                    // list going slow to fill a column nobody asked for.
                    Fact { text: "items"; visible: itemsValue.visible }
                    Value {
                        id: itemsValue
                        visible: details.has && details.entry.directory
                                 && details.entry.items !== undefined && details.entry.items >= 0
                        text: details.has && details.entry.items !== undefined
                              ? (details.entry.items === 1 ? "1 item"
                                                            : details.entry.items + " items")
                              : ""
                    }

                    Fact { text: "changed"; visible: timeValue.visible }
                    Value {
                        id: timeValue
                        visible: details.has && details.entry.mtimeText !== ""
                        text: details.has ? details.entry.mtimeText : ""
                    }

                    // The two facts a permissions dialog exists for, read and set here
                    // instead: whose it is, and what they and everyone else may do.
                    // Yours is `you`, and only somebody else's names the user and
                    // the group -- the question the line answers is whether it is
                    // yours, and a name you would have to recognise as your own is
                    // a slower way of saying so.
                    Fact { text: "owner"; visible: ownerValue.visible }
                    Value {
                        id: ownerValue
                        visible: details.has && details.entry.owner !== ""
                        text: !details.has ? ""
                            : details.entry.mine ? "you"
                            : details.entry.owner + " · " + details.entry.group
                    }

                    // A click on a bit runs `chmod` on the row. Greyed where the row is
                    // not yours, since only the owner or root may; and on a link, whose
                    // bits are the target's and which `chmod` refuses to change through.
                    Fact {
                        text: "mode"
                        visible: modeValue.visible
                        Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    }
                    ModeBits {
                        id: modeValue
                        Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        objectName: "detailsMode"
                        visible: details.has && details.entry.mode !== ""
                        value: details.has && details.entry.permissions !== undefined
                               ? details.entry.permissions : 0
                        editable: details.has && details.entry.mine === true
                                  && details.entry.link !== true
                        onToggled: function (value) {
                            IndexModel.changeMode([details.entry.path], modeValue.octalOf(value))
                        }
                    }

                    Fact { text: "marked"; visible: markValue.visible }
                    Value {
                        id: markValue
                        visible: details.has && details.entry.marked === true
                        text: "yes"
                        color: Theme.accent
                    }
                }

                Item {
                    width: parent.width
                    visible: details.has && details.entry.kind === "file"
                    implicitHeight: tagWrap.implicitHeight

                    Flow {
                        id: tagWrap
                        width: parent.width
                        spacing: Theme.spacingXs

                        Label {
                            quiet: true
                            visible: details.has && details.entry.tags.length === 0
                            text: "no tags"
                            height: Theme.chipHeight
                            verticalAlignment: Text.AlignVCenter
                        }

                        Repeater {
                            model: details.has ? details.entry.tags : []
                            FocusableControl {
                                id: tagChip
                                objectName: "detailsTagChip"
                                Accessible.role: Accessible.Button
                                Accessible.name: "Edit tag " + tagChip.modelData
                                onActivated: details.tagActivated(tagChip.modelData)
                                required property string modelData
                                height: Theme.chipHeight
                                width: mark.implicitWidth + 2 * Theme.spacingXs
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
                                    font.pixelSize: Theme.fontSmall
                                    renderType: Text.NativeRendering
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
