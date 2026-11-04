pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import omafiles

// The dock under the panes: what the row under the cursor in the focused pane
// is, its tags, and what can be done with it. Three columns split by hairlines:
// the file, its tags, its actions.
//
// Readouts do not take Tab; permission bits, tag chips and buttons do.
Rectangle {
    id: details

    property var entry: ({})
    /// The keys of the "+ Tag" button and of the three actions.
    property string addTagKey: ""
    property string openKey: ""
    property string renameKey: ""
    readonly property bool has: entry && entry.path !== undefined
    // A column the dock has no room for is dropped whole, the actions first:
    // each has its key. The file column keeps room for its facts.
    readonly property real fileRoom: 400
    readonly property real tagsWidth: 248
    readonly property real actionsWidth: 156
    readonly property bool roomForTags: width >= fileRoom + 1 + tagsWidth
    readonly property bool roomForActions: roomForTags && width >= fileRoom + 2 + tagsWidth + actionsWidth
    readonly property bool onRow: details.has && (details.entry.kind === "file" || details.entry.kind === "dir")

    signal tagOpened(string tag)
    signal tagRemoved(string tag)
    signal tagAddRequested()
    signal openRequested()
    signal openWithRequested(Item button)
    signal renameRequested()

    color: Theme.chrome
    // The file column at its tallest -- a path on all three lines -- so the
    // dock keeps its height while the cursor moves.
    implicitHeight: Math.ceil(2 * Theme.spacingL + name.implicitHeight + Theme.spacingXs
                              + 3 * pathLine.lineSpacing + Theme.spacingS + facts.implicitHeight)

    Accessible.role: Accessible.StaticText
    Accessible.name: "Details"
    Accessible.description: details.has ? details.entry.name : "no row"
    Accessible.ignored: false

    // The path as it is drawn: a line may break only after a `/`. U+200B after
    // each `/` is the break, U+2060 between the other characters forbids the
    // breaks a space or a hyphen would give.
    function breakable(path) {
        return path.split("/").map(part => Array.from(part).join("\u2060")).join("/\u200B")
    }
    // The path on at most three lines: whole folders are dropped from the
    // middle, behind one `…`, until the rest fits.
    function fitPath() {
        // Read off the entry and not `has`, which may not have caught up with
        // it yet inside `onEntryChanged`.
        const full = details.entry ? details.entry.path : undefined
        if (full === undefined) {
            path.text = ""
            return
        }
        const parts = full.split("/")
        pathProbe.text = breakable(parts.join("/"))
        // The first part is the empty one before the root's `/`, and the last
        // is the name: both stay.
        for (let dropped = 1; pathProbe.lineCount > 3 && parts.length - dropped >= 2; ++dropped) {
            const head = Math.max(1, Math.floor((parts.length - dropped) / 2))
            const shown = parts.slice(0, head).concat(["…"], parts.slice(head + dropped))
            pathProbe.text = breakable(shown.join("/"))
        }
        path.text = pathProbe.text
    }
    onEntryChanged: fitPath()
    Component.onCompleted: fitPath()

    component Fact: Label {
        quiet: true
        Layout.alignment: Qt.AlignLeft | Qt.AlignBaseline
    }
    component Value: Label {
        Layout.fillWidth: true
        elide: Text.ElideRight
        Layout.alignment: Qt.AlignLeft | Qt.AlignBaseline
    }

    component Action: Chip {
        x: Theme.spacingL
        width: parent.width - 2 * Theme.spacingL
        usable: details.has
        showsKey: true
    }

    FontMetrics {
        id: pathLine
        font: path.font
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ------------------------------------------------------------ file
        Item {
            objectName: "detailsFile"
            Layout.fillWidth: true
            Layout.fillHeight: true

            Rectangle {
                id: square
                x: Theme.spacingL
                y: Theme.spacingL
                width: 3 * Theme.iconSize
                height: 3 * Theme.iconSize
                radius: Theme.rounding
                color: Theme.raised
                border.width: 1
                border.color: Theme.line

                Icon {
                    objectName: "detailsIcon"
                    anchors.centerIn: parent
                    width: 2 * Theme.iconSize
                    height: 2 * Theme.iconSize
                    kind: !details.has ? "file"
                        : details.entry.kind === "file" ? "file"
                        : details.entry.kind === "parent" ? "parent" : "folder"
                    ink: details.has && details.entry.kind !== "file" ? Theme.accent : Theme.dim
                }
            }

            Column {
                id: about
                x: square.x + square.width + Theme.spacingL
                y: Theme.spacingL
                width: parent.width - x - Theme.spacingL

                Label {
                    id: name
                    objectName: "detailsName"
                    width: parent.width
                    text: details.has ? details.entry.name : "no row"
                    elide: Text.ElideMiddle
                    color: details.has ? Theme.foreground : Theme.dim
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                }

                Item { width: 1; height: Theme.spacingXs }

                Label {
                    id: path
                    objectName: "detailsPath"
                    width: parent.width
                    visible: details.has
                    quiet: true
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    onWidthChanged: details.fitPath()
                }
                Label {
                    id: pathProbe
                    visible: false
                    width: path.width
                    quiet: true
                    wrapMode: Text.Wrap
                }

                Item { width: 1; height: Theme.spacingS }

                // Two facts to a row, each name on its value's baseline; a
                // value too long for its cell elides.
                GridLayout {
                    id: facts
                    // The widest caption, the bits, and the Marked pair.
                    readonly property bool modeRowFits: width >= modifiedFact.implicitWidth + modeValue.implicitWidth
                        + markedFact.implicitWidth + markedValue.implicitWidth + 3 * columnSpacing
                    width: parent.width
                    visible: details.has
                    columns: 4
                    columnSpacing: Theme.spacingS
                    rowSpacing: Theme.spacingXs

                    // The type is the mime type: the table's type column carries
                    // the suffix.
                    Fact { text: "Type" }
                    Value { text: details.has ? details.entry.mime : "" }

                    // A folder has no size, and the number worth having in its
                    // place is what is in it. It is counted for this row only: a
                    // column of them would be an open() per folder on every rebuild.
                    Fact { text: details.has && details.entry.directory ? "Items" : "Size" }
                    Value {
                        text: !details.has ? ""
                            : !details.entry.directory ? (details.entry.sizeText || "")
                            : details.entry.items !== undefined && details.entry.items >= 0
                              ? Counted.of(details.entry.items, "item") : ""
                    }

                    Fact { id: modifiedFact; text: "Modified" }
                    Value { text: details.has ? details.entry.mtimeText : "" }

                    // Whose it is and what they and everyone else may do. Yours
                    // is `you`; only somebody else's names the user and group.
                    Fact { text: "Owner" }
                    Value {
                        objectName: "detailsOwner"
                        text: !details.has || details.entry.owner === "" ? ""
                            : details.entry.mine ? "you"
                            : details.entry.owner + " · " + details.entry.group
                    }

                    // A click on a bit runs `chmod` on the row. Greyed where the
                    // row is not yours, since only the owner or root may; and on
                    // a link, whose bits are the target's. The row is dropped
                    // whole where the column cannot hold it, so no bit is cut.
                    Fact {
                        text: "Mode"
                        visible: facts.modeRowFits
                        Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    }
                    // The cell stays where the bits hide, so the pairs keep their places.
                    Item {
                        visible: facts.modeRowFits
                        Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        implicitWidth: modeValue.implicitWidth
                        implicitHeight: modeValue.implicitHeight

                        ModeBits {
                            id: modeValue
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
                    }

                    Fact { id: markedFact; text: "Marked"; visible: facts.modeRowFits }
                    Value {
                        id: markedValue
                        objectName: "detailsMarked"
                        visible: facts.modeRowFits
                        text: details.has && details.entry.marked === true ? "yes" : "no"
                        color: text === "yes" ? Theme.accent : Theme.foreground
                    }
                }
            }
        }

        Rule {
            vertical: true
            visible: scroll.visible
            Layout.fillHeight: true
            Layout.topMargin: Theme.spacingL
            Layout.bottomMargin: Theme.spacingL
            Layout.preferredWidth: 1
        }

        // ------------------------------------------------------------ tags
        OverlayScroll {
            id: scroll
            objectName: "detailsScroll"
            visible: details.roomForTags
            focusOnScroll: true
            Layout.preferredWidth: details.tagsWidth
            Layout.fillHeight: true
            Layout.topMargin: Theme.spacingM
            Layout.bottomMargin: Theme.spacingM
            contentHeight: tagColumn.implicitHeight

            Column {
                id: tagColumn
                width: scroll.contentWidth
                spacing: Theme.spacingS

                SectionHeading { width: parent.width; text: "Tags" }

                Flow {
                    x: Theme.inset
                    width: parent.width - 2 * Theme.inset
                    spacing: Theme.spacingXs

                    Label {
                        quiet: true
                        visible: details.onRow && details.entry.tags.length === 0
                        text: "no tags"
                        height: Theme.chipHeight
                        verticalAlignment: Text.AlignVCenter
                    }

                    Repeater {
                        model: details.onRow ? details.entry.tags : []

                        // The body opens the tag's view; the × at its end, and
                        // Delete while it has the keyboard, remove the tag.
                        FocusableControl {
                            id: tagChip
                            objectName: "detailsTagChip"
                            required property string modelData
                            readonly property Item removeTarget: cross
                            Accessible.role: Accessible.Button
                            Accessible.name: "Tag " + tagChip.modelData
                            Accessible.description: "Enter opens the tag, Delete removes it"
                            width: face.width
                            height: face.height
                            tapped: function (position, button) {
                                if (position.x >= cross.x - Theme.spacingXs)
                                    details.tagRemoved(tagChip.modelData)
                                else
                                    details.tagOpened(tagChip.modelData)
                            }
                            onActivated: details.tagOpened(tagChip.modelData)
                            Keys.onDeletePressed: function (event) {
                                details.tagRemoved(tagChip.modelData)
                                event.accepted = true
                            }

                            TagChip {
                                id: face
                                tag: tagChip.modelData
                                width: implicitWidth + cross.width + Theme.spacingXs

                                Text {
                                    id: cross
                                    objectName: "detailsTagRemove"
                                    anchors.right: parent.right
                                    anchors.rightMargin: Theme.spacingXs
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Theme.removeGlyph
                                    horizontalAlignment: Text.AlignHCenter
                                    text: "×"
                                    color: Theme.foreground
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.removeGlyph
                                    renderType: Text.NativeRendering
                                }
                            }
                        }
                    }
                }

                Chip {
                    objectName: "detailsAddTag"
                    x: Theme.inset
                    usable: details.onRow
                    label: "+ Tag"
                    caption: details.onRow ? "Add a tag to the row or the marked files"
                                           : "Select a file to tag it"
                    key: details.addTagKey
                    showsKey: true
                    onClicked: details.tagAddRequested()
                }

                Label {
                    x: Theme.inset
                    width: parent.width - 2 * Theme.inset
                    quiet: true
                    wrapMode: Text.Wrap
                    text: "Del removes the tag in focus"
                }
            }
        }

        Rule {
            vertical: true
            visible: actions.visible
            Layout.fillHeight: true
            Layout.topMargin: Theme.spacingL
            Layout.bottomMargin: Theme.spacingL
            Layout.preferredWidth: 1
        }

        // --------------------------------------------------------- actions
        Column {
            id: actions
            objectName: "detailsActions"
            visible: details.roomForActions
            Layout.preferredWidth: details.actionsWidth
            Layout.fillHeight: true
            Layout.topMargin: Theme.spacingL
            spacing: Theme.spacingS

            Action {
                objectName: "detailsOpen"
                label: "Open"
                caption: "Open the file, or enter the folder"
                key: details.openKey
                onClicked: details.openRequested()
            }
            Action {
                id: openWith
                objectName: "detailsOpenWith"
                label: "Open with"
                caption: "Choose the application to open it"
                usable: details.has && IndexModel.canOpenWith && details.onRow
                onClicked: details.openWithRequested(openWith)
            }
            Action {
                objectName: "detailsRename"
                label: "Rename"
                caption: "Rename the row or the marked files"
                key: details.renameKey
                usable: details.onRow
                onClicked: details.renameRequested()
            }
        }
    }
}
