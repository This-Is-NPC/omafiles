pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic as C
import QtQuick.Layouts
import omafiles

// The studio window.
//
// One keyboard hub. `root` holds the focus whenever the window is not in a text
// field or a sheet, and every key is decided in `handleKey` -- not spread across
// the views, which is how a key ends up working in one of them and not the
// other. The file views own no keys at all; they draw what the model says and
// report clicks. Shared move helpers carry j/k and page keys; each region keeps
// its own actions in handleListKey, handleTagKey, and handleGraphKey.
//
// Everything a mouse can reach here the keyboard can reach too: the controls
// are Chips and TabHandles, which take Tab, Space and Enter, and Escape from
// any of them returns to the list. `?` prints the whole map.
Window {
    id: win
    width: 1180
    height: 720
    minimumWidth: 880
    minimumHeight: 480
    visible: true
    title: "omafiles"
    color: Theme.background
    onClosing: Qt.quit()

    property var activeFilesRoot: null
    property var activeGraph: null
    property var activeDock: null
    property bool filtering: false
    property bool editingPath: false
    property string focusRegion: "list"
    property bool pendingG: false

    readonly property var activeTab: Workspace.currentTab
    readonly property var activeFolder: activeFilesRoot ? activeFilesRoot.folder : null
    readonly property var activeListing: activeFilesRoot ? activeFilesRoot.view : null
    readonly property var activeTagsList: activeDock ? activeDock.tagsList : null
    readonly property bool onFiles: activeTab !== null && activeTab !== undefined
                                    && activeTab.kind === "files" && activeFilesRoot !== null
    readonly property bool onGraph: activeTab !== null && activeTab !== undefined
                                    && activeTab.kind === "graph" && activeGraph !== null
    readonly property bool blocked: commandPalette.visible || helpSheet.visible
                                    || fileMenu.visible || tabMenu.visible
                                    || newTabMenu.visible || editingPath || filtering

    // ---------------------------------------------------------------- keymap
    //
    // The sheet under `?` renders this, and a test presses every key on it and
    // requires an effect -- so a key written here that does not work fails the
    // build. The other direction is on the author: a key the window answers and
    // nobody writes here is a key nobody will find.
    readonly property var keymap: [
        { title: "move", keys: [
            { key: "j k", label: "down, up" },
            { key: "↓ ↑", label: "down, up" },
            { key: "g g", label: "first row" },
            { key: "G", label: "last row" },
            { key: "Ctrl d", label: "half page down" },
            { key: "Ctrl u", label: "half page up" },
            { key: "PgDn PgUp", label: "page" },
            { key: "Home End", label: "first, last row" },
            { key: "← →", label: "prev, next (icons)" } ] },
        { title: "go", keys: [
            { key: "l", label: "open or enter" },
            { key: "Enter", label: "open or enter" },
            { key: "→", label: "open or enter (list, details)" },
            { key: "h", label: "parent folder" },
            { key: "Bksp", label: "parent folder" },
            { key: "←", label: "parent folder (list, details)" },
            { key: "~", label: "home folder" },
            { key: "Ctrl l", label: "type a path" },
            { key: "F5", label: "reload index" },
            { key: "Ctrl r", label: "reload index" } ] },
        { title: "mark", keys: [
            { key: "Space", label: "mark row" },
            { key: "a", label: "mark all" },
            { key: "A", label: "clear marks" },
            { key: "m", label: "menu for row or tab" },
            { key: "Menu", label: "menu for row or tab" },
            { key: "Shift F10", label: "menu for row or tab" } ] },
        { title: "view", keys: [
            { key: "1", label: "list" },
            { key: "2", label: "details" },
            { key: "3", label: "icons" },
            { key: ".", label: "hidden files" },
            { key: "Ctrl h", label: "hidden files" },
            { key: "s", label: "name, tags, type, size, modified, each the way it reads" },
            { key: "S", label: "flip the direction" } ] },
        { title: "find", keys: [
            { key: "/", label: "filter this folder" },
            { key: "Enter", label: "keep filter" },
            { key: "Esc", label: "clear filter" },
            { key: "t", label: "tag panel" },
            { key: "Enter", label: "toggle tag" },
            { key: "Space", label: "toggle tag" },
            { key: "h", label: "leave the tag panel" },
            { key: "← Esc", label: "leave the tag panel" } ] },
        { title: "tabs and panes", keys: [
            { key: "Ctrl t", label: "new tab" },
            { key: "Ctrl w", label: "close tab" },
            { key: "] [", label: "next, previous tab" },
            { key: "Ctrl Tab", label: "next tab" },
            { key: "Ctrl Shift Tab", label: "previous tab" },
            { key: "Alt 1…9", label: "go to tab" },
            { key: "Ctrl \\", label: "split right" },
            { key: "Ctrl Shift w", label: "close pane" },
            { key: "Alt h l", label: "focus pane" },
            { key: "Alt ← →", label: "focus pane" },
            { key: "Menu", label: "menu for tab" },
            { key: "Shift F10", label: "menu for tab" },
            { key: "Delete", label: "close tab" } ] },
        { title: "graph tab", keys: [
            { key: "j k", label: "next, previous node" },
            { key: "g G", label: "first, last node" },
            { key: "l", label: "open a file, list a tag" },
            { key: "m", label: "menu for a file node" },
            { key: "← →", label: "next, previous node" },
            { key: "Home End", label: "first, last node" },
            { key: "Enter", label: "open a file, list a tag" },
            { key: "Menu", label: "menu for a file node" },
            { key: "+ - =", label: "zoom" },
            { key: "0", label: "reset the view" } ] },
        { title: "window", keys: [
            { key: ":", label: "commands" },
            { key: "Ctrl p", label: "commands" },
            { key: "?", label: "this list" },
            { key: "Tab", label: "next control" },
            { key: "Esc", label: "back to the list" } ] }
    ]

    // Hints answer the question in front of you. Least important first: the row
    // is right-aligned and drops from its left end when the window narrows.
    readonly property var hintsForRegion: {
        if (editingPath)
            return [{ key: "Esc", label: "cancel" }, { key: "Enter", label: "go" }]
        if (filtering)
            return [{ key: "Esc", label: "clear" }, { key: "Enter", label: "keep" }]
        if (onGraph)
            return [{ key: "?", label: "keys" },
                    { key: ":", label: "cmd" },
                    { key: "0", label: "reset view" },
                    { key: "+ - =", label: "zoom" },
                    { key: "m", label: "menu" },
                    { key: "l", label: "open" },
                    { key: "j k", label: "node" }]
        if (focusRegion === "tags")
            return [{ key: "?", label: "keys" },
                    { key: "Esc", label: "back" },
                    { key: "j k", label: "move" },
                    { key: "Enter", label: "toggle" }]
        return [{ key: "?", label: "keys" },
                { key: ":", label: "cmd" },
                { key: "Space", label: "mark" },
                { key: "t", label: "tags" },
                { key: "/", label: "filter" },
                { key: "m", label: "menu" },
                { key: "h", label: "up" },
                { key: "l", label: "open" },
                { key: "j k", label: "move" }]
    }

    // ----------------------------------------------------------------- focus
    function takeFocus() {
        focusRegion = "list"
        root.forceActiveFocus()
    }

    function startFilter() {
        if (!onFiles)
            return
        filtering = true
        focusRegion = "filter"
        filterField.text = activeTab.filter
        filterField.focusEntry()
    }

    function finishFilter(clear) {
        if (clear && activeTab)
            activeTab.filter = ""
        filtering = false
        takeFocus()
    }

    function startPathEdit() {
        if (!onFiles)
            return
        editingPath = true
        focusRegion = "path"
        pathField.text = activeTab.cwd
        pathField.focusEntry()
    }

    function finishPathEdit(accept, text) {
        if (accept && activeFolder) {
            // Typing a path asks for that folder, which is a different question
            // from the tag that may be on screen.
            if (activeTab)
                activeTab.selectedTag = ""
            activeFolder.directory = text
        }
        editingPath = false
        takeFocus()
    }

    function tagRow(name) {
        for (let i = 0; i < IndexModel.tagRowCount(); ++i) {
            if (IndexModel.tagAt(i) === name)
                return i
        }
        return -1
    }

    function focusTags() {
        if (!activeTagsList || IndexModel.tagRowCount() === 0)
            return
        focusRegion = "tags"
        // On the tag the listing is showing, when there is one: the way out of
        // a tag listing is the key that opened it, and hunting for the row
        // again is not a way out.
        const chosen = activeTab ? tagRow(activeTab.selectedTag) : -1
        if (chosen >= 0)
            activeTagsList.currentIndex = chosen
        else if (activeTagsList.currentIndex < 0)
            activeTagsList.currentIndex = 0
    }

    function toggleTagAt(row) {
        if (!activeTab || row < 0)
            return
        const name = IndexModel.tagAt(row)
        if (name === "")
            return
        activeTab.selectedTag = activeTab.selectedTag === name ? "" : name
    }

    // ------------------------------------------------------------------ menu
    function openFileMenuAt(x, y) {
        fileMenu.returnFocus = takeFocus
        fileMenu.openAt(x, y)
    }

    function openFileMenuForPath(path, x, y) {
        fileMenu.targetPath = path
        openFileMenuAt(x, y)
    }

    // Opened by `m`, so it has to place itself: under the row if the row is on
    // screen, in the middle of the pane if there is no row to point at.
    function openFileMenuForListing() {
        const listing = activeListing
        const folder = activeFolder
        if (!listing || !folder || listing.currentIndex < 0) {
            fileMenu.targetPath = ""
            openFileMenuAt(root.width / 2, root.height / 3)
            return
        }
        fileMenu.targetPath = folder.pathAt(listing.currentIndex)
        // Only rows the view has actually built have a position to point at,
        // and a cursor that is off screen -- or past the end of a list that has
        // just shrunk -- has none.
        const item = listing.currentIndex < folder.count
                     ? listing.itemAtIndex(listing.currentIndex) : null
        if (!item) {
            openFileMenuAt(root.width / 2, root.height / 3)
            return
        }
        const pos = listing.mapToItem(root, 0, item.y - listing.contentY)
        openFileMenuAt(pos.x + 24, pos.y + item.height)
    }

    function openTabMenuAt(paneIndex, tabIndex, x, y) {
        const pane = Workspace.paneAtIndex(paneIndex)
        if (!pane)
            return
        Workspace.focusedPane = paneIndex
        pane.currentIndex = tabIndex
        tabMenu.targetPaneIndex = paneIndex
        tabMenu.targetTabIndex = tabIndex
        tabMenu.targetTabCount = pane.tabCount
        tabMenu.returnFocus = win.takeFocus
        tabMenu.openAt(x, y)
    }

    // ------------------------------------------------------------------ keys
    function moveCursor(delta) {
        if (activeFilesRoot)
            activeFilesRoot.setCursor(activeFilesRoot.cursor + delta)
    }

    function moveCursorTo(row) {
        if (activeFilesRoot)
            activeFilesRoot.setCursor(row)
    }

    // How many rows fit in the list the keyboard is walking. The tag panel
    // draws 25px rows with a pixel between them, the file views 30.
    function pageStep() {
        if (focusRegion === "tags")
            return activeTagsList ? Math.max(1, Math.floor(activeTagsList.height / 26)) : 10
        const listing = activeListing
        if (!listing)
            return 10
        return Math.max(1, Math.floor(listing.height / 30))
    }

    function moveTagCursor(delta) {
        const tags = activeTagsList
        if (!tags)
            return
        tags.currentIndex = Math.max(0, Math.min(tags.currentIndex + delta,
                                                 IndexModel.tagRowCount() - 1))
    }

    function moveByDelta(delta) {
        if (onGraph) {
            const graph = activeGraph
            if (graph)
                graph.stepSelection(delta)
            return
        }
        if (focusRegion === "tags")
            moveTagCursor(delta)
        else
            moveCursor(delta)
    }

    function moveToEnd(last) {
        if (onGraph) {
            const graph = activeGraph
            if (graph)
                graph.selectEnd(last)
            return
        }
        if (focusRegion === "tags") {
            const tags = activeTagsList
            if (tags)
                tags.currentIndex = last ? Math.max(0, IndexModel.tagRowCount() - 1) : 0
        } else {
            const folder = activeFolder
            if (folder)
                moveCursorTo(last ? folder.count - 1 : 0)
        }
    }

    function movePage(direction) {
        moveByDelta(direction * pageStep())
    }
    function handleMoveKey(event, text) {
        if (event.key === Qt.Key_Down || text === "j") {
            moveByDelta(1)
            return true
        }
        if (event.key === Qt.Key_Up || text === "k") {
            moveByDelta(-1)
            return true
        }
        if (event.key === Qt.Key_PageDown) {
            movePage(1)
            return true
        }
        if (event.key === Qt.Key_PageUp) {
            movePage(-1)
            return true
        }
        return false
    }

    function handleEndKey(event, text, viGg) {
        if ((viGg && text === "G") || event.key === Qt.Key_End) {
            moveToEnd(true)
            return true
        }
        if ((viGg && text === "g") || event.key === Qt.Key_Home) {
            moveToEnd(false)
            return true
        }
        return false
    }


    // Half a page, in whichever list the keyboard is in. The chord block runs
    // before any region is consulted, so a half page decided there would walk
    // the file list from inside the tag panel -- a cursor nobody can see move.
    function halfPage(direction) {
        if (onGraph)
            return false
        const step = direction * Math.max(1, Math.floor(pageStep() / 2))
        if (focusRegion === "tags")
            moveTagCursor(step)
        else
            moveCursor(step)
        return true
    }

    function handleModified(event, ctrl, alt, shift) {
        if (ctrl) {
            switch (event.key) {
            case Qt.Key_L: startPathEdit(); return true
            case Qt.Key_P: commandPalette.open(); return true
            case Qt.Key_T: Workspace.addTab("files"); return true
            case Qt.Key_R: IndexModel.reload(); return true
            case Qt.Key_W:
                if (shift)
                    Workspace.closePane(Workspace.focusedPane)
                else
                    Workspace.closeCurrentTab()
                return true
            case Qt.Key_Backslash: Workspace.splitRight("files"); return true
            case Qt.Key_Tab: Workspace.stepTab(1); return true
            case Qt.Key_Backtab: Workspace.stepTab(-1); return true
            case Qt.Key_D: return halfPage(1)
            case Qt.Key_U: return halfPage(-1)
            case Qt.Key_H:
                if (activeTab)
                    activeTab.toggleHidden()
                return true
            }
            return false
        }
        if (alt) {
            if (event.key === Qt.Key_H || event.key === Qt.Key_Left) {
                Workspace.stepPane(-1)
                return true
            }
            if (event.key === Qt.Key_L || event.key === Qt.Key_Right) {
                Workspace.stepPane(1)
                return true
            }
            if (event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
                Workspace.selectTab(event.key - Qt.Key_1)
                return true
            }
        }
        return false
    }

    function handleTagKey(event, text) {
        const tags = activeTagsList
        if (!tags)
            return false
        if (text === "h" || event.key === Qt.Key_Left) {
            takeFocus()
            return true
        }
        if (handleMoveKey(event, text))
            return true
        if (handleEndKey(event, text, true))
            return true
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Space) {
            toggleTagAt(tags.currentIndex)
            return true
        }
        return false
    }

    function handleListKey(event, text) {
        const listing = activeListing
        const folder = activeFolder
        if (!listing || !folder)
            return false

        if (handleMoveKey(event, text))
            return true
        if (handleEndKey(event, text, false))
            return true

        const icons = activeTab && activeTab.viewMode === "icons"

        switch (event.key) {
        case Qt.Key_Right:
            // In a grid, left and right are movement; in a column they are the
            // two directions a column does not have, so they walk the tree.
            if (icons)
                moveCursor(1)
            else
                folder.activateRow(listing.currentIndex)
            return true
        case Qt.Key_Left:
            if (icons)
                moveCursor(-1)
            else
                folder.goUp()
            return true
        case Qt.Key_Backspace: folder.goUp(); return true
        case Qt.Key_Return:
        case Qt.Key_Enter: folder.activateRow(listing.currentIndex); return true
        case Qt.Key_Space: folder.toggleMarkAt(listing.currentIndex); return true
        case Qt.Key_Menu: openFileMenuForListing(); return true
        case Qt.Key_F10:
            if (event.modifiers & Qt.ShiftModifier) {
                openFileMenuForListing()
                return true
            }
            return false
        }

        switch (text) {
        case "l": folder.activateRow(listing.currentIndex); return true
        case "h": folder.goUp(); return true
        case "~": folder.goHome(); return true
        case "t": focusTags(); return true
        case "m": openFileMenuForListing(); return true
        case "a": folder.markAll(); return true
        case "A": folder.clearMarks(); return true
        case "s": if (activeTab) activeTab.cycleSortRole(); return true
        case "S": if (activeTab) activeTab.toggleSortDirection(); return true
        case "1": if (activeTab) activeTab.viewMode = "list"; return true
        case "2": if (activeTab) activeTab.viewMode = "details"; return true
        case "3": if (activeTab) activeTab.viewMode = "icons"; return true
        case ".": if (activeTab) activeTab.toggleHidden(); return true
        case "G":
            moveCursorTo(folder.count - 1)
            pendingG = false
            gTimer.stop()
            return true
        case "g":
            // `gg`, the way vi spells "top". A single `g` is held for a moment
            // in case a second one is coming; the timer is what keeps a stray
            // `g` from arming it forever.
            if (event.isAutoRepeat)
                return true
            if (pendingG) {
                moveCursorTo(0)
                pendingG = false
                gTimer.stop()
            } else {
                pendingG = true
                gTimer.restart()
            }
            return true
        }
        return false
    }

    function handleGraphKey(event, text) {
        const graph = activeGraph
        if (!graph)
            return false
        if (handleMoveKey(event, text))
            return true
        if (handleEndKey(event, text, true))
            return true
        switch (event.key) {
        case Qt.Key_Right: moveByDelta(1); return true
        case Qt.Key_Left: moveByDelta(-1); return true
        case Qt.Key_Return:
        case Qt.Key_Enter: graph.activateSelection(); return true
        case Qt.Key_Menu: graph.menuForSelection(); return true
        }
        switch (text) {
        case "l": graph.activateSelection(); return true
        case "m": graph.menuForSelection(); return true
        case "0": graph.resetView(); return true
        case "+":
        case "=": graph.zoomBy(1.15); return true
        case "-": graph.zoomBy(1 / 1.15); return true
        }
        return false
    }

    function handleKey(event) {
        if (blocked)
            return

        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const alt = (event.modifiers & Qt.AltModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        const text = event.text

        // Escape comes first and from anywhere, including a control reached by
        // Tab: it is the way back to the list, and it has to work before any
        // region has had a chance to have an opinion.
        if (event.key === Qt.Key_Escape) {
            takeFocus()
            event.accepted = true
            return
        }

        if (ctrl || alt) {
            if (handleModified(event, ctrl, alt, shift))
                event.accepted = true
            return
        }

        if (event.key === Qt.Key_F5) {
            IndexModel.reload()
            event.accepted = true
            return
        }
        if (text === "?") {
            helpSheet.returnFocus = takeFocus
            helpSheet.open()
            event.accepted = true
            return
        }
        if (text === ":") {
            commandPalette.open()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Slash) {
            startFilter()
            event.accepted = true
            return
        }
        if (text === "]") {
            Workspace.stepTab(1)
            event.accepted = true
            return
        }
        if (text === "[") {
            Workspace.stepTab(-1)
            event.accepted = true
            return
        }

        if (onGraph) {
            if (handleGraphKey(event, text))
                event.accepted = true
            return
        }

        if (!onFiles)
            return

        const handled = focusRegion === "tags" ? handleTagKey(event, text)
                                               : handleListKey(event, text)
        if (handled)
            event.accepted = true
    }

    Timer {
        id: gTimer
        interval: 400
        onTriggered: win.pendingG = false
    }

    Item {
        id: root
        anchors.fill: parent
        focus: true

        Accessible.role: Accessible.Pane
        Accessible.name: win.title

        // In the Tab ring as well as the default holder of the focus, so Tab
        // walks out through the controls and comes back here rather than
        // stranding the keyboard on the last chip.
        activeFocusOnTab: true

        Keys.onPressed: function (event) { win.handleKey(event) }

        Column {
            anchors.fill: parent
            spacing: 0

            // -------------------------------------------------------- header
            Rectangle {
                id: header
                width: parent.width
                height: 38
                color: Theme.panel

                Row {
                    id: navRow
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    ToolButton {
                        anchors.verticalCenter: parent.verticalCenter
                        iconKind: "up"
                        caption: "Parent folder"
                        key: "h"
                        usable: win.activeFolder !== null && !win.activeFolder.atRoot
                        onClicked: if (win.activeFolder) win.activeFolder.goUp()
                    }
                    ToolButton {
                        anchors.verticalCenter: parent.verticalCenter
                        iconKind: "home"
                        caption: "Home folder"
                        key: "~"
                        usable: win.onFiles
                        onClicked: if (win.activeFolder) win.activeFolder.goHome()
                    }
                    ToolButton {
                        anchors.verticalCenter: parent.verticalCenter
                        iconKind: "reload"
                        caption: "Reload index"
                        key: "F5"
                        onClicked: IndexModel.reload()
                    }
                }

                // The path, and the box you type one into, in the same place.
                // Clicking the path opens the box; Ctrl+L is the same door.
                Item {
                    id: pathArea
                    anchors.left: navRow.right
                    anchors.leftMargin: 12
                    anchors.right: counts.left
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    height: 26

                    Row {
                        id: crumbs
                        objectName: "pathCrumbs"
                        visible: !win.editingPath
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 0

                        // A tag listing spans the index, so there is no path to
                        // print: it is spelled `#tag`, the way the graph and
                        // the tab strip spell it, rather than as a folder the
                        // window is not standing in.
                        readonly property string full: !win.activeTab
                                                       || win.activeTab.kind !== "files" ? ""
                            : win.activeTab.selectedTag !== ""
                              ? "#" + win.activeTab.selectedTag : win.activeTab.cwd
                        readonly property int cut: full.lastIndexOf("/")
                        readonly property string stem: cut > 0 ? full.substring(0, cut + 1)
                                                               : (cut === 0 ? "/" : "")
                        readonly property string leaf: cut >= 0 ? full.substring(cut + 1) : full

                        Text {
                            // Elided from the left: the end of a path is the
                            // part that says where you are. Sized to its own
                            // text while that fits, so the leaf sits against
                            // the stem instead of being pushed to the far edge.
                            width: Math.max(0, Math.min(implicitWidth,
                                                        crumbs.width - leafText.implicitWidth))
                            anchors.verticalCenter: parent.verticalCenter
                            text: crumbs.stem
                            color: Theme.dim
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            elide: Text.ElideLeft
                            renderType: Text.NativeRendering
                        }
                        Text {
                            id: leafText
                            anchors.verticalCenter: parent.verticalCenter
                            text: crumbs.leaf
                            color: Theme.foreground
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            renderType: Text.NativeRendering
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        visible: !win.editingPath
                        cursorShape: Qt.IBeamCursor
                        onClicked: win.startPathEdit()
                    }

                    Entry {
                        id: pathField
                        anchors.fill: parent
                        visible: win.editingPath
                        placeholder: "path"
                        onAccepted: function (value) { win.finishPathEdit(true, value) }
                        onCancelled: win.finishPathEdit(false, "")
                    }
                }

                Label {
                    id: counts
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    quiet: true
                    text: IndexModel.tagCount + " tags · " + IndexModel.fileCount + " tagged"
                }
            }

            Rule {}

            // ------------------------------------------------------- content
            C.SplitView {
                id: split
                width: parent.width
                // Less the two Rule hairlines between the three bars. Named,
                // because a third divider added above would otherwise push the
                // pane off the bottom of the window with nothing to say why.
                readonly property int dividers: 2
                height: parent.height - header.height - status.height
                        - filterBar.height - dividers
                orientation: Qt.Horizontal

                handle: Rectangle {
                    implicitWidth: 3
                    color: C.SplitHandle.pressed || C.SplitHandle.hovered
                           ? Theme.fill(Theme.accent, 0.5) : Theme.line
                }

                Repeater {
                    model: Workspace.panes
                    delegate: Pane {
                        required property var modelData
                        required property int index
                        pane: modelData
                        paneIndex: index
                        C.SplitView.preferredWidth: win.width / Math.max(1, Workspace.paneCount)
                        C.SplitView.minimumWidth: 280
                    }
                }
            }

            // -------------------------------------------------------- filter
            Item {
                id: filterBar
                width: parent.width
                height: win.filtering ? 34 : 0
                visible: height > 0
                clip: true

                Rectangle {
                    anchors.fill: parent
                    color: Theme.panel
                }

                Entry {
                    id: filterField
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    lead: "/"
                    placeholder: "filter this folder"
                    onTextChanged: if (win.filtering && win.activeTab) win.activeTab.filter = text
                    onAccepted: win.finishFilter(false)
                    onCancelled: win.finishFilter(true)
                }
            }

            Rule {}

            // -------------------------------------------------------- status
            StatusBar {
                id: status
                objectName: "statusBar"
                width: parent.width
                // A graph tab counts nodes, because that is what it has: the
                // folder counts belong to a FolderModel it does not use, and
                // read out here they would be a confident `0/0`.
                cursor: win.onGraph ? win.activeGraph.selectedRow
                                    : (win.activeFilesRoot ? win.activeFilesRoot.cursor : -1)
                total: win.onGraph ? win.activeGraph.nodeCount
                                   : (win.activeFolder ? win.activeFolder.count : 0)
                note: win.onGraph ? win.activeGraph.summary : ""
                marked: win.activeFolder ? win.activeFolder.markedCount : 0
                tagged: win.activeFolder ? win.activeFolder.taggedCount : 0
                tag: win.activeTab ? win.activeTab.selectedTag : ""
                filter: win.activeTab ? win.activeTab.filter : ""
                // The folder first: it is about the pane in front of you and
                // it goes away when you walk out of it, where an index error
                // stays until the index is fixed and will be there when you do.
                message: win.activeFolder && win.activeFolder.error !== ""
                         ? win.activeFolder.error : IndexModel.error
                hints: win.hintsForRegion
            }
        }

        // ------------------------------------------------------------ sheets
        CommandPalette {
            id: commandPalette
            anchors.fill: parent
            returnFocus: win.takeFocus
            commands: [
                { id: "add-files", label: "New files tab", key: "Ctrl+T" },
                { id: "add-graph", label: "New graph tab", key: "" },
                { id: "close-tab", label: "Close tab", key: "Ctrl+W" },
                { id: "split", label: "Split right", key: "Ctrl+\\" },
                { id: "close-pane", label: "Close pane", key: "Ctrl+Shift+W" },
                { id: "reload", label: "Reload index", key: "F5" },
                { id: "home", label: "Go to home folder", key: "~" },
                { id: "up", label: "Go to parent folder", key: "h" },
                { id: "path", label: "Type a path", key: "Ctrl+L" },
                { id: "filter", label: "Filter this folder", key: "/" },
                { id: "mark-all", label: "Mark all", key: "a" },
                { id: "clear-marks", label: "Clear marks", key: "A" },
                { id: "clear-tag", label: "Leave the tag listing", key: "" },
                { id: "view-list", label: "View as list", key: "1" },
                { id: "view-details", label: "View as details", key: "2" },
                { id: "view-icons", label: "View as icons", key: "3" },
                { id: "hidden", label: "Show hidden files", key: "." },
                { id: "sort-name", label: "Sort by name", key: "s" },
                { id: "sort-tags", label: "Sort by tag, untagged last", key: "s" },
                { id: "sort-type", label: "Sort by type", key: "s" },
                { id: "sort-size", label: "Sort by size", key: "s" },
                { id: "sort-mtime", label: "Sort by modified", key: "s" },
                { id: "sort-flip", label: "Flip sort direction", key: "S" },
                { id: "keys", label: "Show keys", key: "?" }
            ]
            onChosen: function (id) {
                switch (id) {
                case "add-files": Workspace.addTab("files"); break
                case "add-graph": Workspace.addTab("graph"); break
                case "close-tab": Workspace.closeCurrentTab(); break
                case "split": Workspace.splitRight("files"); break
                case "close-pane": Workspace.closePane(Workspace.focusedPane); break
                case "reload": IndexModel.reload(); break
                case "home": if (win.activeFolder) win.activeFolder.goHome(); break
                case "up": if (win.activeFolder) win.activeFolder.goUp(); break
                case "path": Qt.callLater(win.startPathEdit); break
                case "filter": Qt.callLater(win.startFilter); break
                case "mark-all": if (win.activeFolder) win.activeFolder.markAll(); break
                case "clear-marks": if (win.activeFolder) win.activeFolder.clearMarks(); break
                case "clear-tag": if (win.activeTab) win.activeTab.selectedTag = ""; break
                case "view-list": if (win.activeTab) win.activeTab.viewMode = "list"; break
                case "view-details": if (win.activeTab) win.activeTab.viewMode = "details"; break
                case "view-icons": if (win.activeTab) win.activeTab.viewMode = "icons"; break
                case "hidden": if (win.activeTab) win.activeTab.toggleHidden(); break
                case "sort-name": if (win.activeTab) win.activeTab.pickSortRole("name"); break
                case "sort-type": if (win.activeTab) win.activeTab.pickSortRole("type"); break
                case "sort-size": if (win.activeTab) win.activeTab.pickSortRole("size"); break
                case "sort-mtime": if (win.activeTab) win.activeTab.pickSortRole("mtime"); break
                case "sort-tags": if (win.activeTab) win.activeTab.pickSortRole("tags"); break
                case "sort-flip": if (win.activeTab) win.activeTab.toggleSortDirection(); break
                case "keys":
                    Qt.callLater(function () {
                        helpSheet.returnFocus = win.takeFocus
                        helpSheet.open()
                    })
                    break
                }
                commandPalette.close()
            }
        }

        HelpSheet {
            id: helpSheet
            anchors.fill: parent
            groups: win.keymap
        }

        ContextMenu {
            id: fileMenu
            anchors.fill: parent
            property string targetPath: ""
            actions: [
                { id: "open", label: "Open", key: "l" },
                { id: "share", label: "Send with LocalSend", key: "", when: IndexModel.canShare }
            ]
            onChosen: function (id) {
                const path = fileMenu.targetPath
                if (!path)
                    return
                if (id === "open") {
                    const folder = win.activeFolder
                    const listing = win.activeListing
                    if (folder && listing && listing.currentIndex >= 0
                            && folder.pathAt(listing.currentIndex) === path)
                        folder.activateRow(listing.currentIndex)
                    else
                        IndexModel.openPath(path)
                } else if (id === "share") {
                    IndexModel.sharePath(path)
                }
            }
        }

        ContextMenu {
            id: tabMenu
            anchors.fill: parent
            property int targetPaneIndex: 0
            property int targetTabIndex: 0
            property int targetTabCount: 1
            actions: [
                { id: "close", label: "Close tab", key: "Ctrl+W" },
                { id: "close-others", label: "Close others", when: targetTabCount > 1 }
            ]
            onChosen: function (id) {
                const pi = tabMenu.targetPaneIndex
                const ti = tabMenu.targetTabIndex
                if (id === "close") {
                    Workspace.closeTab(pi, ti)
                } else if (id === "close-others") {
                    for (let i = tabMenu.targetTabCount - 1; i > ti; i--)
                        Workspace.closeTab(pi, i)
                    for (let i = ti - 1; i >= 0; i--)
                        Workspace.closeTab(pi, i)
                }
            }
        }

        ContextMenu {
            id: newTabMenu
            anchors.fill: parent
            property int targetPaneIndex: 0
            actions: [
                { id: "files", label: "Files tab" },
                { id: "graph", label: "Graph tab" }
            ]
            onChosen: function (id) {
                Workspace.addTabIn(newTabMenu.targetPaneIndex, id)
            }
        }
    }

    // ------------------------------------------------------------------ pane
    component Pane: Rectangle {
        id: paneRoot
        // Named so a test can click inside the pane it means rather than at a
        // fraction of the window and hope the split had finished laying out.
        objectName: "pane"
        property var pane
        property int paneIndex
        color: Theme.background

        // Any press anywhere in this pane makes it the focused one.
        //
        // In front of the pane's contents and refusing the event, not behind
        // them. Behind, it sees nothing: the list is a Flickable, it accepts the
        // press for itself, and delivery stops there, so a click on the empty
        // space below the last row would leave the keyboard on the other pane.
        // Declining the press here lets it go on to whatever is under the
        // pointer, so this reads every click without taking one.
        MouseArea {
            anchors.fill: parent
            z: 100
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            onPressed: function (mouse) {
                Workspace.focusedPane = paneRoot.paneIndex
                mouse.accepted = false
            }
        }

        // The dock is a column of the pane, not a column of the view inside it,
        // so it runs the whole height and the tab strip stops where it starts.
        RowLayout {
            anchors.fill: parent
            spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // The tab strip, and on its right the controls that say how the
            // list is drawn. They sit here rather than in a bar of their own,
            // which spends a whole strip of window on six words that belong
            // beside the tabs.
            Rectangle {
                id: strip
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                color: Workspace.focusedPane === paneRoot.paneIndex ? Theme.panel : Theme.sunken
                clip: true

                Row {
                    id: tabRow
                    anchors.left: parent.left
                    anchors.leftMargin: 4
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    spacing: 0

                    Repeater {
                        model: paneRoot.pane.tabs
                        delegate: TabHandle {
                            objectName: "tabHandle"
                            required property var modelData
                            required property int index
                            title: modelData.title
                            // A tab showing a tag is not a folder, and a folder
                            // icon beside `#work` says it is.
                            kind: modelData.kind === "files" && modelData.selectedTag !== ""
                                  ? "tag" : modelData.kind
                            // Optional, because closing a pane leaves the
                            // delegate alive one evaluation longer than the
                            // pane: under `ComponentBehavior: Bound` the
                            // enclosing scope is kept, so this runs once more
                            // with `pane` already null.
                            current: paneRoot.pane?.currentIndex === index
                            onActivated: {
                                Workspace.focusedPane = paneRoot.paneIndex
                                paneRoot.pane.currentIndex = index
                            }
                            onClosed: {
                                Workspace.focusedPane = paneRoot.paneIndex
                                Workspace.closeTab(paneRoot.paneIndex, index)
                            }
                            Keys.onPressed: function (event) {
                                if (event.key === Qt.Key_Menu
                                        || (event.key === Qt.Key_F10
                                            && (event.modifiers & Qt.ShiftModifier))) {
                                    const pos = mapToItem(root, width / 2, height)
                                    win.openTabMenuAt(paneRoot.paneIndex, index, pos.x, pos.y)
                                    event.accepted = true
                                }
                            }
                            onMenuRequested: function (x, y) {
                                const pos = mapToItem(root, x, y)
                                win.openTabMenuAt(paneRoot.paneIndex, index, pos.x, pos.y)
                            }
                        }
                    }

                    Item { width: 6; height: 1 }

                    Chip {
                        anchors.verticalCenter: parent.verticalCenter
                        label: "+"
                        caption: "New tab"
                        key: "Ctrl+T"
                        onClicked: {
                            Workspace.focusedPane = paneRoot.paneIndex
                            newTabMenu.targetPaneIndex = paneRoot.paneIndex
                            const pos = mapToItem(root, 0, height + 4)
                            newTabMenu.returnFocus = win.takeFocus
                            newTabMenu.openAt(pos.x, pos.y)
                        }
                    }
                }

                Row {
                    id: viewRow

                    property var tab: paneRoot.pane.current

                    function sortMark(role) {
                        if (!viewRow.tab || viewRow.tab.sortRole !== role)
                            return ""
                        return viewRow.tab.sortAscending ? " ↑" : " ↓"
                    }

                    // Dropped whole when the pane is too narrow for it rather
                    // than sliding under the tabs. Every one of these has a key.
                    visible: viewRow.tab && viewRow.tab.kind === "files"
                             && strip.width - tabRow.width > implicitWidth + 28
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 1

                    Chip {
                        quiet: true
                        label: "list"
                        caption: "List view"
                        key: "1"
                        on: viewRow.tab && viewRow.tab.viewMode === "list"
                        onClicked: if (viewRow.tab) viewRow.tab.viewMode = "list"
                    }
                    Chip {
                        quiet: true
                        label: "details"
                        caption: "Details view"
                        key: "2"
                        on: viewRow.tab && viewRow.tab.viewMode === "details"
                        onClicked: if (viewRow.tab) viewRow.tab.viewMode = "details"
                    }
                    Chip {
                        quiet: true
                        label: "icons"
                        caption: "Icons view"
                        key: "3"
                        on: viewRow.tab && viewRow.tab.viewMode === "icons"
                        onClicked: if (viewRow.tab) viewRow.tab.viewMode = "icons"
                    }

                    Item {
                        width: 15
                        height: 14
                        anchors.verticalCenter: parent.verticalCenter
                        Rule {
                            vertical: true
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
                    }

                    Chip {
                        quiet: true
                        label: "name" + viewRow.sortMark("name")
                        caption: "Sort by name"
                        key: "s"
                        on: viewRow.tab && viewRow.tab.sortRole === "name"
                        onClicked: viewRow.tab.pickSortRole("name")
                    }
                    Chip {
                        quiet: true
                        label: "tags" + viewRow.sortMark("tags")
                        caption: "Sort by tag, untagged last"
                        key: "s"
                        on: viewRow.tab && viewRow.tab.sortRole === "tags"
                        onClicked: viewRow.tab.pickSortRole("tags")
                    }
                    Chip {
                        quiet: true
                        label: "type" + viewRow.sortMark("type")
                        caption: "Sort by type"
                        key: "s"
                        on: viewRow.tab && viewRow.tab.sortRole === "type"
                        onClicked: viewRow.tab.pickSortRole("type")
                    }
                    Chip {
                        quiet: true
                        label: "size" + viewRow.sortMark("size")
                        caption: "Sort by size"
                        key: "s"
                        on: viewRow.tab && viewRow.tab.sortRole === "size"
                        onClicked: viewRow.tab.pickSortRole("size")
                    }
                    Chip {
                        quiet: true
                        label: "modified" + viewRow.sortMark("mtime")
                        caption: "Sort by modified"
                        key: "s"
                        on: viewRow.tab && viewRow.tab.sortRole === "mtime"
                        onClicked: viewRow.tab.pickSortRole("mtime")
                    }
                }
            }

            Rule { Layout.fillWidth: true }

            Loader {
                id: content
                Layout.fillWidth: true
                Layout.fillHeight: true
                sourceComponent: paneRoot.pane.current && paneRoot.pane.current.kind === "graph"
                                  ? graphView : filesView
                property var tab: paneRoot.pane.current
                property int paneIndex: paneRoot.paneIndex
            }
        }

        Rule {
            vertical: true
            visible: dock.visible
            Layout.fillHeight: true
            Layout.preferredWidth: 1
        }

        Dock {
            id: dock
            Layout.fillHeight: true
            Layout.preferredWidth: 252
            readonly property bool holdsTags: paneRoot.pane.current
                                              && paneRoot.pane.current.kind === "files"
            // Dropped whole in a pane too narrow to hold it and a list at the
            // same time -- splitting the window is asking for two lists, not
            // for two copies of the same tag vocabulary where the names should
            // be -- and back for as long as `t` is walking it, because a key
            // the sheet names without a condition has to work at every size.
            visible: dock.holdsTags && (paneRoot.width >= 640 || dock.walking)

            tab: paneRoot.pane.current
            // A Loader's payload has no static type -- it is whichever component
            // is loaded -- so nothing can tell the linter that this one has an
            // `entry`. The check is disabled for the line rather than for the
            // file, and the runtime guard above is what actually keeps it safe.
            // qmllint disable missing-property
            entry: content.item && content.item.entry !== undefined ? content.item.entry : ({})
            // qmllint enable missing-property
            walking: win.focusRegion === "tags" && win.activeDock === dock

            onTagActivated: function (name) {
                win.takeFocus()
                if (dock.tab)
                    dock.tab.selectedTag = dock.tab.selectedTag === name ? "" : name
            }

            // On the tab and not on the width: the panel the keyboard talks to
            // is the one this pane's tab has, whether or not there is room to
            // draw it yet.
            function syncActive() {
                if (dock.holdsTags && Workspace.currentTab === paneRoot.pane.current)
                    win.activeDock = dock
                else if (win.activeDock === dock)
                    win.activeDock = null
            }

            Component.onCompleted: syncActive()
            onHoldsTagsChanged: syncActive()
            Component.onDestruction: if (win.activeDock === dock) win.activeDock = null

            Connections {
                target: Workspace
                function onCurrentTabChanged() { dock.syncActive() }
                function onFocusedPaneChanged() { dock.syncActive() }
            }
        }
        }
    }

    // ------------------------------------------------------------ files view
    Component {
        id: filesView
        Item {
            id: filesRoot
            anchors.fill: parent
            // `parent` is the Loader that holds this component, and a Loader is
            // typed as a plain item -- the two properties it carries for its
            // payload cannot be seen statically from in here. Disabled for these
            // two lines only.
            // qmllint disable missing-property
            property var tab: parent.tab
            // Which pane this is, so a click on a row can say so itself. The
            // pane's own catch-all covers it too, but the row is the one that
            // knows for certain.
            property int paneIndex: parent.paneIndex
            // qmllint enable missing-property
            property alias folder: folder
            readonly property string viewMode: filesRoot.tab ? filesRoot.tab.viewMode : "list"
            readonly property var view: viewMode === "icons" ? grid : listing

            // The details table's geometry, in one place. A heading and the
            // cells under it that each work out their own width are a heading
            // and a column that do not line up.
            readonly property real nameIndent: 54
            readonly property real typeWidth: 62
            readonly property real sizeWidth: 74
            readonly property real timeWidth: 128
            readonly property real tagsWidth: Math.max(90, Math.min(260, listArea.width * 0.26))

            readonly property int cursor: view ? view.currentIndex : -1

            // What the details panel reads. Recomputed rather than bound,
            // because the row it describes can change without its index moving:
            // a rebuild keeps index 4 and puts a different file there.
            property var entry: ({})

            function syncEntry() {
                filesRoot.entry = folder.entryAt(filesRoot.cursor)
            }

            // The one place the cursor moves. Clamped here so no caller has to
            // know the row count, and remembered here so a rebuild -- a filter
            // keystroke, a reload, a change of directory -- can put it back.
            function setCursor(row) {
                if (folder.count <= 0)
                    return
                const clamped = Math.max(0, Math.min(row, folder.count - 1))
                listing.currentIndex = clamped
                grid.currentIndex = clamped
                folder.rememberCursor(clamped)
                syncEntry()
            }

            // After a rebuild the views reset their own cursor to the top before
            // this gets a say, so the restore has to run once that has happened.
            function restoreCursor() {
                const row = folder.cursorRow()
                if (row < 0) {
                    filesRoot.entry = ({})
                    return
                }
                listing.currentIndex = row
                grid.currentIndex = row
                syncEntry()
            }

            function syncActive() {
                if (filesRoot.tab && Workspace.currentTab === filesRoot.tab)
                    win.activeFilesRoot = filesRoot
                else if (win.activeFilesRoot === filesRoot)
                    win.activeFilesRoot = null
            }

            Component.onCompleted: { syncActive(); syncEntry() }
            Component.onDestruction: {
                if (win.activeFilesRoot === filesRoot)
                    win.activeFilesRoot = null
            }
            onTabChanged: syncActive()

            Connections {
                target: Workspace
                function onCurrentTabChanged() { filesRoot.syncActive() }
                function onFocusedPaneChanged() { filesRoot.syncActive() }
            }

            // One model per pane, drawing whichever of the pane's tabs is
            // current. The tab it is pointed at owns the marks and the cursor,
            // so switching tabs here changes the folder and everything said
            // about it together.
            FolderModel {
                id: folder
                source: IndexModel
                tab: filesRoot.tab
                directory: filesRoot.tab ? filesRoot.tab.cwd : ""
                filter: filesRoot.tab ? filesRoot.tab.filter : ""
                selectedTag: filesRoot.tab ? filesRoot.tab.selectedTag : ""
                showHidden: filesRoot.tab ? filesRoot.tab.showHidden : false
                sortRole: filesRoot.tab ? filesRoot.tab.sortRole : "name"
                sortAscending: filesRoot.tab ? filesRoot.tab.sortAscending : true
                onDirectoryChanged: if (filesRoot.tab) filesRoot.tab.cwd = directory
                // Walking into a folder and going home both leave a tag
                // listing, so the tag travels back to the tab the way the
                // directory does.
                onSelectedTagChanged: if (filesRoot.tab) filesRoot.tab.selectedTag = selectedTag
                onChanged: Qt.callLater(filesRoot.restoreCursor)
                // The panel reads one row, and marking does not move the
                // cursor: without this the `marked` line only catches up when
                // the cursor next moves off the row and back.
                onMarkedChanged: filesRoot.syncEntry()
            }

            Item {
                id: listArea
                anchors.fill: parent

                    ListView {
                        id: listing
                        anchors.fill: parent
                        visible: filesRoot.viewMode !== "icons"
                        clip: true
                        model: folder
                        boundsBehavior: Flickable.StopAtBounds
                        currentIndex: 0
                        keyNavigationEnabled: false
                        Accessible.role: filesRoot.viewMode === "details"
                                         ? Accessible.Table : Accessible.List
                        Accessible.name: "Files"
                        onCurrentIndexChanged: filesRoot.syncEntry()

                        header: Rectangle {
                            id: head
                            width: listing.width
                            height: filesRoot.viewMode === "details" ? 24 : 0
                            visible: height > 0
                            color: Theme.sunken

                            ColumnHead {
                                anchors.left: parent.left
                                anchors.leftMargin: filesRoot.nameIndent
                                anchors.right: headTags.left
                                anchors.rightMargin: 10
                                height: parent.height
                                label: "name"
                                role: "name"
                                sortRole: filesRoot.tab ? filesRoot.tab.sortRole : ""
                                ascending: filesRoot.tab && filesRoot.tab.sortAscending
                                onPicked: filesRoot.tab.pickSortRole("name")
                            }
                            ColumnHead {
                                id: headTags
                                anchors.right: headType.left
                                anchors.rightMargin: 10
                                height: parent.height
                                width: filesRoot.tagsWidth
                                label: "tags"
                                role: "tags"
                                sortRole: filesRoot.tab ? filesRoot.tab.sortRole : ""
                                ascending: filesRoot.tab && filesRoot.tab.sortAscending
                                onPicked: filesRoot.tab.pickSortRole("tags")
                            }
                            ColumnHead {
                                id: headType
                                anchors.right: headSize.left
                                anchors.rightMargin: 10
                                height: parent.height
                                width: filesRoot.typeWidth
                                label: "type"
                                role: "type"
                                sortRole: filesRoot.tab ? filesRoot.tab.sortRole : ""
                                ascending: filesRoot.tab && filesRoot.tab.sortAscending
                                onPicked: filesRoot.tab.pickSortRole("type")
                            }
                            ColumnHead {
                                id: headSize
                                anchors.right: headTime.left
                                anchors.rightMargin: 10
                                height: parent.height
                                width: filesRoot.sizeWidth
                                label: "size"
                                role: "size"
                                sortRole: filesRoot.tab ? filesRoot.tab.sortRole : ""
                                ascending: filesRoot.tab && filesRoot.tab.sortAscending
                                onPicked: filesRoot.tab.pickSortRole("size")
                            }
                            ColumnHead {
                                id: headTime
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                height: parent.height
                                width: filesRoot.timeWidth
                                label: "modified"
                                role: "mtime"
                                sortRole: filesRoot.tab ? filesRoot.tab.sortRole : ""
                                ascending: filesRoot.tab && filesRoot.tab.sortAscending
                                onPicked: filesRoot.tab.pickSortRole("mtime")
                            }

                            Rule { anchors.bottom: parent.bottom }
                        }

                        delegate: Rectangle {
                            id: row
                            required property string path
                            required property string name
                            required property string kind
                            required property string type
                            required property string tags
                            required property bool marked
                            required property string sizeText
                            required property string mtimeText
                            required property int index
                            readonly property bool details: filesRoot.viewMode === "details"
                            readonly property var tagList: row.tags.length === 0
                                                           ? [] : row.tags.split(" ")
                            readonly property bool onCursor: listing.currentIndex === index

                            width: ListView.view.width
                            height: 30
                            Accessible.role: row.details ? Accessible.Cell : Accessible.ListItem
                            Accessible.name: row.name
                            color: onCursor ? Theme.fill(Theme.accent, 0.16)
                                 : rowHover.hovered ? Theme.fill(Theme.foreground, 0.05)
                                                    : "transparent"

                            // The cursor is a bar at the left edge rather than a
                            // border around the row: at this row height an
                            // outline is most of what you see, and the bar still
                            // reads when the row is half scrolled out of view.
                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: 2
                                color: row.onCursor ? Theme.accent : "transparent"
                            }

                            Text {
                                id: markDot
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                width: 10
                                horizontalAlignment: Text.AlignHCenter
                                text: row.marked ? "\u2022" : ""
                                color: Theme.accent
                                font.family: Theme.fontFamily
                                font.pixelSize: 14
                                renderType: Text.NativeRendering
                            }

                            Icon {
                                id: rowIcon
                                anchors.left: markDot.right
                                anchors.leftMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                kind: row.kind === "file" ? "file"
                                    : (row.kind === "parent" ? "parent" : "folder")
                                ink: row.kind === "file" ? Theme.dim : Theme.accent
                            }

                            // Anchored between the icon and whatever is on the
                            // right, rather than laid out in a Row: in a Row the
                            // name takes a width guessed from the row width and
                            // the tags run off the end of it.
                            Label {
                                anchors.left: rowIcon.right
                                anchors.leftMargin: 8
                                anchors.right: row.details ? detailTags.left : tagArea.left
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: row.name
                                opacity: row.onCursor ? 1 : 0.88
                            }

                            // A column, so it takes the same width on every row
                            // and packs from the left where its heading starts.
                            Label {
                                id: typeText
                                visible: row.details
                                anchors.right: sizeText.left
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                width: filesRoot.typeWidth
                                quiet: true
                                font.pixelSize: 11
                                text: row.type
                            }

                            TagStrip {
                                id: detailTags
                                visible: row.details
                                anchors.right: typeText.left
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                width: filesRoot.tagsWidth
                                limit: filesRoot.tagsWidth
                                alignRight: false
                                tags: row.tagList
                            }

                            Label {
                                id: sizeText
                                visible: row.details
                                anchors.right: mtimeText.left
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                width: filesRoot.sizeWidth
                                quiet: true
                                font.pixelSize: 11
                                text: row.sizeText
                            }

                            Label {
                                id: mtimeText
                                visible: row.details
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                width: filesRoot.timeWidth
                                quiet: true
                                font.pixelSize: 11
                                text: row.mtimeText
                            }

                            // Floats at the end of the row rather than lining up
                            // in a column: the list view has no columns to line
                            // up with.
                            TagStrip {
                                id: tagArea
                                visible: !row.details
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                limit: row.width * 0.4
                                tags: row.tagList
                            }

                            HoverHandler { id: rowHover }

                            TapHandler {
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onTapped: function (point, button) {
                                    Workspace.focusedPane = filesRoot.paneIndex
                                    filesRoot.setCursor(row.index)
                                    win.takeFocus()
                                    if (button === Qt.RightButton) {
                                        const pos = row.mapToItem(root, point.position.x,
                                                                  point.position.y)
                                        win.openFileMenuForPath(row.path, pos.x, pos.y)
                                    }
                                }
                                onDoubleTapped: {
                                    Workspace.focusedPane = filesRoot.paneIndex
                                    filesRoot.setCursor(row.index)
                                    win.takeFocus()
                                    folder.activateRow(row.index)
                                }
                            }
                        }
                    }

                    GridView {
                        id: grid
                        anchors.fill: parent
                        visible: filesRoot.viewMode === "icons"
                        clip: true
                        model: folder
                        cellWidth: 112
                        cellHeight: 92
                        boundsBehavior: Flickable.StopAtBounds
                        currentIndex: 0
                        keyNavigationEnabled: false
                        Accessible.role: Accessible.List
                        Accessible.name: "Files"
                        onCurrentIndexChanged: filesRoot.syncEntry()

                        delegate: Item {
                            id: cell
                            required property string path
                            required property string name
                            required property string kind
                            required property bool marked
                            required property int index
                            readonly property bool onCursor: grid.currentIndex === index

                            Accessible.role: Accessible.ListItem
                            Accessible.name: cell.name
                            width: grid.cellWidth
                            height: grid.cellHeight

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 4
                                radius: Theme.rounding
                                color: cell.onCursor ? Theme.fill(Theme.accent, 0.16)
                                     : cellHover.hovered ? Theme.fill(Theme.foreground, 0.05)
                                                         : "transparent"
                                border.width: 1
                                border.color: cell.onCursor ? Theme.accent : "transparent"

                                Column {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 6

                                    Icon {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        width: 30
                                        height: 30
                                        kind: cell.kind === "file" ? "file"
                                            : (cell.kind === "parent" ? "parent" : "folder")
                                        ink: cell.kind === "file" ? Theme.dim : Theme.accent
                                    }
                                    Label {
                                        width: parent.width
                                        text: (cell.marked ? "\u2022 " : "") + cell.name
                                        font.pixelSize: 11
                                        opacity: cell.onCursor ? 1 : 0.88
                                        horizontalAlignment: Text.AlignHCenter
                                    }
                                }

                                HoverHandler { id: cellHover }

                                TapHandler {
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onTapped: function (point, button) {
                                        Workspace.focusedPane = filesRoot.paneIndex
                                        filesRoot.setCursor(cell.index)
                                        win.takeFocus()
                                        if (button === Qt.RightButton) {
                                            const pos = cell.mapToItem(root, point.position.x,
                                                                       point.position.y)
                                            win.openFileMenuForPath(cell.path, pos.x, pos.y)
                                        }
                                    }
                                    onDoubleTapped: {
                                        Workspace.focusedPane = filesRoot.paneIndex
                                        filesRoot.setCursor(cell.index)
                                        win.takeFocus()
                                        folder.activateRow(cell.index)
                                    }
                                }
                            }
                        }
                    }
                }

            }
        }

    // ------------------------------------------------------------ graph view
    Component {
        id: graphView
        Item {
            id: graphRoot
            anchors.fill: parent
            // Same as the files view: read off the Loader that holds this one.
            // qmllint disable missing-property
            property var tab: parent.tab
            // qmllint enable missing-property

            function syncActive() {
                if (graphRoot.tab && Workspace.currentTab === graphRoot.tab)
                    win.activeGraph = graph
                else if (win.activeGraph === graph)
                    win.activeGraph = null
            }

            Component.onCompleted: syncActive()
            Component.onDestruction: {
                if (win.activeGraph === graph)
                    win.activeGraph = null
            }
            onTabChanged: syncActive()

            Connections {
                target: Workspace
                function onCurrentTabChanged() { graphRoot.syncActive() }
                function onFocusedPaneChanged() { graphRoot.syncActive() }
            }

            GraphItem {
                Accessible.role: Accessible.Graphic
                Accessible.name: graphRoot.tab ? graphRoot.tab.title : "Graph"
                id: graph
                anchors.fill: parent
                index: IndexModel
                // `Theme`, the singleton type, not a context property called
                // `theme` -- which is what `GraphItem`'s own property of that
                // name is called, so `theme: theme` would bind it to itself and
                // leave the graph drawing in hardcoded colours.
                theme: Theme
                selectedId: graphRoot.tab ? graphRoot.tab.selectedNode : ""
                onSelectedIdChanged: if (graphRoot.tab) graphRoot.tab.selectedNode = selectedId
                // A tag node names files, so picking one lists them: a files tab
                // scoped to that tag, holding every file that carries it
                // wherever it lives, which is what picking the same tag in the
                // dock does to the list beside it. A graph tab's own
                // selectedTag reaches nothing but the tab's title.
                onNodeActivated: function (id, kind, label) {
                    if (kind === "file") {
                        IndexModel.openPath(label)
                        return
                    }
                    Workspace.addTab("files")
                    if (Workspace.currentTab)
                        Workspace.currentTab.selectedTag = label
                }
                onNodeMenuRequested: function (id, kind, path, x, y) {
                    if (kind !== "file" || !path)
                        return
                    const pos = mapToItem(root, x, y)
                    win.openFileMenuForPath(path, pos.x, pos.y)
                }
            }

            Label {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 12
                quiet: true
                text: "#tag is a tag · j k walks the map · l opens · m for the menu"
            }
        }
    }
}
