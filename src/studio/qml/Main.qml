pragma ComponentBehavior: Bound

import QtQuick
import QtQml.Models
import QtQuick.Window
import QtQuick.Controls.Basic as C
import QtQuick.Templates as T
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
    property bool firstFrameShown: false
    onFrameSwapped: firstFrameShown = true
    width: 1180
    height: 720
    minimumWidth: 880
    minimumHeight: 480
    visible: true
    title: "omafiles"
    color: Theme.background
    onClosing: Qt.quit()

    property var receiptDialogs: []
    property TrashView activeTrash: null
    readonly property bool onTrash: activeTab && activeTab.kind === "trash" && activeTrash !== null
    function showReceipt(title, text) {
        const dialog = receiptDialogComponent.createObject(win.contentItem, {
            title: title, receipt: text
        }) as C.Dialog
        receiptDialogs = receiptDialogs.concat([dialog])
        dialog.open()
    }

    function reloadIndex() {
        changeNoticeContext()
        IndexModel.reload()
        if (activeFolder)
            activeFolder.reload()
    }

    function changeNoticeContext() {
        IndexModel.changeNoticeContext(activeFolder ? activeFolder.directory : "")
    }
    onActiveFolderChanged: changeNoticeContext()

    Connections {
        target: Workspace
        function onCurrentTabChanged() { win.changeNoticeContext() }
        function onFocusedPaneChanged() { win.changeNoticeContext() }
        function onPropertiesRequested(text: string) {
            win.showReceipt("Properties", text)
        }
    }
    Connections {
        target: win.activeFolder
        function onDirectoryChanged() { win.changeNoticeContext() }
        function onSelectedTagChanged() { win.changeNoticeContext() }
        function onQueryChanged() { win.changeNoticeContext() }
        function onPatternChanged() { win.changeNoticeContext() }
        function onRecentChanged() { win.changeNoticeContext() }
        function onFacetsChanged() { win.changeNoticeContext() }
    }
    Connections {
        target: Views
        function onNotice(message: string) { IndexModel.setNotice(message) }
    }
    Connections {
        target: IndexModel
        function onDefaultManagerFinished(ok: bool, text: string) {
            win.showReceipt(ok ? "Default file manager" : "Default file manager: incomplete", text)
        }
    }
    Connections {
        target: Devices
        function onOpenRequested(path: string) {
            Workspace.openTabIn(Workspace.focusedPane, "files", path)
            win.takeFocus()
        }
        function onNotice(message: string) { IndexModel.setNotice(message) }
        function onStorageChanged() { IndexModel.refreshStorage() }
    }

    Component {
        id: receiptDialogComponent
        C.Dialog {
            id: receiptDialog
            property string receipt: ""
            readonly property Item focusTarget: propertyText
            objectName: "propertiesDialog"
            parent: win.contentItem
            anchors.centerIn: parent
            width: Math.min(700, Math.max(0, win.width - 16))
            height: Math.min(480, Math.max(0, win.height - 16))
            title: "Properties"
            font.family: Theme.fontFamily
            font.pixelSize: 12
            palette.window: Theme.panel
            palette.windowText: Theme.foreground
            palette.text: Theme.foreground
            palette.base: Theme.panel
            palette.button: Theme.sunken
            palette.buttonText: Theme.foreground
            palette.highlight: Theme.accent
            palette.highlightedText: Theme.background
            modal: true
            popupType: C.Popup.Item
            onOpened: propertyText.forceActiveFocus()
            onClosed: {
                win.receiptDialogs = win.receiptDialogs.filter(function (dialog) { return dialog !== receiptDialog })
                destroy()
            }
            footer: Item {
                implicitHeight: 64
                Chip {
                    objectName: "receiptClose"
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(100, implicitWidth)
                    height: 40
                    label: "Close"
                    onClicked: receiptDialog.close()
                }
            }
            contentItem: OverlayScroll {
                id: propertiesScroll
                contentHeight: propertyText.height
                ReadOnlyText {
                    id: propertyText
                    objectName: "propertyText"
                    width: propertiesScroll.width
                    height: Math.max(implicitHeight, propertiesScroll.height)
                    focusFrame: propertiesScroll
                    onCursorRectangleChanged: if (activeFocus) propertiesScroll.revealRect(propertyText, cursorRectangle)
                    text: receiptDialog.receipt
                    wrapMode: TextEdit.Wrap
                    Keys.onPressed: function (event) {
                        if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
                            const delta = propertiesScroll.height * 0.9
                                * (event.key === Qt.Key_PageDown ? 1 : -1)
                            const next = positionAt(cursorRectangle.x, cursorRectangle.y + delta)
                            propertiesScroll.handleKey(event)
                            if (event.modifiers & Qt.ShiftModifier)
                                moveCursorSelection(next)
                            else
                                cursorPosition = next
                        }
                    }
                }
            }
        }
    }

    property var activeFilesRoot: null
    property var activeGraph: null
    property var activeDock: null
    property bool filtering: false
    property bool editingPath: false
    /// What the bottom bar is asking for, or nothing: a name for a view, a new
    /// name for the row, a name for a folder. Every prompt borrows the filter
    /// bar -- one text box for one prompt at a time, where two bars would be
    /// two places to look.
    property string prompt: ""
    readonly property bool naming: prompt === "view"
    readonly property bool renaming: prompt === "rename"
    readonly property bool renamingSet: prompt === "set"
    readonly property bool makingFolder: prompt === "folder"
    readonly property bool matching: prompt === "match"
    readonly property bool finding: prompt === "find"
    /// The row the rename prompt is about, or the set. Held here and not read
    /// off the cursor when the answer comes, because the list can rebuild
    /// under an open prompt and the cursor is not promised to stay.
    property string promptPath: ""
    property var promptPaths: []
    readonly property string focusRegion: {
        const item = activeFocusItem
        if (editingPath && insideOverlay(pathField, item)) return "path"
        if (filterBar.visible && insideOverlay(filterBar, item)) return "filter"
        if (activeDock) {
            if (item === activeDock.devicesFocusTarget || insideOverlay(activeDevicesList, item)) return "devices"
            if (item === activeDock.tagsFocusTarget || insideOverlay(activeTagsList, item)) return "tags"
            if (item === activeDock.viewsFocusTarget || insideOverlay(activeViewsList, item)) return "views"
        }
        return "list"
    }
    property bool pendingG: false
    property var paneItems: ({})
    /// Where the last menu was opened, so a second page of one lands on it.
    property real menuX: 0
    property real menuY: 0


    readonly property var activeTab: Workspace.currentTab
    readonly property var activeFolder: activeFilesRoot ? activeFilesRoot.folder : null
    readonly property var activeListing: activeFilesRoot ? activeFilesRoot.view : null
    readonly property var activeTagsList: activeDock ? activeDock.tagsList : null
    readonly property var activeDevicesList: activeDock ? activeDock.devicesList : null
    readonly property var activeViewsList: activeDock ? activeDock.viewsList : null
    // A list is on screen. Two kinds of tab draw one: a folder, and a set of
    // tags read as folders. They share the view, the keys and the model, so
    // almost everything that asks this question means either of them.
    readonly property bool onFiles: activeTab !== null && activeTab !== undefined
                                    && (activeTab.kind === "files" || activeTab.kind === "tags")
                                    && activeFilesRoot !== null
    /// The narrower question, for the handful of things that are about a place
    /// in the tree: the home key, and a path that names a folder.
    readonly property bool onDirectory: onFiles && activeTab.kind === "files"
    /// Narrower still: a files tab showing a folder, and not a tag, a search
    /// or the desktop's list -- the only listing something can be made or
    /// pasted into, because it is the only one that is somewhere.
    function isFolderTab(tab) {
        return tab && tab.kind === "files" && tab.selectedTag === ""
            && tab.pattern === "" && !tab.recent
    }
    readonly property bool standingInFolder: onDirectory && isFolderTab(activeTab)
    readonly property bool onTags: onFiles && activeTab.kind === "tags"
    readonly property bool onGraph: activeTab !== null && activeTab !== undefined
                                    && activeTab.kind === "graph" && activeGraph !== null
    readonly property var overlays: [filterBar, pathField, commandPalette, helpSheet, previewSheet,
        fileMenu, tabMenu, openWithMenu, scriptsMenu, viewMenu, deviceMenu, tagMenu, modeSheet, tagSheet, conflictSheet, archiveSheet,
        filterMenu, columnMenu, newTabMenu, trashMenu, trashEmptyMenu].concat(receiptDialogs)
    property var overlayStack: []
    property Item overlayOrigin: null
    property int overlayRevision: 0
    readonly property bool overlayOpen: overlayStack.some(function (entry) {
        return !win.inlineOverlay(entry.overlay)
    })

    function inlineOverlay(overlay) { return overlay === filterBar || overlay === pathField }

    function insideOverlay(overlay, item) {
        if (!overlay)
            return false
        if (inlineOverlay(overlay) && item && item.objectName === "hintAction") {
            for (let parent = item.parent; parent; parent = parent.parent)
                if (parent === status) return true
        }
        const container = overlay instanceof T.Popup ? overlay.contentItem.parent : overlay
        for (let parent = item; parent; parent = parent.parent) {
            if (parent === container)
                return true
        }
        return false
    }

    function focusControl(item) {
        if (item && item.activeFocusOnTab) return item
        for (let parent = item; parent; parent = parent.parent)
            if (parent instanceof ListView) return parent
        return item
    }

    function rememberOverlayFocus() {
        const top = overlayStack[overlayStack.length - 1]
        if (top && insideOverlay(top.overlay, activeFocusItem))
            top.focus = focusControl(activeFocusItem)
        else if (!top && activeFocusItem && activeFocusItem !== contentItem
                 && !overlays.some(function (overlay) { return overlay.visible && insideOverlay(overlay, activeFocusItem) }))
            overlayOrigin = focusControl(activeFocusItem)
    }
    function keepOverlayFocus() {
        const top = overlayStack[overlayStack.length - 1]
        if (top && !insideOverlay(top.overlay, activeFocusItem))
            restoreOverlayFocus()
    }
    onActiveFocusItemChanged: {
        rememberOverlayFocus()
        Qt.callLater(keepOverlayFocus)
    }

    function restoreOverlayFocus() {
        const top = overlayStack[overlayStack.length - 1]
        if (top) {
            const item = top.focus
            if (item && item.visible && item.enabled && insideOverlay(top.overlay, item))
                item.forceActiveFocus()
            else
                top.overlay.focusTarget.forceActiveFocus()
        }
    }

    function trackOverlay(overlay) {
        const next = overlayStack.slice()
        const index = next.findIndex(function (entry) { return entry.overlay === overlay })
        if (overlay.visible) {
            if (index >= 0)
                return
            rememberOverlayFocus()
            next.push({ overlay: overlay, focus: null })
        } else {
            if (index < 0)
                return
            next.splice(index, 1)
        }
        for (let i = 0; i < next.length; ++i) {
            if (!(next[i].overlay instanceof T.Popup) && !inlineOverlay(next[i].overlay))
                next[i].overlay.z = 81 + i
        }
        overlayStack = next
        const revision = ++overlayRevision
        if (overlay.visible || index !== next.length)
            return
        // Wait for Qt's popup focus cleanup and any command opened by the closing menu.
        Qt.callLater(function () {
            if (revision !== overlayRevision)
                return
            if (overlayStack.length > 0)
                restoreOverlayFocus()
            else if (!activeFocusItem || activeFocusItem === contentItem || activeFocusItem === root
                     || !activeFocusItem.visible || insideOverlay(overlay, activeFocusItem)) {
                if (overlayOrigin && overlayOrigin.visible && overlayOrigin.enabled)
                    overlayOrigin.forceActiveFocus()
                else
                    takeFocus()
            }
        })
    }

    Instantiator {
        model: win.overlays
        delegate: Connections {
            required property var modelData
            target: modelData
            function onVisibleChanged() { win.trackOverlay(modelData) }
        }
    }
    readonly property bool blocked: overlayOpen || editingPath || filtering
                                    || prompt !== ""

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
        { title: "devices", keys: [
            { key: "d", label: "devices panel" },
            { key: "j k / ↑ ↓", label: "move; Home/End/Page Up/Page Down also work" },
            { key: "Enter Space l", label: "open device; mount if needed" },
            { key: "m Menu Shift F10", label: "mount, unmount, eject or cancel" },
            { key: "h ← Esc", label: "return to files" } ] },
        { title: "archives", keys: [
            { key: "m", label: "row menu: Compress or Extract" },
            { key: ":", label: "commands: compress or extract" },
            { key: "Tab Shift+Tab", label: "destination, format and action controls" },
            { key: "Escape", label: "cancel preparation; stop publication after current entry" } ] },
        { title: "go", keys: [
            { key: "i", label: "quick preview" },
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
        { title: "preview", keys: [
            { key: "i Esc", label: "close preview" },
            { key: "j k", label: "next, previous file" },
            { key: "← →", label: "previous, next file" },
            { key: "P", label: "play, pause media" },
            { key: "[ ]", label: "seek five seconds" },
            { key: "PgDn PgUp", label: "scroll preview" },
            { key: "Home End", label: "scroll to ends" },
            { key: "Tab", label: "preview controls" } ] },
        { title: "mark", keys: [
            { key: "Space", label: "mark row" },
            { key: "J K", label: "mark row, then down, up" },
            { key: "a", label: "mark all" },
            { key: "A", label: "clear marks" },
            { key: "*", label: "mark by pattern" },
            { key: "m", label: "menu for a row, a tab or a view" },
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
        { title: "edit", keys: [
            { key: "Ctrl z", label: "undo the last operation" },
            { key: "r", label: "rename" },
            { key: "F2", label: "rename" },
            { key: "n", label: "new folder" },
            { key: "Ctrl Shift n", label: "new folder" },
            { key: "Ctrl c", label: "copy the row or the marked" },
            { key: "Ctrl x", label: "cut the row or the marked" },
            { key: "Ctrl v", label: "paste into this folder" },
            { key: "Ctrl Shift v", label: "link clipboard files here" } ] },
        { title: "find", keys: [
            { key: "/", label: "filter this folder" },
            { key: "f", label: "find by name across the roots" },
            { key: "F", label: "filter by type, tag or date" },
            { key: "Enter", label: "keep filter" },
            { key: "Esc", label: "clear filter" },
            { key: "t", label: "tag panel; a adds, d removes, r renames" },
            { key: "v", label: "saved views" },
            { key: "Enter", label: "toggle tag" },
            { key: "Space", label: "toggle tag" },
            { key: "m Menu Shift F10", label: "tag menu" },
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
            { key: "Ctrl ,", label: "narrow the pane" },
            { key: "Ctrl .", label: "widen the pane" },
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
            { key: "Shift h j k l", label: "pan" },
            { key: "0", label: "reset the view" } ] },
        { title: "trash", keys: [
            { key: ":", label: "Open Trash from commands" },
            { key: "j k / ↑ ↓", label: "move; Home/End/Page Up/Page Down also work" },
            { key: "Space / a / A", label: "mark / mark all / clear marks" },
            { key: "Enter", label: "read the complete original path and deletion details" },
            { key: "r", label: "restore row or marked entries" },
            { key: "m / Menu", label: "row menu" },
            { key: "e", label: "confirm empty Trash (permanent)" },
            { key: "F5", label: "refresh Trash" } ] },
        { title: "window", keys: [
            { key: ":", label: "commands" },
            { key: "Ctrl p", label: "commands" },
            { key: "?", label: "this list" },
            { key: "Tab", label: "next control or receipt" },
            { key: "Esc", label: "back to the list" } ] }
    ]

    // Hints answer the question in front of you. Least important first: the row
    // is right-aligned and drops from its left end when the window narrows.
    readonly property var hintsForRegion: {
        if (editingPath)
            return [{ key: "Esc", label: "cancel" }, { key: "Enter", label: "go" }]
        if (filtering)
            return [{ key: "Esc", label: "clear" }, { key: "Enter", label: "keep" }]
        if (prompt !== "")
            return [{ key: "Esc", label: "cancel" },
                    { key: "Enter", label: renaming || renamingSet ? "rename"
                                         : makingFolder ? "make"
                                         : matching ? "mark"
                                         : finding ? "find" : "save" }]
        if (onTrash)
            return [{ key: "?", label: "keys" }, { key: ":", label: "cmd" },
                    { key: "Space", label: "mark" }, { key: "m", label: "menu" },
                    { key: "r", label: "restore" }, { key: "e", label: "empty" },
                    { key: "j k", label: "move" }]
        if (onGraph)
            return [{ key: "?", label: "keys" },
                    { key: ":", label: "cmd" },
                    { key: "0", label: "reset view" },
                    { key: "+ - =", label: "zoom" },
                    { key: "m", label: "menu" },
                    { key: "l", label: "open" },
                    { key: "j k", label: "node" }]
        if (focusRegion === "devices" || focusRegion === "views")
            return [{ key: "?", label: "keys" },
                    { key: "Esc", label: "back" },
                    { key: "m", label: "menu" },
                    { key: "j k", label: "move" },
                    { key: "Enter", label: "open" }]
        if (focusRegion === "tags")
            return [{ key: "?", label: "keys" },
                    { key: "Esc", label: "back" },
                    { key: "m", label: "menu" },
                    { key: "j k", label: "move" },
                    { key: "Enter", label: "toggle" }]
        return [{ key: "?", label: "keys" },
                { key: ":", label: "cmd" },
                { key: "Space", label: "mark" },
                { key: "v", label: "views" },
                { key: "t", label: "tags" },
                { key: "/", label: "filter" },
                { key: "m", label: "menu" },
                { key: "h", label: "up" },
                { key: "l", label: "open" },
                { key: "j k", label: "move" }]
    }

    // ----------------------------------------------------------------- focus
    function takeFocus() {
        if (overlayStack.length > 0) {
            restoreOverlayFocus()
            return
        }
        if (onTrash) activeTrash.focusTarget.forceActiveFocus()
        else root.forceActiveFocus()
        if (activeDock) activeDock.expanded = false
    }

    function openPreview(path) {
        const file = path || cursorPath()
        if (file) previewSheet.openFor(file)
    }

    function stepPreview(direction) {
        if (!activeFolder || !activeListing) return
        const row = activeFolder.previewRow(activeListing.currentIndex, direction)
        if (row < 0) return
        moveCursorTo(row)
        previewSheet.openFor(activeFolder.pathAt(row))
    }

    function startFilter() {
        if (!onFiles)
            return
        filtering = true
        filterField.text = activeTab.filter
        filterField.focusEntry()
    }

    function finishFilter(clear) {
        if (clear && activeTab)
            activeTab.filter = ""
        filtering = false
        takeFocus()
    }

    /// The row the menu under `F` was opened over. Held rather than bound to
    /// the cursor, like `fileMenu.targetPath`, so a closed menu is not rebuilt
    /// on every step.
    property var filterRow: ({})

    // The criteria a tab can be narrowed by, each an entry that toggles it. The
    // type and the tags come off `row`, the way `Mark same type` reads them;
    // the palette passes no row and lists the rest.
    function filterActionsFor(row) {
        if (!onFiles)
            return []
        const offered = [{ criterion: "kind", value: "dir" },
                         { criterion: "kind", value: "file" }]
        if (row.kind === "file")
            offered.push({ criterion: "type", value: row.type })
        offered.push({ criterion: "tagged", value: true },
                     { criterion: "tagged", value: false })
        const tags = row.kind === "file" || row.kind === "dir" ? row.tags : []
        for (let i = 0; i < tags.length; i++)
            offered.push({ criterion: "tag", value: tags[i] })
        for (let i = 0; i < criteriaBar.presets.length; i++)
            offered.push({ criterion: "within", value: criteriaBar.presets[i].seconds })

        const held = activeTab.criteria
        const out = []
        for (let i = 0; i < offered.length; i++) {
            const one = offered[i]
            const on = held[one.criterion] === one.value
            out.push({ id: "only-" + one.criterion + ":" + one.value,
                       label: (on ? "\u2713 " : "") + "Only "
                              + criteriaBar.label(one.criterion, one.value),
                       criterion: one.criterion, value: one.value })
        }
        if (Object.keys(held).length > 0)
            out.push({ id: "clear-criteria", label: "Clear filters" })
        return out
    }

    function narrow(id) {
        if (!activeTab)
            return
        if (id === "clear-criteria") {
            activeTab.criteria = ({})
            return
        }
        const one = filterActionsFor(filterRow).find(function (action) {
            return action.id === id
        })
        if (one)
            activeTab.toggleCriterion(one.criterion, one.value)
    }

    function openFilterMenu() {
        if (!onFiles)
            return
        filterRow = activeFilesRoot.entry
        filterMenu.openAt(root.width / 2 - 100, root.height / 3)
    }

    // Saving what is on screen: the folder, or the tag query. The name is the
    // only thing that cannot be read off the window, so it is the only thing
    // asked for.
    function startNaming() {
        if (!onFiles)
            return
        prompt = "view"
        filterField.text = onTags ? win.facetPath(activeTab.facets, activeTab.untagged)
                                  : win.nameOf(activeFolder ? activeFolder.directory : "")
        filterField.focusEntry()
    }

    function finishNaming(accept, text) {
        prompt = ""
        if (accept && activeTab) {
            if (onTags)
                Views.saveQuery(text, activeTab.facets)
            else if (activeFolder)
                Views.saveFolder(text, activeFolder.directory)
        }
        takeFocus()
    }

    // The row under the cursor, or the marked set it stands for, as a list of
    // paths. Nothing for a facet or the `..` row, which are not files.
    function cursorPath() {
        const folder = activeFolder
        const listing = activeListing
        if (!onFiles || !folder || !listing || listing.currentIndex < 0)
            return ""
        const kind = folder.kindAt(listing.currentIndex)
        if (kind === "parent" || kind === "tag" || kind === "untagged")
            return ""
        return folder.pathAt(listing.currentIndex)
    }

    // Renaming is `mv` to the same folder, so the prompt offers the name and
    // the verb keeps the tags. The stem is selected and the extension is not:
    // the extension is the part nobody opened the prompt to change.
    function startArchive(extracting, target) {
        if (!onFiles || !activeFolder || IndexModel.operationBusy) return
        const path = target || cursorPath()
        if (!path) return
        const paths = activeFolder.targetPaths(path)
        if (extracting && paths.length !== 1) {
            IndexModel.setNotice("Extract one archive at a time; clear the other marks first")
            return
        }
        archiveSheet.openFor(paths, extracting, parentOf(path))
    }

    function startRename(target) {
        const path = target || cursorPath()
        if (!path || !onFiles)
            return
        // The rule the menu keeps: a marked row stands for the set, and a set
        // has no one name to offer -- it takes a template instead.
        const set = activeFolder ? activeFolder.targetPaths(path) : [path]
        if (set.length > 1) {
            startRenameSet(set)
            return
        }
        promptPath = path
        prompt = "rename"
        const name = nameOf(path)
        filterField.text = name
        const dot = name.lastIndexOf(".")
        filterField.focusEntry(dot > 0 ? dot : -1)
    }

    function finishRename(accept, text) {
        const path = promptPath
        prompt = ""
        promptPath = ""
        takeFocus()
        const name = text.trim()
        if (!accept || name === "" || name === nameOf(path))
            return
        const dir = parentOf(path)
        const to = (dir === "/" ? "" : dir) + "/" + name
        if (activeFolder)
            activeFolder.pointCursorAt(to)
        IndexModel.rename(path, to)
    }

    // ------------------------------------------------------------- clipboard
    //
    // The desktop's clipboard, in the formats its file managers use, so what is
    // copied here pastes in Nautilus and what is copied there pastes here. The
    // paste is `cp` or `mv` into the folder on screen, which is where the
    // refusals live: nothing here decides what happens to a name already taken.
    function copyToClipboard(cut) {
        const folder = activeFolder
        const path = cursorPath()
        if (!folder)
            return false
        // The row, or the marked set it stands for; with the cursor off every
        // file, the marked set alone.
        const paths = path !== "" ? folder.targetPaths(path) : folder.markedPaths()
        if (paths.length === 0)
            return false
        Clipboard.copyFiles(paths, cut)
        return true
    }

    // Whether a paste has somewhere to land: a folder, and not a tag listing,
    // which spans the index and is nowhere in particular.
    readonly property bool canPaste: standingInFolder && Clipboard.holdsFiles

    function paste() {
        const folder = activeFolder
        if (!canPaste || !folder)
            return false
        const paths = Clipboard.files()
        const dir = folder.directory
        transferFiles(paths, dir, Clipboard.cutting() ? Qt.MoveAction : Qt.CopyAction)
        if (Clipboard.cutting())
            Clipboard.clear()
        return true
    }

    function transferFiles(paths, dir, action) {
        const folder = activeFolder
        // One file lands under the cursor in the receiving folder.
        if (folder && folder.directory === dir && paths.length === 1)
            folder.pointCursorAt((dir === "/" ? "" : dir) + "/" + nameOf(paths[0]))
        if (action === Qt.MoveAction)
            IndexModel.moveInto(paths, dir)
        else if (action === Qt.LinkAction)
            IndexModel.linkInto(paths, dir)
        else
            IndexModel.copyInto(paths, dir)
    }

    Connections {
        target: FileTransfer
        function onRequested(paths, directory, action) { win.transferFiles(paths, directory, action) }
        function onRefused(reason) { IndexModel.setNotice(reason) }
    }

    // A shell pattern over every name under the declared roots, answered out
    // of the lists `sweep` writes: `ls --name <glob> --cached`, as a listing.
    // The tab stays where it was, and `h` brings it back there.
    function startFinding() {
        if (!onDirectory || !activeFolder)
            return
        prompt = "find"
        filterField.text = activeTab.pattern
        filterField.focusEntry()
    }

    function finishFinding(accept, text) {
        prompt = ""
        takeFocus()
        const pattern = text.trim()
        if (!accept || pattern === "" || !activeTab)
            return
        activeTab.selectedTag = ""
        activeTab.recent = false
        activeTab.pattern = pattern
    }

    // What the desktop opened lately, as a listing in the tab: `omafiles
    // recent --desktop`, walked. From anywhere but a files tab it opens one.
    function openRecent() {
        if (!onDirectory)
            Workspace.openTabIn(Workspace.focusedPane, "files",
                                activeFolder ? activeFolder.directory : "")
        const tab = Workspace.currentTab
        if (!tab || tab.kind !== "files")
            return
        tab.selectedTag = ""
        tab.pattern = ""
        tab.recent = true
    }

    // A shell pattern over the names on screen, added to the marks. The count
    // it made is on the status bar already, so the prompt has no receipt.
    function startMarking() {
        if (!onFiles || !activeFolder)
            return
        prompt = "match"
        filterField.text = ""
        filterField.focusEntry()
    }

    function finishMarking(accept, text) {
        prompt = ""
        takeFocus()
        if (accept && activeFolder && text.trim() !== "")
            activeFolder.markMatching(text)
    }

    // A set renamed by `mv --pattern`: the template is the one thing typed,
    // and the verb settles every name before it moves any.
    function startRenameSet(paths) {
        if (!onFiles || paths.length === 0)
            return
        promptPaths = paths
        prompt = "set"
        filterField.text = "{stem}{ext}"
        filterField.focusEntry()
    }

    function finishRenameSet(accept, text) {
        const paths = promptPaths
        prompt = ""
        promptPaths = []
        takeFocus()
        const pattern = text.trim()
        if (!accept || pattern === "" || pattern === "{name}")
            return
        IndexModel.renameSet(paths, pattern)
    }

    // `chmod` over the row, or the set it is marked in, once the sheet has
    // shown the mode and been told to go ahead.
    function startModeChange(target) {
        const path = target || cursorPath()
        const folder = activeFolder
        if (!path || !folder)
            return
        modeSheet.openFor(folder.targetPaths(path), folder.entryAt(folder.rowForPath(path)))
    }

    // ----------------------------------------------------------------- links
    function joined(dir, name) {
        return (dir === "/" ? "" : dir) + "/" + name
    }

    // `Link to <name>`, beside the target: the desktop's spelling, so a link
    // made here and one made in Nautilus read the same on disk.
    function linkBeside(path) {
        const folder = activeFolder
        if (!path || !folder)
            return
        const link = joined(parentOf(path), "Link to " + nameOf(path))
        if (!IndexModel.makeLink(path, link))
            return
        folder.pointCursorAt(link)
        folder.reload()
    }

    // The clipboard's files, as links in the folder on screen, under their own
    // names. The clipboard is left as it was: a link is not a paste.
    function linkHere() {
        const folder = activeFolder
        if (!canPaste || !folder)
            return false
        const paths = Clipboard.files()
        const dir = folder.directory
        transferFiles(paths, dir, Qt.LinkAction)
        return true
    }

    // Only where the list is a folder: a tag listing spans the index and has
    // nowhere to put one.
    function startMakingFolder() {
        if (!standingInFolder || !activeFolder)
            return
        prompt = "folder"
        filterField.text = ""
        filterField.focusEntry()
    }

    function finishMakingFolder(accept, text) {
        prompt = ""
        takeFocus()
        const name = text.trim()
        const folder = activeFolder
        if (!accept || name === "" || !folder)
            return
        const dir = folder.directory
        if (!IndexModel.makeFolder(dir, name))
            return
        // Listed now rather than when the watcher gets round to it, and with
        // the cursor on it: what was just made is what you want to walk into.
        folder.pointCursorAt((dir === "/" ? "" : dir) + "/" + name)
        folder.reload()
    }

    function startPathEdit() {
        if (!onFiles)
            return
        editingPath = true
        pathField.text = onTags ? facetPath(activeTab.facets, activeTab.untagged)
                                : activeTab.cwd
        pathField.focusEntry()
    }

    function finishPathEdit(accept, text) {
        if (accept && activeFolder) {
            if (onTags) {
                // A query typed rather than walked to. The same words in
                // another order are the same question, so nothing here cares
                // which order they arrive in.
                if (text.trim().toLowerCase() === "(no tag)") {
                    activeTab.untagged = true
                } else {
                    activeTab.untagged = false
                    activeTab.facets = parseFacetPath(text)
                }
            } else {
                // Typing a path asks for that folder, which is a different
                // question from the tag or the search that may be on screen.
                if (activeTab) {
                    activeTab.selectedTag = ""
                    activeTab.pattern = ""
                    activeTab.recent = false
                    activeTab.cwd = text
                }
            }
        }
        editingPath = false
        takeFocus()
    }

    // A tags tab already standing on a query, which is what every way into one
    // means: made and then walked would show the whole vocabulary for a frame.
    function openTagsTab(facets) {
        Workspace.addTab("tags")
        if (Workspace.currentTab)
            Workspace.currentTab.facets = facets
    }

    // ---------------------------------------------------------------- facets
    //
    // A list of tags, written the way the tab strip and the graph already spell
    // one, and read back from whatever somebody types: with or without the
    // hashes, separated by slashes, spaces or commas.
    function facetPath(facets, untagged) {
        // The complement is not a tag and cannot be spelled as one, so it is
        // spelled as what it is. Typing it back is how you get there without
        // walking, which `parseFacetPath` is the other half of.
        if (untagged)
            return "(no tag)"
        if (!facets || facets.length === 0)
            return ""
        const sorted = facets.slice().sort()
        return "#" + sorted.join("/#")
    }

    function parseFacetPath(text) {
        const out = []
        const words = text.split(/[\/\s,]+/)
        for (let i = 0; i < words.length; i++) {
            const word = words[i].replace(/^#/, "").trim().toLowerCase()
            if (word !== "" && out.indexOf(word) < 0)
                out.push(word)
        }
        return out
    }

    function tagRow(name) {
        for (let i = 0; i < IndexModel.tagRowCount(); ++i) {
            if (IndexModel.tagAt(i) === name)
                return i
        }
        return -1
    }

    function focusTags() {
        if (!activeTagsList)
            return
        // On the tag the listing is showing, when there is one: the way out of
        // a tag listing is the key that opened it, and hunting for the row
        // again is not a way out.
        activeDock.expanded = true
        activeDock.tagsFocusTarget.forceActiveFocus()
        const chosen = activeTab ? tagRow(activeTab.selectedTag) : -1
        if (chosen >= 0)
            activeTagsList.currentIndex = chosen
        else if (activeTagsList.currentIndex < 0 && IndexModel.tagRowCount() > 0)
            activeTagsList.currentIndex = 0
    }

    function startTagEdit(operation, path, tag) {
        const folder = activeFolder
        const listing = activeListing
        if (!folder || !listing)
            return
        if (!path)
            path = folder.pathAt(listing.currentIndex)
        const row = folder.rowForPath(path)
        const kind = folder.kindAt(row)
        if (kind !== "file" && kind !== "dir")
            return
        tagSheet.openFor(folder.targetPaths(path), operation || "add", tag || "")
    }

    function focusDevices() {
        if (!activeDock) {
            Workspace.addTab("files")
            Qt.callLater(focusDevices)
            return
        }
        activeDock.expanded = true
        activeDock.devicesFocusTarget.forceActiveFocus()
    }

    function openDeviceMenu(id, x, y) {
        if (!activeDock) return
        activeDock.expanded = true
        const entries = Devices.entries
        for (let row = 0; row < entries.length; ++row)
            if (entries[row].id === id) activeDevicesList.currentIndex = row
        activeDock.devicesFocusTarget.forceActiveFocus()
        deviceMenu.targetId = id
        deviceMenu.openAt(x, y)
    }

    // Focused even with nothing in it, so `v` shows where views live rather
    // than doing nothing at all -- which is the same key answering the same
    // question either way.
    function focusViews() {
        if (!activeViewsList)
            return
        activeDock.expanded = true
        activeDock.viewsFocusTarget.forceActiveFocus()
        if (Views.count > 0 && activeViewsList.currentIndex < 0)
            activeViewsList.currentIndex = 0
    }

    // A view is a folder or a set of tags, and which it is only matters here.
    function openView(row) {
        const view = Views.at(row)
        if (!view || !view.name)
            return
        takeFocus()
        if (view.kind === "folder") {
            Workspace.openTabIn(Workspace.focusedPane, "files", view.dir)
            return
        }
        openTagsTab(view.tags)
    }

    function openTagMenuAt(row, x, y) {
        if (!activeTagsList || IndexModel.tagAt(row) === "") return
        activeTagsList.currentIndex = row
        activeDock.expanded = true
        activeDock.tagsFocusTarget.forceActiveFocus()
        tagMenu.targetRow = row
        tagMenu.openAt(x, y)
    }

    function toggleTagAt(row) {
        if (!activeTab || row < 0)
            return
        const name = IndexModel.tagAt(row)
        if (name === "")
            return
        activeTab.selectedTag = activeTab.selectedTag === name ? "" : name
    }

    // ----------------------------------------------------------------- paths
    //
    // The menu says things about paths -- copies them, walks to the folder
    // holding them, quotes them for a command line -- and none of that is
    // anything the models are for.
    function parentOf(path) {
        if (path === "" || path === "/")
            return path
        const cut = path.lastIndexOf("/")
        return cut <= 0 ? "/" : path.substring(0, cut)
    }

    function nameOf(path) {
        const cut = path.lastIndexOf("/")
        return cut < 0 ? path : path.substring(cut + 1)
    }

    // `encodeURI` leaves `#` and `?` where they are, and both of them end a
    // path early in whatever reads the URI back.
    function uriOf(path) {
        return "file://" + encodeURI(path).replace(/#/g, "%23").replace(/\?/g, "%3F")
    }

    // A path as a shell would have to be handed it. Left bare where every
    // character in it is one no shell reads as anything, quoted otherwise --
    // and a single quote inside single quotes is spelled by closing them.
    function shellWord(path) {
        if (/^[A-Za-z0-9_@%+=:,.\/-]+$/.test(path))
            return path
        return "'" + path.replace(/'/g, "'\\''") + "'"
    }

    function uriLines(paths) {
        const out = []
        for (let i = 0; i < paths.length; i++)
            out.push(win.uriOf(paths[i]))
        return out.join("\n")
    }

    function shellWords(paths) {
        const out = []
        for (let i = 0; i < paths.length; i++)
            out.push(win.shellWord(paths[i]))
        return out.join(" ")
    }

    // ------------------------------------------------------------------ menu
    function openFileMenuAt(x, y) {
        // The receipt from the last command answered the question that was
        // asked then. Opening the menu is as good a sign as any that it has
        // been read, and the status bar has one line to say things on.
        IndexModel.clearNotice()
        menuX = x
        menuY = y
        fileMenu.openAt(x, y)
    }

    // The list of applications, over where the entry that asked for it was.
    //
    // A second menu and not a panel that grows out of the side of the first:
    // this is the same component, opened in the same place, walked with the
    // same keys. `Esc` leaves it the way `Esc` leaves anything here.
    function openOpenWithMenu(path) {
        const apps = IndexModel.applicationsFor(path)
        if (apps.length === 0) {
            // Said rather than shown as an empty box: a menu with nothing in it
            // reads as a menu that failed to open.
            IndexModel.noApplicationFor(path)
            return
        }
        openWithMenu.apps = apps
        openWithMenu.targetPath = path
        openWithMenu.openAt(menuX, menuY)
    }

    // The person's own scripts, over where the entry that asked for them was.
    // The same second menu `Open with…` is, for the same reasons.
    function openScriptsMenu(paths, directory) {
        const scripts = IndexModel.scripts()
        if (scripts.length === 0)
            return
        scriptsMenu.scripts = scripts
        scriptsMenu.targetPaths = paths
        scriptsMenu.targetDir = directory
        scriptsMenu.openAt(menuX, menuY)
    }

    function columnLabel(field) {
        return field === "mtime" ? "modified" : field
    }

    // The details columns as commands: a toggle for every field, and a move each
    // way for each of `moving` that has somewhere to go. The heading menu and
    // the command palette both draw this list.
    function columnActions(moving) {
        const shown = Workspace.columns
        const out = []
        for (const field of Workspace.columnFields)
            out.push({ id: "column-toggle:" + field,
                       label: (shown.indexOf(field) >= 0 ? "Hide " : "Show ")
                              + "the " + columnLabel(field) + " column" })
        for (const field of moving) {
            const at = shown.indexOf(field)
            if (at > 0)
                out.push({ id: "column-left:" + field,
                           label: "Move the " + columnLabel(field) + " column left" })
            if (at >= 0 && at < shown.length - 1)
                out.push({ id: "column-right:" + field,
                           label: "Move the " + columnLabel(field) + " column right" })
        }
        return out
    }

    function runColumnAction(id) {
        const field = id.slice(id.indexOf(":") + 1)
        if (id.startsWith("column-toggle:"))
            Workspace.toggleColumn(field)
        else if (id.startsWith("column-left:"))
            Workspace.moveColumn(field, -1)
        else if (id.startsWith("column-right:"))
            Workspace.moveColumn(field, 1)
    }

    function openColumnMenu(moving, x, y) {
        columnMenu.moving = moving
        columnMenu.openAt(x, y)
    }

    function openFileMenuForPath(path, kind, x, y) {
        fileMenu.targetPath = path
        fileMenu.targetKind = kind
        openFileMenuAt(x, y)
    }

    // Opened by `m`, so it has to place itself: under the row if the row is on
    // screen, in the middle of the pane if there is no row to point at.
    function openFileMenuForListing() {
        const listing = activeListing
        const folder = activeFolder
        if (!listing || !folder || listing.currentIndex < 0) {
            fileMenu.targetPath = ""
            fileMenu.targetKind = ""
            openFileMenuAt(root.width / 2, root.height / 3)
            return
        }
        fileMenu.targetPath = folder.pathAt(listing.currentIndex)
        fileMenu.targetKind = folder.kindAt(listing.currentIndex)
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

    function openViewMenuAt(row, x, y) {
        if (row < 0 || row >= Views.count)
            return
        IndexModel.clearNotice()
        if (activeViewsList) {
            activeViewsList.currentIndex = row
            activeDock.expanded = true
            activeDock.viewsFocusTarget.forceActiveFocus()
        }
        viewMenu.targetRow = row
        viewMenu.openAt(x, y)
    }

    // Opened by `m` while the views are being walked, so it has to place
    // itself: under the row if the row is on screen, in the middle if not.
    function openViewMenuForCursor() {
        const views = activeViewsList
        if (!views || views.currentIndex < 0)
            return
        const item = views.itemAtIndex(views.currentIndex)
        if (!item) {
            openViewMenuAt(views.currentIndex, root.width / 2, root.height / 3)
            return
        }
        const pos = views.mapToItem(root, 0, item.y - views.contentY)
        openViewMenuAt(views.currentIndex, pos.x - 180, pos.y + item.height)
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
        IndexModel.clearNotice()
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
        if (focusRegion === "devices")
            return activeDevicesList ? Math.max(1, Math.floor(activeDevicesList.height / 32)) : 6
        if (focusRegion === "views")
            return activeViewsList ? Math.max(1, Math.floor(activeViewsList.height / 26)) : 6
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

    function moveViewCursor(delta) {
        const views = activeViewsList
        if (!views)
            return
        views.currentIndex = Math.max(0, Math.min(views.currentIndex + delta, Views.count - 1))
    }

    function moveByDelta(delta) {
        if (onGraph) {
            const graph = activeGraph
            if (graph)
                graph.stepSelection(delta)
            return
        }
        if (focusRegion === "devices") {
            const list = activeDevicesList
            if (list) list.currentIndex = Math.max(0, Math.min(list.currentIndex + delta, list.count - 1))
        } else if (focusRegion === "views")
            moveViewCursor(delta)
        else if (focusRegion === "tags")
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
        if (focusRegion === "devices") {
            const list = activeDevicesList
            if (list) list.currentIndex = last ? Math.max(0, list.count - 1) : 0
        } else if (focusRegion === "views") {
            const views = activeViewsList
            if (views)
                views.currentIndex = last ? Math.max(0, Views.count - 1) : 0
        } else if (focusRegion === "tags") {
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
        moveByDelta(step)
        return true
    }

    function handleModified(event, ctrl, alt, shift) {
        if (ctrl) {
            switch (event.key) {
            case Qt.Key_Z: IndexModel.undo(); return true
            case Qt.Key_L: startPathEdit(); return true
            case Qt.Key_P: commandPalette.open(); return true
            case Qt.Key_T: Workspace.addTab("files"); return true
            case Qt.Key_R: win.reloadIndex(); return true
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
            case Qt.Key_N:
                if (shift)
                    startMakingFolder()
                return shift
            case Qt.Key_C: return copyToClipboard(false)
            case Qt.Key_X: return copyToClipboard(true)
            case Qt.Key_V: return shift ? linkHere() : paste()
            case Qt.Key_Comma: return resizeFocusedPane(-40)
            case Qt.Key_Period: return resizeFocusedPane(40)
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

    function resizeFocusedPane(delta) {
        const pane = paneItems[Workspace.focusedPane]
        if (!pane)
            return false
        pane.panePreferredWidth = Math.max(280, pane.panePreferredWidth + delta)
        return true
    }

    // What the tags list and the views list share: `h` back to the files, and
    // the moves every list takes.
    function handleSideListKey(event, text) {
        if (text === "h" || event.key === Qt.Key_Left) {
            takeFocus()
            return true
        }
        return handleMoveKey(event, text) || handleEndKey(event, text, true)
    }

    function handleTagKey(event, text) {
        const tags = activeTagsList
        if (!tags)
            return false
        if (text === "a" || text === "d" || text === "r") {
            startTagEdit(text === "a" ? "add" : text === "d" ? "remove" : "rename",
                         "", text === "a" ? "" : IndexModel.tagAt(tags.currentIndex))
            return true
        }
        if (text === "m" || event.key === Qt.Key_Menu
                || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
            const at = activeDock.mapToItem(root, 12, 64)
            openTagMenuAt(tags.currentIndex, at.x, at.y)
            return true
        }
        if (handleSideListKey(event, text))
            return true
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Space) {
            toggleTagAt(tags.currentIndex)
            return true
        }
        return false
    }

    function handleDeviceKey(event, text) {
        if (handleSideListKey(event, text)) return true
        const list = activeDevicesList
        const device = list && list.currentIndex >= 0 ? Devices.entries[list.currentIndex] : null
        if (!device) return false
        if (text === "m" || event.key === Qt.Key_Menu
                || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
            const item = list.currentItem || list
            const at = item.mapToItem(root, 0, item.height)
            openDeviceMenu(device.id, at.x, at.y)
            return true
        }
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Space || text === "l") {
            Devices.act(device.id, "open")
            return true
        }
        return false
    }

    function handleViewKey(event, text) {
        const views = activeViewsList
        if (!views)
            return false
        if (handleSideListKey(event, text))
            return true
        if (text === "m" || event.key === Qt.Key_Menu
                || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
            openViewMenuForCursor()
            return true
        }
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Space || text === "l") {
            openView(views.currentIndex)
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
        case Qt.Key_F2: startRename(); return true
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
        case "i": openPreview(""); return true
        case "h": folder.goUp(); return true
        case "~": folder.goHome(); return true
        case "t": focusTags(); return true
        case "v": focusViews(); return true
        case "m": openFileMenuForListing(); return true
        case "a": folder.markAll(); return true
        case "A": folder.clearMarks(); return true
        case "r": startRename(); return true
        case "n": startMakingFolder(); return true
        case "*": startMarking(); return true
        case "f": startFinding(); return true
        case "F": openFilterMenu(); return true
        // Space and a step, in one key: the way a run of rows is marked
        // without the thumb going back and forth.
        case "J": folder.toggleMarkAt(listing.currentIndex); moveCursor(1); return true
        case "K": folder.toggleMarkAt(listing.currentIndex); moveCursor(-1); return true
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
        if (event.modifiers & Qt.ShiftModifier) {
            switch (event.key) {
            case Qt.Key_H:
            case Qt.Key_Left: graph.panBy(-40, 0); return true
            case Qt.Key_L:
            case Qt.Key_Right: graph.panBy(40, 0); return true
            case Qt.Key_K:
            case Qt.Key_Up: graph.panBy(0, -40); return true
            case Qt.Key_J:
            case Qt.Key_Down: graph.panBy(0, 40); return true
            }
        }
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

    function activateHint(caption) {
        const name = caption.split(" ")[0]
        if (editingPath || filtering || prompt !== "") {
            const entry = editingPath ? pathField : filterField
            if (name === "Esc") entry.cancelled()
            else if (name === "Enter") entry.accepted(entry.text)
            return
        }
        const key = name === "Esc" ? Qt.Key_Escape : name === "Enter" ? Qt.Key_Return
            : name === "Space" ? Qt.Key_Space : name.toUpperCase().charCodeAt(0)
        handleKey({ key: key, text: name === "Space" ? " " : name,
                    modifiers: Qt.NoModifier, accepted: false, isAutoRepeat: false })
    }

    function handleKey(event) {
        // A band can be drawn with the filter bar open, which blocks the rest.
        if (event.key === Qt.Key_Escape && activeFilesRoot && activeFilesRoot.band.live) {
            activeFilesRoot.band.cancel()
            event.accepted = true
            return
        }
        const top = overlayStack[overlayStack.length - 1]
        if (top && inlineOverlay(top.overlay) && event.key === Qt.Key_Escape) {
            activateHint("Esc")
            event.accepted = true
            return
        }
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

        if (text === "d" && focusRegion !== "tags") {
            focusDevices()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_F5) {
            Devices.refresh()
            if (onTrash) activeTrash.model.reload()
            win.reloadIndex()
            event.accepted = true
            return
        }
        if (text === "?") {
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
        if (onTrash) {
            activeTrash.handleKey(event)
            return
        }
        if (!onFiles)
            return

        const handled = focusRegion === "devices" ? handleDeviceKey(event, text)
                      : focusRegion === "views" ? handleViewKey(event, text)
                      : focusRegion === "tags"  ? handleTagKey(event, text)
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

        Component.onCompleted: forceActiveFocus()

        Column {
            enabled: !win.overlayOpen
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
                        objectName: "undoButton"
                        anchors.verticalCenter: parent.verticalCenter
                        iconKind: "undo"
                        caption: IndexModel.undoLabel
                        key: "Ctrl+Z"
                        usable: IndexModel.canUndo
                        onClicked: IndexModel.undo()
                    }
                    ToolButton {
                        anchors.verticalCenter: parent.verticalCenter
                        iconKind: "reload"
                        caption: "Reload index"
                        key: "F5"
                        onClicked: win.reloadIndex()
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
                        Accessible.role: Accessible.Button
                        Accessible.name: crumbs.full
                        Accessible.focusable: win.onFiles && crumbs.full !== ""
                        activeFocusOnTab: win.onFiles && crumbs.full !== ""
                        Keys.onReturnPressed: function (event) {
                            win.startPathEdit()
                            event.accepted = true
                        }
                        Keys.onSpacePressed: function (event) {
                            win.startPathEdit()
                            event.accepted = true
                        }

                        // A tag listing spans the index, so there is no path to
                        // print: it is spelled `#tag`, the way the graph and
                        // the tab strip spell it, rather than as a folder the
                        // window is not standing in. A tags tab spells the
                        // whole query the same way -- `#work/#draft` -- which
                        // is a path you can type as readily as a folder.
                        readonly property string full: {
                            if (!win.activeTab)
                                return ""
                            if (win.activeTab.kind === "tags")
                                return win.facetPath(win.activeTab.facets,
                                                     win.activeTab.untagged)
                            if (win.activeTab.kind !== "files")
                                return ""
                            // A search spans the roots the way a tag spans
                            // the index, so it is spelled as the pattern
                            // and not as a folder the window is not in.
                            if (win.activeTab.recent)
                                return "(recent)"
                            if (win.activeTab.pattern !== "")
                                return win.activeTab.pattern
                            return win.activeTab.selectedTag !== ""
                                   ? "#" + win.activeTab.selectedTag : win.activeTab.cwd
                        }
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
                    nextFocus: status.firstHint || pathField.focusTarget
                    previousFocus: status.lastHint || pathField.focusTarget
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
                        - criteriaBar.height - filterBar.height - dividers
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
                        C.SplitView.preferredWidth: panePreferredWidth
                    }
                }
            }

            CriteriaBar {
                id: criteriaBar
                objectName: "criteriaBar"
                width: parent.width
                criteria: win.onFiles ? win.activeTab.criteria : ({})
                height: held.length > 0 ? 30 : 0
                visible: height > 0
                onRemoved: function (key, value) { win.activeTab.toggleCriterion(key, value) }
                onCleared: win.narrow("clear-criteria")
            }

            // -------------------------------------------------------- filter
            Item {
                id: filterBar
                readonly property Item focusTarget: filterField.focusTarget
                width: parent.width
                height: win.filtering || win.prompt !== "" ? 34 : 0
                visible: height > 0
                clip: true

                Rectangle {
                    anchors.fill: parent
                    color: Theme.panel
                }

                Entry {
                    id: filterField
                    nextFocus: status.firstHint || filterField.focusTarget
                    previousFocus: status.lastHint || filterField.focusTarget
                    objectName: "filterEntry"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    lead: win.prompt !== "" ? "" : "/"
                    placeholder: win.naming ? "name this view"
                               : win.renaming ? "rename " + win.nameOf(win.promptPath)
                               : win.renamingSet ? "rename " + win.promptPaths.length
                                                   + " files: {name} {stem} {ext} {n}"
                               : win.makingFolder ? "new folder"
                               : win.matching ? "mark names matching, like *.pdf"
                               : win.finding ? "find names across the roots, like *.pdf"
                               : "filter this folder"
                    onTextChanged: if (win.filtering && win.activeTab) win.activeTab.filter = text
                    onAccepted: function (value) {
                        if (win.naming)
                            win.finishNaming(true, value)
                        else if (win.renaming)
                            win.finishRename(true, value)
                        else if (win.renamingSet)
                            win.finishRenameSet(true, value)
                        else if (win.makingFolder)
                            win.finishMakingFolder(true, value)
                        else if (win.matching)
                            win.finishMarking(true, value)
                        else if (win.finding)
                            win.finishFinding(true, value)
                        else
                            win.finishFilter(false)
                    }
                    onCancelled: {
                        if (win.naming)
                            win.finishNaming(false, "")
                        else if (win.renaming)
                            win.finishRename(false, "")
                        else if (win.renamingSet)
                            win.finishRenameSet(false, "")
                        else if (win.makingFolder)
                            win.finishMakingFolder(false, "")
                        else if (win.matching)
                            win.finishMarking(false, "")
                        else if (win.finding)
                            win.finishFinding(false, "")
                        else
                            win.finishFilter(true)
                    }
                }
            }

            Rule {}

            // -------------------------------------------------------- status
            StatusBar {
                id: status
                nextHintFocus: win.editingPath ? pathField.focusTarget
                    : filterBar.visible ? filterField.focusTarget : null
                previousHintFocus: nextHintFocus
                objectName: "statusBar"
                editing: win.editingPath || filterBar.visible
                onReceiptRequested: win.showReceipt("Receipt", status.message)
                onHintRequested: function (key) { win.activateHint(key) }
                width: parent.width
                // A graph tab counts nodes, because that is what it has: the
                // folder counts belong to a FolderModel it does not use, and
                // read out here they would be a confident `0/0`.
                cursor: win.onTrash ? win.activeTrash.cursor : win.onGraph ? win.activeGraph.selectedRow
                                    : (win.activeFilesRoot ? win.activeFilesRoot.cursor : -1)
                total: win.onTrash ? win.activeTrash.model.count : win.onGraph ? win.activeGraph.nodeCount
                                   : (win.activeFolder ? win.activeFolder.count : 0)
                note: win.onGraph ? win.activeGraph.summary : ""
                marked: win.onTrash ? win.activeTrash.model.markedCount : win.activeFolder ? win.activeFolder.markedCount : 0
                tagged: win.activeFolder ? win.activeFolder.taggedCount : 0
                pastable: Clipboard.holdsFiles ? Clipboard.files().length : 0
                cutting: Clipboard.holdsFiles && Clipboard.cutting()
                tag: win.activeTab ? win.activeTab.selectedTag : ""
                filter: win.activeTab ? win.activeTab.filter : ""
                message: Devices.busy ? Devices.status
                         : IndexModel.archive.busy ? IndexModel.archive.progress
                         : IndexModel.operationBusy ? "File operation in progress"
                         : win.onTrash && win.activeTrash.model.busy ? win.activeTrash.model.message
                         : win.onTrash && win.activeTrash.model.loading ? win.activeTrash.model.message
                         : win.onTrash && win.activeTrash.model.error !== "" ? win.activeTrash.model.error
                         : Views.error !== "" ? Views.error
                         : win.activeFolder && win.activeFolder.error !== "" ? win.activeFolder.error
                         : IndexModel.error !== "" ? IndexModel.error
                         : win.onTrash && win.activeTrash.model.message !== "" ? win.activeTrash.model.message
                         : IndexModel.notice
                hints: win.hintsForRegion
            }
        }

        // ------------------------------------------------------------ sheets
        CommandPalette {
            id: commandPalette
            objectName: "commandPalette"
            anchors.fill: parent
            commands: [
                { id: "add-files", label: "New files tab", key: "Ctrl+T" },
                { id: "add-tags", label: "New tags tab", key: "" },
                { id: "devices", label: "Devices", key: "d" },
                { id: "open-trash", label: "Open Trash", key: "" },
                { id: "restore-trash", label: "Restore from Trash", key: "r" },
                { id: "empty-trash", label: "Empty Trash (permanent)…", key: "e" },
                { id: "save-view", label: "Save this as a view", key: "" },
                { id: "views", label: "Saved views", key: "v" },
                { id: "add-graph", label: "New graph tab", key: "" },
                { id: "close-tab", label: "Close tab", key: "Ctrl+W" },
                { id: "split", label: "Split right", key: "Ctrl+\\" },
                { id: "close-pane", label: "Close pane", key: "Ctrl+Shift+W" },
                { id: "reload", label: "Reload index", key: "F5" },
                { id: "home", label: "Go to home folder", key: "~" },
                { id: "up", label: "Go to parent folder", key: "h" },
                { id: "path", label: "Type a path", key: "Ctrl+L" },
                { id: "filter", label: "Filter this folder", key: "/" },
                { id: "find", label: "Find by name across the roots", key: "f" },
                { id: "filter-menu", label: "Filter by type, tag or date", key: "F" },
                { id: "leave-search", label: "Leave the search", key: "h" },
                { id: "recent", label: "Recently opened on the desktop", key: "" },
                { id: "rename", label: "Rename", key: "r" },
                { id: "preview", label: "Quick preview", key: "i" },
                { id: "compress", label: "Compress row or marked files…", key: "" },
                { id: "extract", label: "Extract archive…", key: "" },
                { id: "chmod", label: "Permissions", key: "" },
                { id: "edit-tag-add", label: "Add tag to the row or marked files", key: "t a" },
                { id: "edit-tag-remove", label: "Remove tag from the row or marked files", key: "t d" },
                { id: "edit-tag-rename", label: "Rename tag on the row or marked files", key: "t r" },
                { id: "new-folder", label: "New folder", key: "n" },
                { id: "copy-files", label: "Copy", key: "Ctrl+C" },
                { id: "cut-files", label: "Cut", key: "Ctrl+X" },
                { id: "paste", label: "Paste", key: "Ctrl+V" },
                { id: "mark-all", label: "Mark all", key: "a" },
                { id: "clear-marks", label: "Clear marks", key: "A" },
                { id: "mark-pattern", label: "Mark by pattern", key: "*" },
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
                { id: "make-default", label: "Make omafiles the default file manager", key: "" },
                { id: "undo", label: IndexModel.undoLabel, key: "Ctrl+Z" },
                { id: "keys", label: "Show keys", key: "?" }
            ].concat(win.columnActions(Workspace.columns), win.filterActionsFor({}))
            onChosen: function (id) {
                // The filter entries are `narrow`'s; it passes over the rest.
                win.narrow(id)
                switch (id) {
                case "preview": Qt.callLater(win.openPreview, ""); break
                case "compress": Qt.callLater(win.startArchive, false, ""); break
                case "extract": Qt.callLater(win.startArchive, true, ""); break
                case "add-files": Workspace.addTab("files"); break
                case "add-tags": Workspace.addTab("tags"); break
                case "open-trash": Workspace.addTab("trash"); Qt.callLater(win.takeFocus); break
                case "restore-trash": if (win.onTrash) win.activeTrash.restore(); break
                case "empty-trash": if (win.onTrash) win.confirmEmptyTrash(); break
                case "save-view": Qt.callLater(win.startNaming); break
                case "views": Qt.callLater(win.focusViews); break
                case "add-graph": Workspace.addTab("graph"); break
                case "close-tab": Workspace.closeCurrentTab(); break
                case "split": Workspace.splitRight("files"); break
                case "close-pane": Workspace.closePane(Workspace.focusedPane); break
                case "undo": IndexModel.undo(); break
                case "reload": win.reloadIndex(); break
                case "home": if (win.activeFolder) win.activeFolder.goHome(); break
                case "up": if (win.activeFolder) win.activeFolder.goUp(); break
                case "path": Qt.callLater(win.startPathEdit); break
                case "filter": Qt.callLater(win.startFilter); break
                case "find": Qt.callLater(win.startFinding); break
                case "filter-menu": Qt.callLater(win.openFilterMenu); break
                case "leave-search": if (win.activeTab) win.activeTab.pattern = ""; break
                case "recent": win.openRecent(); break
                case "rename": Qt.callLater(win.startRename); break
                case "chmod": Qt.callLater(win.startModeChange); break
                case "edit-tag-add": Qt.callLater(win.startTagEdit, "add"); break
                case "edit-tag-remove": Qt.callLater(win.startTagEdit, "remove"); break
                case "edit-tag-rename": Qt.callLater(win.startTagEdit, "rename"); break
                case "new-folder": Qt.callLater(win.startMakingFolder); break
                case "copy-files": win.copyToClipboard(false); break
                case "cut-files": win.copyToClipboard(true); break
                case "paste": win.paste(); break
                case "mark-all": if (win.onTrash) win.activeTrash.model.markAll(); else if (win.activeFolder) win.activeFolder.markAll(); break
                case "clear-marks": if (win.onTrash) win.activeTrash.model.clearMarks(); else if (win.activeFolder) win.activeFolder.clearMarks(); break
                case "mark-pattern": Qt.callLater(win.startMarking); break
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
                case "make-default": IndexModel.makeDefaultFileManager(); break
                case "keys":
                    Qt.callLater(function () {
                        helpSheet.open()
                    })
                    break
                default: win.runColumnAction(id)
                }
                commandPalette.close()
            }
        }

        HelpSheet {
            id: helpSheet
            anchors.fill: parent
            groups: win.keymap
        }

        PreviewSheet {
            id: previewSheet
            anchors.fill: parent
            frontmost: win.overlayStack.length > 0 && win.overlayStack[win.overlayStack.length - 1].overlay === previewSheet
            previousAvailable: visible && win.activeFolder !== null && win.activeListing !== null
                && win.activeFolder.previewRow(win.activeListing.currentIndex, -1) >= 0
            nextAvailable: visible && win.activeFolder !== null && win.activeListing !== null
                && win.activeFolder.previewRow(win.activeListing.currentIndex, 1) >= 0
            onStepRequested: function (direction) { win.stepPreview(direction) }
        }

        ConflictSheet { id: conflictSheet; anchors.fill: parent }
        ArchiveSheet { id: archiveSheet; anchors.fill: parent }

        TagSheet { id: tagSheet; anchors.fill: parent }

        ModeSheet {
            id: modeSheet
            objectName: "modeSheet"
            anchors.fill: parent
        }

        ContextMenu {
            id: fileMenu
            objectName: "fileMenu"
            anchors.fill: parent
            property string targetPath: ""
            property string targetKind: ""

            // What a command chosen here acts on: the marked set if the row
            // under the pointer is in it, that row alone if it is not. The rule
            // is the model's rather than each entry's, so that no entry can be
            // written that forgets it -- which is how a delete of one file ends
            // up taking forty.
            readonly property var targets: win.activeFolder
                ? win.activeFolder.targetPaths(fileMenu.targetPath)
                : (fileMenu.targetPath !== "" ? [fileMenu.targetPath] : [])
            readonly property bool hasTarget: fileMenu.targetPath !== ""
            readonly property bool isFile: fileMenu.targetKind === "file"
            // `..` is a way up and not a file, so nothing acts on it.
            readonly property bool isRow: fileMenu.hasTarget
                                          && fileMenu.targetKind !== "parent"
            readonly property bool onRow: win.onFiles && fileMenu.isRow
            readonly property int markedCount: win.activeFolder ? win.activeFolder.markedCount : 0
            readonly property bool targetMarked: win.activeFolder !== null
                                                 && win.activeFolder.isMarked(fileMenu.targetPath)
            readonly property var targetTags: fileMenu.hasTarget
                ? (win.activeFolder ? win.activeFolder.tagsFor(fileMenu.targetPath)
                                    : IndexModel.tagsForPath(fileMenu.targetPath))
                : []
            readonly property string firstTag: fileMenu.targetTags.length > 0
                                               ? fileMenu.targetTags[0] : ""
            // The tag these entries are about, named in the label rather than
            // left to be guessed: an entry that says `Apply tag` and not which
            // one is an entry nobody can press twice with confidence.
            //
            // The tag being listed first, because a tag listing is a question
            // about one tag and the tab is titled with it. Otherwise the one
            // the panel's cursor is on. Both are on screen where they count,
            // and where neither is -- a window whose tag panel has not been
            // touched yet -- these entries are not drawn rather than drawn
            // pointing at a tag nobody chose.
            readonly property string focusedTag: {
                if (win.activeTab && win.activeTab.selectedTag !== "")
                    return win.activeTab.selectedTag
                // In a tags tab it is the one just walked into, which is the
                // last word of the path on screen.
                if (win.onTags && win.activeTab.facets.length > 0)
                    return win.activeTab.facets[win.activeTab.facets.length - 1]
                return win.activeTagsList
                       ? IndexModel.tagAt(win.activeTagsList.currentIndex) : ""
            }
            // The folder a `here` entry means: the folder pointed at, the
            // folder holding the file pointed at, or the one on screen. Read
            // off the target and not off the tab, so that these still mean
            // something in a tag listing, where the rows come from anywhere.
            readonly property string hereDir: fileMenu.targetKind === "dir"
                ? fileMenu.targetPath
                : (fileMenu.hasTarget
                   ? win.parentOf(fileMenu.targetPath)
                   // A tags tab is not standing anywhere, so with no row under
                   // the pointer there is no folder for a `here` to mean.
                   : (win.activeFolder && !win.onTags ? win.activeFolder.directory : ""))
            readonly property bool hasHere: fileMenu.hereDir !== ""
            // Worth offering to walk to only where it would be a move.
            readonly property bool elsewhere: win.activeFolder !== null && fileMenu.hasTarget
                && (win.activeFolder.selectedTag !== ""
                    || win.parentOf(fileMenu.targetPath) !== win.activeFolder.directory)
            readonly property string otherDir: {
                const pane = Workspace.otherPane(Workspace.focusedPane)
                return pane && pane.current ? pane.current.cwd : ""
            }

            // A label that counts the files it is about, where there is more
            // than one of them. This is what stands in for a confirmation
            // dialog: the entry says `Move 12 files to trash` before it is
            // chosen, rather than asking about it afterwards.
            function counted(one, many) {
                return fileMenu.targets.length > 1
                       ? many.replace("%1", fileMenu.targets.length) : one
            }

            actions: [
                { id: "open", label: "Open", key: "l", when: fileMenu.hasTarget },
                { id: "preview", label: "Quick preview", key: "i", when: fileMenu.isFile },
                { id: "compress", label: fileMenu.counted("Compress…", "Compress %1 entries…"), when: fileMenu.onRow && !IndexModel.operationBusy },
                { id: "extract", label: "Extract archive…", when: fileMenu.isFile && !IndexModel.operationBusy },
                { id: "open-with", label: "Open with…",
                  when: IndexModel.canOpenWith && fileMenu.isRow },
                { id: "go-to", label: "Go to containing folder",
                  when: win.onFiles && fileMenu.elsewhere },
                { id: "open-tab", label: "Open in new tab", when: fileMenu.hasHere },
                { id: "open-pane", label: "Open in other pane", when: fileMenu.hasHere },
                { id: "open-parent-tab", label: "Open parent in new tab", when: fileMenu.hasHere },
                { id: "open-graph", label: "Open in graph tab", when: fileMenu.hasTarget },
                { id: "terminal", label: "Open terminal here",
                  when: IndexModel.canOpenTerminal && fileMenu.hasHere },
                { id: "editor", label: "Open in editor",
                  when: IndexModel.canEdit && fileMenu.isFile },
                { id: "share", label: "Send with LocalSend",
                  when: IndexModel.canShare && fileMenu.hasTarget },
                { id: "script",
                  label: fileMenu.counted("Run a script over it…",
                                          "Run a script over %1 files…"),
                  when: IndexModel.hasScripts && fileMenu.isRow },

                { id: "copy-path", label: fileMenu.counted("Copy path", "Copy %1 paths"),
                  when: fileMenu.hasTarget },
                { id: "copy-name", label: "Copy name", when: fileMenu.hasTarget },
                { id: "copy-uri", label: fileMenu.counted("Copy as URI", "Copy %1 URIs"),
                  when: fileMenu.hasTarget },
                { id: "edit-tag-add", label: fileMenu.counted("Add tag…", "Add tag to %1 files…"),
                  when: fileMenu.onRow },
                { id: "edit-tag-remove", label: fileMenu.counted("Remove tag…", "Remove tag from %1 files…"),
                  when: fileMenu.onRow },
                { id: "edit-tag-rename", label: fileMenu.counted("Rename tag…", "Rename tag on %1 files…"),
                  when: fileMenu.onRow },
                { id: "copy-tags", label: "Copy tags", when: fileMenu.targetTags.length > 0 },
                { id: "copy-tag-command", label: "Copy tag command", when: fileMenu.hasTarget },
                { id: "copy-open-command", label: "Copy open command", when: fileMenu.isFile },

                { id: "mark", label: fileMenu.targetMarked ? "Unmark" : "Mark", key: "Space",
                  when: fileMenu.onRow },
                { id: "mark-all", label: "Mark all", key: "a", when: win.onFiles },
                { id: "clear-marks", label: "Clear marks", key: "A",
                  when: win.onFiles && fileMenu.markedCount > 0 },
                { id: "invert-marks", label: "Invert marks", when: win.onFiles },
                { id: "mark-same-tag", label: "Mark same tag",
                  when: fileMenu.onRow && fileMenu.targetTags.length > 0 },
                { id: "mark-same-type", label: "Mark same type", when: fileMenu.onRow },
                { id: "mark-pattern", label: "Mark by pattern", key: "*", when: win.onFiles },

                { id: "filter-tag", label: "Filter by " + fileMenu.firstTag,
                  when: win.onFiles && fileMenu.firstTag !== "" },
                // The other half of the same thought: a listing is one tag, a
                // tags tab is that tag and every one that still divides it.
                { id: "browse-tag", label: "Browse #" + fileMenu.firstTag,
                  when: fileMenu.firstTag !== "" },
                // A folder and a query are pinned by the same entry, because
                // they are the same thing saved: what is on screen, by a name.
                { id: "save-view",
                  label: win.onTags ? "Save this query as a view"
                                    : "Save this folder as a view",
                  when: win.onFiles },
                { id: "leave-tag", label: "Leave tag listing",
                  when: win.activeTab !== null && win.activeTab.selectedTag !== "" },
                { id: "find", label: "Find by name across the roots", key: "f",
                  when: win.onDirectory },
                { id: "leave-search", label: "Leave the search", key: "h",
                  when: win.onDirectory && win.activeTab.pattern !== "" },
                { id: "recent", label: "Recently opened on the desktop",
                  when: win.onFiles && !(win.onDirectory && win.activeTab.recent) },
                { id: "leave-recent", label: "Leave the recent list", key: "h",
                  when: win.onDirectory && win.activeTab.recent },
                // Not offered where the row already carries it: applying a
                // tag a file has is a write that changes nothing, and an entry
                // that does nothing reads as one that failed.
                { id: "tag-add", label: "Apply tag " + fileMenu.focusedTag,
                  when: fileMenu.hasTarget && fileMenu.focusedTag !== ""
                        && fileMenu.targetTags.indexOf(fileMenu.focusedTag) < 0 },
                { id: "tag-rm", label: "Remove tag " + fileMenu.focusedTag,
                  when: fileMenu.hasTarget && fileMenu.focusedTag !== ""
                        && fileMenu.targetTags.indexOf(fileMenu.focusedTag) >= 0 },
                { id: "tag-clear", label: fileMenu.counted("Clear tags", "Clear tags on %1 files"),
                  when: fileMenu.hasTarget },
                { id: "tag-cp", label: "Copy tags onto the marked",
                  when: fileMenu.hasTarget && !fileMenu.targetMarked
                        && fileMenu.markedCount > 0 && fileMenu.targetTags.length > 0 },
                { id: "adopt", label: "Adopt tags from disk here", when: fileMenu.hasHere },

                { id: "copy-files", label: fileMenu.counted("Copy", "Copy %1 files"),
                  key: "Ctrl+C", when: fileMenu.isRow },
                { id: "cut-files", label: fileMenu.counted("Cut", "Cut %1 files"),
                  key: "Ctrl+X", when: fileMenu.isRow },
                { id: "paste",
                  label: Clipboard.holdsFiles && Clipboard.files().length > 1
                         ? (Clipboard.cutting() ? "Move %1 files here" : "Paste %1 files")
                               .replace("%1", Clipboard.files().length)
                         : (Clipboard.cutting() ? "Move here" : "Paste"),
                  key: "Ctrl+V", when: win.canPaste },
                { id: "copy-other",
                  label: fileMenu.counted("Copy to other pane", "Copy %1 files to other pane"),
                  when: fileMenu.isRow && fileMenu.otherDir !== "" },
                { id: "move-other",
                  label: fileMenu.counted("Move to other pane", "Move %1 files to other pane"),
                  when: fileMenu.isRow && fileMenu.otherDir !== "" },
                { id: "duplicate", label: "Duplicate",
                  when: fileMenu.isFile && fileMenu.targets.length === 1 },
                // Beside the target, named the way the desktop names one; and
                // here, for what is on the clipboard, which is what a paste
                // would put here as copies.
                { id: "link-beside", label: "Make a link beside",
                  when: fileMenu.isRow && fileMenu.targets.length === 1 },
                { id: "link-here",
                  label: Clipboard.holdsFiles && Clipboard.files().length > 1
                         ? "Link %1 files here".replace("%1", Clipboard.files().length)
                         : "Link it here",
                  when: win.canPaste },
                // A set is renamed by a template rather than a name.
                { id: "rename",
                  label: fileMenu.counted("Rename", "Rename %1 files by a pattern"),
                  key: "r", when: fileMenu.isRow },
                { id: "chmod",
                  label: fileMenu.counted("Permissions…", "Permissions of %1 files…"),
                  when: fileMenu.onRow },
                { id: "new-folder", label: "New folder", key: "n",
                  when: win.standingInFolder },
                { id: "trash", label: fileMenu.counted("Move to trash", "Move %1 files to trash"),
                  when: fileMenu.isRow },

                { id: "reindex", label: "Reindex this folder", when: fileMenu.hasHere },
                { id: "prune", label: "Prune missing rows here", when: fileMenu.hasHere },
                { id: "verify", label: "Verify this tree", when: fileMenu.hasHere },
                { id: "reload", label: "Reload index", key: "F5" }
            ]

            onChosen: function (id) {
                const path = fileMenu.targetPath
                const paths = fileMenu.targets
                const folder = win.activeFolder
                const listing = win.activeListing
                switch (id) {
                case "open":
                    // Through the row where the cursor is on it, so a folder is
                    // walked into rather than handed to the desktop to open.
                    if (folder && listing && listing.currentIndex >= 0
                            && folder.pathAt(listing.currentIndex) === path)
                        folder.activateRow(listing.currentIndex)
                    else
                        IndexModel.openPath(path)
                    break
                case "preview": Qt.callLater(win.openPreview, path); break
                case "compress": Qt.callLater(win.startArchive, false, path); break
                case "extract": Qt.callLater(win.startArchive, true, path); break
                // Open the next page after this menu has closed.
                case "open-with": Qt.callLater(win.openOpenWithMenu, path); break
                case "go-to": if (folder) folder.goTo(path); break
                case "open-tab":
                    Workspace.openTabIn(Workspace.focusedPane, "files", fileMenu.hereDir)
                    break
                case "open-pane": Workspace.splitWith("files", fileMenu.hereDir); break
                case "open-parent-tab":
                    Workspace.openTabIn(Workspace.focusedPane, "files",
                                        win.parentOf(fileMenu.hereDir))
                    break
                case "open-graph": Workspace.addTab("graph"); break
                case "terminal": IndexModel.openTerminalIn(fileMenu.hereDir); break
                case "editor": IndexModel.editPath(path); break
                case "share": IndexModel.sharePath(path); break
                case "script": Qt.callLater(win.openScriptsMenu, paths, fileMenu.hereDir); break

                // One path per line, which is what a list of paths is
                // everywhere else in this program and what every editor and
                // shell will take back.
                case "copy-path": Clipboard.copy(paths.join("\n")); break
                case "copy-name": Clipboard.copy(win.nameOf(path)); break
                case "copy-uri": Clipboard.copy(win.uriLines(paths)); break
                case "copy-tags": Clipboard.copy(fileMenu.targetTags.join(" ")); break
                // Left with the tag missing on purpose: the command is copied
                // to be finished, and a tag guessed here would be a tag applied
                // by somebody who meant to type a different one.
                case "copy-tag-command":
                    Clipboard.copy("omafiles tag add --tag  -- " + win.shellWords(paths))
                    break
                case "copy-open-command": Clipboard.copy(IndexModel.openCommandFor(path)); break

                case "mark": if (folder) folder.toggleMarkAt(folder.rowForPath(path)); break
                case "mark-all": if (folder) folder.markAll(); break
                case "clear-marks": if (folder) folder.clearMarks(); break
                case "invert-marks": if (folder) folder.invertMarks(); break
                case "mark-same-tag": if (folder) folder.markSameTag(path); break
                case "mark-same-type": if (folder) folder.markSameType(path); break
                case "mark-pattern": Qt.callLater(win.startMarking); break

                case "filter-tag":
                    if (win.activeTab) win.activeTab.selectedTag = fileMenu.firstTag
                    break
                case "browse-tag": win.openTagsTab([fileMenu.firstTag]); break
                case "save-view": Qt.callLater(win.startNaming); break
                case "leave-tag": if (win.activeTab) win.activeTab.selectedTag = ""; break
                case "find": Qt.callLater(win.startFinding); break
                case "leave-search": if (win.activeTab) win.activeTab.pattern = ""; break
                case "recent": win.openRecent(); break
                case "leave-recent": if (win.activeTab) win.activeTab.recent = false; break
                case "edit-tag-add": Qt.callLater(win.startTagEdit, "add", path); break
                case "edit-tag-remove": Qt.callLater(win.startTagEdit, "remove", path); break
                case "edit-tag-rename": Qt.callLater(win.startTagEdit, "rename", path); break
                case "tag-add": IndexModel.tagAdd(paths, fileMenu.focusedTag); break
                case "tag-rm": IndexModel.tagRemove(paths, fileMenu.focusedTag); break
                case "tag-clear": IndexModel.tagClear(paths); break
                case "tag-cp":
                    IndexModel.tagCopy(path, folder ? folder.markedPaths() : [])
                    break
                case "adopt": IndexModel.adoptTagsIn(fileMenu.hereDir); break

                case "copy-files": Clipboard.copyFiles(paths, false); break
                case "cut-files": Clipboard.copyFiles(paths, true); break
                case "paste": win.paste(); break
                case "copy-other": IndexModel.copyInto(paths, fileMenu.otherDir); break
                case "move-other": IndexModel.moveInto(paths, fileMenu.otherDir); break
                case "duplicate": IndexModel.duplicate(path); break
                case "link-beside": win.linkBeside(path); break
                case "link-here": win.linkHere(); break
                // Prompts begin after the menu has closed.
                case "rename": Qt.callLater(win.startRename, path); break
                case "chmod": Qt.callLater(win.startModeChange, path); break
                case "new-folder": Qt.callLater(win.startMakingFolder); break
                case "trash": IndexModel.trash(paths); break

                case "reindex": IndexModel.reindexTree(fileMenu.hereDir); break
                case "prune": IndexModel.pruneTree(fileMenu.hereDir); break
                case "verify": IndexModel.verifyTree(fileMenu.hereDir); break
                case "reload": win.reloadIndex(); break
                }
            }
        }

        ContextMenu {
            id: tabMenu
            objectName: "tabMenu"
            anchors.fill: parent
            property int targetPaneIndex: 0
            property int targetTabIndex: 0
            property int targetTabCount: 1
            actions: [
                { id: "close", label: "Close tab", key: "Ctrl+W" },
                { id: "close-others", label: "Close others", when: tabMenu.targetTabCount > 1 },
                { id: "close-right", label: "Close tabs to the right",
                  when: tabMenu.targetTabIndex < tabMenu.targetTabCount - 1 },
                { id: "duplicate", label: "Duplicate tab" },
                { id: "move-pane", label: "Move tab to other pane",
                  when: Workspace.paneCount > 1 && tabMenu.targetTabCount > 1 },
                { id: "split", label: "Split with this tab", key: "Ctrl+\\" },
                { id: "close-pane", label: "Close pane", key: "Ctrl+Shift+W",
                  when: Workspace.paneCount > 1 }
            ]
            onChosen: function (id) {
                const pi = tabMenu.targetPaneIndex
                const ti = tabMenu.targetTabIndex
                switch (id) {
                case "close": Workspace.closeTab(pi, ti); break
                case "close-others":
                    // Away from the tab that is staying in both directions, and
                    // from the far end inwards, so no close renumbers one that
                    // is still to come.
                    for (let i = tabMenu.targetTabCount - 1; i > ti; i--)
                        Workspace.closeTab(pi, i)
                    for (let j = ti - 1; j >= 0; j--)
                        Workspace.closeTab(pi, j)
                    break
                case "close-right": Workspace.closeTabsAfter(pi, ti); break
                case "duplicate": Workspace.duplicateTab(pi, ti); break
                case "move-pane": Workspace.moveTabToOtherPane(pi, ti); break
                case "split": {
                    const tab = Workspace.tabAtIndex(pi, ti)
                    if (tab)
                        Workspace.splitWith(tab.kind, tab.cwd)
                    break
                }
                case "close-pane": Workspace.closePane(pi); break
                }
            }
        }

        ContextMenu {
            id: openWithMenu
            objectName: "openWithMenu"
            anchors.fill: parent
            property string targetPath: ""
            // Handed in rather than looked up here: the lookup asks `xdg-mime`,
            // which is two child processes, and a binding would pay for them
            // again on every change it happens to hear about.
            property var apps: []
            actions: {
                const out = []
                for (let i = 0; i < openWithMenu.apps.length; i++)
                    out.push({ id: openWithMenu.apps[i].file,
                               label: openWithMenu.apps[i].name })
                return out
            }
            // The entry file is the id: two applications can be called the same
            // thing, and the file they are launched from cannot.
            onChosen: function (id) {
                IndexModel.openWith(openWithMenu.targetPath, id)
            }
        }

        ContextMenu {
            id: scriptsMenu
            objectName: "scriptsMenu"
            anchors.fill: parent
            property var targetPaths: []
            property string targetDir: ""
            property var scripts: []
            actions: {
                const out = []
                for (let i = 0; i < scriptsMenu.scripts.length; i++)
                    out.push({ id: scriptsMenu.scripts[i].path,
                               label: scriptsMenu.scripts[i].name })
                return out
            }
            onChosen: function (id) {
                IndexModel.runScript(id, scriptsMenu.targetPaths, scriptsMenu.targetDir)
            }
        }

        ContextMenu {
            id: filterMenu
            objectName: "filterMenu"
            anchors.fill: parent
            actions: win.filterActionsFor(win.filterRow)
            onChosen: function (id) { win.narrow(id) }
        }

        ContextMenu {
            id: tagMenu
            objectName: "tagMenu"
            anchors.fill: parent
            property int targetRow: -1
            readonly property string tag: IndexModel.tagAt(targetRow)
            readonly property bool canEdit: win.activeFolder !== null && win.activeListing !== null
                && (win.activeFolder.kindAt(win.activeListing.currentIndex) === "file"
                    || win.activeFolder.kindAt(win.activeListing.currentIndex) === "dir")
            actions: [
                { id: "toggle", label: "Toggle tag filter", key: "Enter" },
                { id: "add", label: "Add tags…", key: "a", when: tagMenu.canEdit },
                { id: "remove", label: "Remove tag…", key: "d", when: tagMenu.canEdit },
                { id: "rename", label: "Rename tag…", key: "r", when: tagMenu.canEdit }
            ]
            onChosen: function (id) {
                if (id === "toggle") win.toggleTagAt(targetRow)
                else Qt.callLater(win.startTagEdit, id, "", id === "add" ? "" : tag)
            }
        }

        ContextMenu {
            id: deviceMenu
            objectName: "deviceMenu"
            anchors.fill: parent
            property string targetId: ""
            readonly property var target: Devices.entries.find(function (device) { return device.id === deviceMenu.targetId }) || ({})
            actions: [
                { id: "open", label: "Open", when: !Devices.busy && deviceMenu.target.id !== undefined },
                { id: "mount", label: "Mount", when: !Devices.busy && deviceMenu.target.canMount && !deviceMenu.target.mounted },
                { id: "unmount", label: "Unmount", when: !Devices.busy && deviceMenu.target.canUnmount },
                { id: "eject", label: "Eject safely", when: !Devices.busy && deviceMenu.target.canEject },
                { id: "cancel", label: "Cancel device operation", when: Devices.busy }
            ]
            onChosen: function (id) { Devices.act(deviceMenu.targetId, id) }
        }

        ContextMenu {
            id: viewMenu
            objectName: "viewMenu"
            anchors.fill: parent
            property int targetRow: -1
            readonly property var target: viewMenu.targetRow >= 0
                                          ? Views.at(viewMenu.targetRow) : ({})
            readonly property string targetName: viewMenu.target.name || ""
            readonly property bool isFolder: viewMenu.target.kind === "folder"

            actions: [
                { id: "open", label: "Open", key: "l", when: viewMenu.targetName !== "" },
                { id: "open-pane", label: "Open in other pane",
                  when: viewMenu.targetName !== "" },
                // What it asks, as the command line that asks it: a view is a
                // thing to read as well as to open, and this is how it gets
                // out of the window and into a script.
                { id: "copy", label: "Copy the command",
                  when: viewMenu.targetName !== "" },
                { id: "forget", label: "Forget " + viewMenu.targetName,
                  when: viewMenu.targetName !== "" }
            ]

            onChosen: function (id) {
                const view = viewMenu.target
                if (!view || !view.name)
                    return
                switch (id) {
                case "open": win.openView(viewMenu.targetRow); break
                case "open-pane":
                    win.takeFocus()
                    if (viewMenu.isFolder)
                        Workspace.splitWith("files", view.dir)
                    else {
                        Workspace.splitRight("tags")
                        if (Workspace.currentTab)
                            Workspace.currentTab.facets = view.tags
                    }
                    break
                case "copy": Clipboard.copy("omafiles " + view.asks); break
                case "forget": Views.forget(view.name); break
                }
            }
        }

        ContextMenu {
            id: columnMenu
            objectName: "columnMenu"
            anchors.fill: parent
            property var moving: []
            actions: win.columnActions(columnMenu.moving)
            onChosen: function (id) { win.runColumnAction(id) }
        }

        ContextMenu {
            id: trashMenu
            objectName: "trashMenu"
            anchors.fill: parent
            actions: win.onTrash ? [
                { id: "restore", label: "Restore row or marked entries", key: "r",
                  when: win.onTrash && win.activeTrash.model.count > 0 },
                { id: "details", label: "Original path and deletion details", key: "Enter" },
                { id: "mark", label: "Toggle mark", key: "Space" },
                { id: "mark-all", label: "Mark all", key: "a" },
                { id: "clear-marks", label: "Clear marks", key: "A" },
                { id: "empty", label: "Empty Trash (permanent)…", key: "e" }
            ] : []
            onChosen: function (id) {
                if (!win.onTrash) return
                if (id === "restore") win.activeTrash.restore()
                else if (id === "details") win.activeTrash.showDetails()
                else if (id === "mark") win.activeTrash.markCurrent()
                else if (id === "mark-all") win.activeTrash.model.markAll()
                else if (id === "clear-marks") win.activeTrash.model.clearMarks()
                else Qt.callLater(win.confirmEmptyTrash)
            }
        }
        ContextMenu {
            id: trashEmptyMenu
            objectName: "trashEmptyMenu"
            anchors.fill: parent
            actions: win.onTrash ? [
                { id: "cancel", label: "Cancel — keep the files" },
                { id: "empty", label: "Permanently delete all Trash entries" }
            ] : []
            onChosen: function (id) {
                if (id === "empty" && win.onTrash) win.activeTrash.model.empty()
            }
        }

        ContextMenu {
            id: newTabMenu
            objectName: "newTabMenu"
            anchors.fill: parent
            property int targetPaneIndex: 0
            actions: [
                { id: "files", label: "Files tab" },
                { id: "tags", label: "Tags tab" },
                { id: "graph", label: "Graph tab" },
                { id: "trash", label: "Trash" }
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
        // Qt Rectangle defaults to white; the files views paint transparent on top of this pane.
        color: Theme.background
        property var pane
        property int paneIndex
        property real panePreferredWidth: win.width / Math.max(1, Workspace.paneCount)
        C.SplitView.minimumWidth: 280

        Component.onCompleted: win.paneItems[paneIndex] = paneRoot

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
                if (win.filtering)
                    win.finishFilter(false)
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
                            id: tabHandle
                            objectName: "tabHandle"
                            required property var modelData
                            required property int index
                            title: modelData.title
                            // A tab showing a tag is not a folder, and a folder
                            // icon beside `#work` says it is. A tags tab is
                            // never a folder at all.
                            kind: modelData.kind === "tags"
                                  || (modelData.kind === "files"
                                      && modelData.selectedTag !== "")
                                  ? "tag" : modelData.kind
                            // Optional, because closing a pane leaves the
                            // delegate alive one evaluation longer than the
                            // pane: under `ComponentBehavior: Bound` the
                            // enclosing scope is kept, so this runs once more
                            // with `pane` already null.
                            current: paneRoot.pane?.currentIndex === index
                            FileDropArea {
                                anchors.fill: parent
                                directory: win.isFolderTab(tabHandle.modelData) ? tabHandle.modelData.cwd : ""
                                blocked: win.blocked
                            }
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
                                  ? graphView : paneRoot.pane.current && paneRoot.pane.current.kind === "trash"
                                  ? trashView : filesView
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
            onKeyRequested: function (event) { win.handleKey(event) }
            onFocusEntered: {
                Workspace.focusedPane = paneRoot.paneIndex
            }
            Layout.fillHeight: true
            Layout.preferredWidth: 252
            // The panel belongs to both list tabs: the views in it are how you
            // get anywhere, and a tags tab needs them as much as a folder does.
            readonly property bool holdsTags: paneRoot.pane.current
                                              && (paneRoot.pane.current.kind === "files"
                                                  || paneRoot.pane.current.kind === "tags")
            showTags: dock.holdsTags
            // Dropped whole in a pane too narrow to hold it and a list at the
            // same time -- splitting the window is asking for two lists, not
            // for two copies of the same tag vocabulary where the names should
            // be -- and back for as long as `t` is walking it, because a key
            // the sheet names without a condition has to work at every size.
            visible: dock.holdsTags && (paneRoot.width >= 640 || dock.expanded)

            tab: paneRoot.pane.current
            // A Loader's payload has no static type -- it is whichever component
            // is loaded -- so nothing can tell the linter that this one has an
            // `entry`. The check is disabled for the line rather than for the
            // file, and the runtime guard above is what actually keeps it safe.
            // qmllint disable missing-property
            entry: content.item && content.item.entry !== undefined ? content.item.entry : ({})
            // qmllint enable missing-property
            walking: win.focusRegion === "tags" && win.activeDock === dock
            walkingDevices: win.focusRegion === "devices" && win.activeDock === dock
            onDeviceActivated: function (id) { Workspace.focusedPane = paneRoot.paneIndex; Devices.act(id, "open") }
            onDeviceMenuRequested: function (id, x, y) {
                Workspace.focusedPane = paneRoot.paneIndex
                win.activeDock = dock
                win.openDeviceMenu(id, x, y)
            }
            walkingViews: win.focusRegion === "views" && win.activeDock === dock

            onTagChipActivated: function (path, tag) {
                Workspace.focusedPane = paneRoot.paneIndex
                win.startTagEdit("remove", path, tag)
            }
            onTagEditRequested: {
                Workspace.focusedPane = paneRoot.paneIndex
                win.startTagEdit("add")
            }
            onViewActivated: function (row) { win.openView(row) }
            onViewMenuRequested: function (row, x, y) {
                Workspace.focusedPane = paneRoot.paneIndex
                win.activeDock = dock
                win.openViewMenuAt(row, x, y)
            }

            onTagMenuRequested: function (row, x, y) {
                Workspace.focusedPane = paneRoot.paneIndex
                win.activeDock = dock
                win.openTagMenuAt(row, x, y)
            }
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

    function confirmEmptyTrash() {
        if (!onTrash || IndexModel.operationBusy || !activeTrash.model.prepareEmpty()) return
        trashEmptyMenu.openAt(width / 2 - 140, height / 2 - 40)
    }

    Component {
        id: trashView
        TrashView {
            id: trashRoot
            // The Loader owns the tab and pane properties.
            // qmllint disable missing-property
            property var tab: parent ? parent.tab : null
            property int paneIndex: parent ? parent.paneIndex : 0
            // qmllint enable missing-property
            function syncActive() {
                if (Workspace.currentTab === tab) win.activeTrash = trashRoot
                else if (win.activeTrash === trashRoot) win.activeTrash = null
            }
            Component.onCompleted: syncActive()
            Component.onDestruction: if (win.activeTrash === trashRoot) win.activeTrash = null
            onTabChanged: syncActive()
            Connections {
                target: Workspace
                function onCurrentTabChanged() { trashRoot.syncActive() }
                function onFocusedPaneChanged() { trashRoot.syncActive() }
            }
            onMenuRequested: function (x, y) {
                Workspace.focusedPane = paneIndex
                const pos = mapToItem(root, x, y)
                trashMenu.openAt(pos.x, pos.y)
            }
            onEmptyRequested: win.confirmEmptyTrash()
            onReceiptRequested: function (text) { win.showReceipt("Trash", text) }
            onGlobalKey: function (event) { win.handleKey(event) }
        }
    }

    // ------------------------------------------------------------ files view
    Component {
        id: filesView
        Item {
            id: filesRoot
            property bool thumbnailsReady: false
            function selectRow(row, modifiers) {
                Workspace.focusedPane = filesRoot.paneIndex
                if (modifiers & Qt.ShiftModifier)
                    folder.markRange(filesRoot.cursor, row)
                filesRoot.setCursor(row)
                win.takeFocus()
            }
            function rowMenu(row, path, kind, source, position) {
                selectRow(row, 0)
                const at = source.mapToItem(root, position.x, position.y)
                win.openFileMenuForPath(path, kind, at.x, at.y)
            }
            function deferThumbnails() {
                thumbnailsReady = false
                thumbnailDelay.restart()
            }
            Timer {
                id: thumbnailDelay
                interval: 160
                running: win.firstFrameShown && !filesRoot.thumbnailsReady
                onTriggered: if (win.firstFrameShown) filesRoot.thumbnailsReady = true
            }
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
            property alias band: band
            readonly property string viewMode: filesRoot.tab ? filesRoot.tab.viewMode : "list"
            readonly property var view: viewMode === "icons" ? grid : listing

            // The details table's geometry, in one place. A heading and the
            // cells under it that each work out their own width are a heading
            // and a column that do not line up.
            readonly property real nameIndent: 54
            readonly property real tagsWidth: Math.max(90, Math.min(260, listArea.width * 0.26))

            function columnWidth(field) {
                switch (field) {
                case "tags": return filesRoot.tagsWidth
                case "type": return 62
                case "size": return 74
                case "mtime": return 128
                case "owner": return 80
                case "mode": return 84
                }
                return 0
            }

            readonly property int cursor: view ? view.currentIndex : -1

            // What the details panel reads. Recomputed rather than bound,
            // because the row it describes can change without its index moving:
            // a rebuild keeps index 4 and puts a different file there.
            property var entry: ({})

            function syncEntry() {
                if (folder.loading) {
                    filesRoot.entry = ({})
                    return
                }
                filesRoot.entry = folder.entryAt(filesRoot.cursor)
            }

            // The one place the cursor moves. Clamped here so no caller has to
            // know the row count, and remembered here so a rebuild -- a filter
            // keystroke, a reload, a change of directory -- can put it back.
            function showCursor(row) {
                const view = filesRoot.view
                view.currentIndex = row
                view.cancelFlick()
                view.forceLayout()
                view.positionViewAtIndex(row, filesRoot.viewMode === "icons"
                                               ? GridView.Contain : ListView.Contain)
                syncEntry()
            }

            function setCursor(row) {
                if (folder.count <= 0)
                    return
                const clamped = Math.max(0, Math.min(row, folder.count - 1))
                showCursor(clamped)
                folder.rememberCursor(clamped)
            }

            function restoreCursor() {
                if (folder.loading)
                    return
                const row = folder.cursorRow()
                if (row < 0) {
                    filesRoot.entry = ({})
                    return
                }
                showCursor(row)
            }

            function revealCursor() {
                if (view && view.currentIndex >= 0)
                    showCursor(view.currentIndex)
            }
            onHeightChanged: Qt.callLater(revealCursor)
            onWidthChanged: Qt.callLater(revealCursor)
            onViewModeChanged: { deferThumbnails(); Qt.callLater(restoreCursor) }

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
                filter: filesRoot.tab ? filesRoot.tab.filter : ""
                criteria: filesRoot.tab ? filesRoot.tab.criteria : ({})
                selectedTag: filesRoot.tab ? filesRoot.tab.selectedTag : ""
                facets: filesRoot.tab ? filesRoot.tab.facets : []
                untagged: filesRoot.tab ? filesRoot.tab.untagged : false
                pattern: filesRoot.tab ? filesRoot.tab.pattern : ""
                recent: filesRoot.tab ? filesRoot.tab.recent : false
                showHidden: filesRoot.tab ? filesRoot.tab.showHidden : false
                sortRole: filesRoot.tab ? filesRoot.tab.sortRole : "name"
                sortAscending: filesRoot.tab ? filesRoot.tab.sortAscending : true
                // Walking into a folder and going home both leave a tag
                // listing, so the tag travels back to the tab.
                onSelectedTagChanged: if (filesRoot.tab) filesRoot.tab.selectedTag = selectedTag
                // `h` out of a search ends it in the model, and the tab has
                // to hear so its title and path bar follow.
                onPatternChanged: if (filesRoot.tab) filesRoot.tab.pattern = pattern
                onRecentChanged: if (filesRoot.tab) filesRoot.tab.recent = recent
                onChanged: {
                    if (loading)
                        filesRoot.syncEntry()
                    Qt.callLater(filesRoot.restoreCursor)
                }
                // The panel reads one row, and marking does not move the
                // cursor: without this the `marked` line only catches up when
                // the cursor next moves off the row and back.
                onMarkedChanged: filesRoot.syncEntry()
                onMetadataChanged: filesRoot.syncEntry()
            }

            Item {
                id: listArea
                anchors.fill: parent

                    FileDropArea {
                        anchors.fill: parent
                        directory: win.isFolderTab(filesRoot.tab) ? folder.directory : ""
                        blocked: win.blocked
                    }

                    ListView {
                        id: listing
                        reuseItems: !folder.loading
                        cacheBuffer: 0
                        onContentYChanged: filesRoot.deferThumbnails()
                        anchors.fill: parent
                        visible: filesRoot.viewMode !== "icons"
                        clip: true
                        model: visible ? folder : null
                        boundsBehavior: Flickable.StopAtBounds
                        highlightResizeDuration: 0
                        readonly property int rowHeight: 30
                        currentIndex: 0
                        keyNavigationEnabled: false
                        highlightMoveDuration: 0
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
                                id: nameHead
                                objectName: "columnHead"
                                anchors.left: parent.left
                                anchors.leftMargin: filesRoot.nameIndent
                                anchors.right: headColumns.left
                                anchors.rightMargin: 10
                                height: parent.height
                                label: "name"
                                role: "name"
                                sortRole: filesRoot.tab ? filesRoot.tab.sortRole : ""
                                ascending: filesRoot.tab && filesRoot.tab.sortAscending
                                onPicked: filesRoot.tab.pickSortRole("name")
                                // Over the name, the menu moves every column.
                                onMenuAsked: function (x, y) {
                                    const pos = nameHead.mapToItem(root, x, y)
                                    win.openColumnMenu(Workspace.columns, pos.x, pos.y)
                                }
                            }
                            Row {
                                id: headColumns
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                height: parent.height
                                spacing: 10

                                Repeater {
                                    model: Workspace.columns
                                    ColumnHead {
                                        id: columnHead
                                        required property string modelData
                                        objectName: "columnHead"
                                        height: headColumns.height
                                        width: filesRoot.columnWidth(columnHead.modelData)
                                        label: win.columnLabel(columnHead.modelData)
                                        // A field the tab cannot sort by is a heading
                                        // and nothing more.
                                        role: filesRoot.tab && filesRoot.tab.validSortRole(
                                                  columnHead.modelData) === columnHead.modelData
                                              ? columnHead.modelData : ""
                                        sortRole: filesRoot.tab ? filesRoot.tab.sortRole : ""
                                        ascending: filesRoot.tab && filesRoot.tab.sortAscending
                                        onPicked: filesRoot.tab.pickSortRole(columnHead.modelData)
                                        onMenuAsked: function (x, y) {
                                            const pos = columnHead.mapToItem(root, x, y)
                                            win.openColumnMenu([columnHead.modelData], pos.x, pos.y)
                                        }
                                    }
                                }
                            }

                            Rule { anchors.bottom: parent.bottom }
                        }

                        delegate: Rectangle {
                            id: row
                            objectName: "fileRow"
                            property bool pooled: false
                            ListView.onPooled: row.pooled = true
                            ListView.onReused: row.pooled = false
                            required property string path
                            required property string name
                            required property string kind
                            required property string type
                            required property string tags
                            required property bool marked
                            required property string sizeText
                            required property string mtimeText
                            required property string owner
                            required property string mode
                            required property int count
                            required property int index
                            /// A facet row: a tag standing in for a folder. It
                            /// has no path, so nothing that acts on a file can
                            /// reach it, and what it shows on the right is how
                            /// many files it leads to rather than its own tags.
                            readonly property bool facet: row.kind === "tag"
                                                          || row.kind === "untagged"
                            readonly property string tally: row.count >= 0
                                                            ? row.count.toString() : ""
                            readonly property bool details: filesRoot.viewMode === "details"
                            readonly property var tagList: row.tags.length === 0
                                                           ? [] : row.tags.split(" ")
                            readonly property bool onCursor: listing.currentIndex === index

                            width: ListView.view.width
                            height: listing.rowHeight
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

                            FileIcon {
                                id: rowIcon
                                folder: folder
                                rowIndex: row.index
                                path: row.path
                                allowed: filesRoot.thumbnailsReady && !previewSheet.visible
                                inViewport: filesRoot.view === listing && !row.pooled && row.visible
                                    && row.y + row.height > listing.contentY
                                    && row.y < listing.contentY + listing.height
                                moving: listing.moving
                                anchors.left: markDot.right
                                anchors.leftMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                kind: row.facet ? "tag"
                                    : (row.kind === "file" ? "file"
                                    : (row.kind === "parent" ? "parent" : "folder"))
                                ink: row.kind === "file" ? Theme.dim : Theme.accent
                            }

                            // Anchored between the icon and whatever is on the
                            // right, rather than laid out in a Row: in a Row the
                            // name takes a width guessed from the row width and
                            // the tags run off the end of it.
                            Label {
                                id: rowName
                                anchors.left: rowIcon.right
                                anchors.leftMargin: 8
                                anchors.right: row.details ? cells.left : tagArea.left
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: row.name
                                opacity: row.onCursor ? 1 : 0.88
                            }

                            // The same columns as the headings, at the same widths,
                            // each packed from the left where its heading starts.
                            Row {
                                id: cells
                                visible: row.details
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 10

                                Repeater {
                                    model: row.details ? Workspace.columns : []
                                    Loader {
                                        id: columnCell
                                        objectName: "columnCell"
                                        required property string modelData
                                        anchors.verticalCenter: parent ? parent.verticalCenter
                                                                       : undefined
                                        width: filesRoot.columnWidth(columnCell.modelData)
                                        sourceComponent: columnCell.modelData === "tags"
                                                         ? tagCell : textCell

                                        Component {
                                            id: tagCell
                                            TagStrip {
                                                limit: filesRoot.tagsWidth
                                                alignRight: false
                                                tags: row.tagList
                                                onActivated: function (tag) {
                                                    Workspace.focusedPane = filesRoot.paneIndex
                                                    Qt.callLater(win.startTagEdit, "remove", row.path, tag)
                                                }
                                            }
                                        }
                                        Component {
                                            id: textCell
                                            Label {
                                                quiet: true
                                                font.pixelSize: 11
                                                text: {
                                                    switch (columnCell.modelData) {
                                                    case "type": return row.type
                                                    // A tag has no size but it does have a
                                                    // count, and this is the column that
                                                    // answers "how much".
                                                    case "size": return row.facet ? row.tally
                                                                                  : row.sizeText
                                                    case "mtime": return row.mtimeText
                                                    case "owner": return row.owner
                                                    case "mode": return row.mode
                                                    }
                                                    return ""
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // Floats at the end of the row rather than lining up
                            // in a column: the list view has no columns to line
                            // up with.
                            Label {
                                id: facetCount
                                visible: !row.details && row.facet
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                quiet: true
                                font.pixelSize: 11
                                text: row.tally
                            }

                            TagStrip {
                                id: tagArea
                                visible: !row.details && !row.facet
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                limit: row.width * 0.4
                                tags: row.tagList
                                onActivated: function (tag) {
                                    Workspace.focusedPane = filesRoot.paneIndex
                                    Qt.callLater(win.startTagEdit, "remove", row.path, tag)
                                }
                            }

                            // The icon and the name, where a drag is the row's
                            // and not the rubber band's.
                            FileDragGrip {
                                folder: filesRoot.folder
                                path: row.path
                                enabled: row.kind === "file" || row.kind === "dir"
                                onStarted: {
                                    Workspace.focusedPane = filesRoot.paneIndex
                                    filesRoot.setCursor(row.index)
                                    win.takeFocus()
                                }
                                x: rowIcon.x
                                width: rowName.x + Math.min(rowName.contentWidth, rowName.width)
                                       - rowIcon.x
                                height: row.height
                            }

                            FileDropArea {
                                anchors.fill: parent
                                directory: row.kind === "dir" || row.kind === "parent" ? row.path : ""
                                blocked: win.blocked
                            }

                            HoverHandler { id: rowHover }

                            RowTapHandler {
                                onClicked: function (position, modifiers) {
                                    filesRoot.selectRow(row.index, modifiers)
                                }
                                onContextRequested: function (position) {
                                    filesRoot.rowMenu(row.index, row.path, row.kind, row, position)
                                }
                                onActivated: {
                                    filesRoot.selectRow(row.index, 0)
                                    folder.activateRow(row.index)
                                }
                            }
                        }
                    }

                    GridView {
                        id: grid
                        reuseItems: !folder.loading
                        cacheBuffer: 0
                        onContentYChanged: filesRoot.deferThumbnails()
                        anchors.fill: parent
                        visible: filesRoot.viewMode === "icons"
                        clip: true
                        model: visible ? folder : null
                        cellWidth: 112
                        cellHeight: 92
                        boundsBehavior: Flickable.StopAtBounds
                        currentIndex: 0
                        keyNavigationEnabled: false
                        highlightMoveDuration: 0
                        Accessible.role: Accessible.List
                        Accessible.name: "Files"
                        onCurrentIndexChanged: filesRoot.syncEntry()

                        delegate: Item {
                            id: cell
                            objectName: "fileRow"
                            property bool pooled: false
                            GridView.onPooled: cell.pooled = true
                            GridView.onReused: cell.pooled = false
                            required property string path
                            required property string name
                            required property string kind
                            required property bool marked
                            required property int count
                            required property int index
                            readonly property bool onCursor: grid.currentIndex === index

                            Accessible.role: Accessible.ListItem
                            Accessible.name: cell.name
                            width: grid.cellWidth
                            height: grid.cellHeight

                            FileDragGrip {
                                folder: filesRoot.folder
                                path: cell.path
                                enabled: cell.kind === "file" || cell.kind === "dir"
                                onStarted: {
                                    Workspace.focusedPane = filesRoot.paneIndex
                                    filesRoot.setCursor(cell.index)
                                    win.takeFocus()
                                }
                                readonly property real span: Math.max(
                                    cellIcon.width,
                                    Math.min(cellName.contentWidth, cellName.width))
                                x: (cell.width - span) / 2
                                y: tile.y + tileColumn.y
                                width: span
                                height: cellName.y + cellName.height
                            }

                            Rectangle {
                                id: tile
                                anchors.fill: parent
                                anchors.margins: 4
                                radius: Theme.rounding
                                color: cell.onCursor ? Theme.fill(Theme.accent, 0.16)
                                     : cellHover.hovered ? Theme.fill(Theme.foreground, 0.05)
                                                         : "transparent"
                                border.width: 1
                                border.color: cell.onCursor ? Theme.accent : "transparent"

                                Column {
                                    id: tileColumn
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 6

                                    FileIcon {
                                        id: cellIcon
                                        folder: folder
                                        rowIndex: cell.index
                                        path: cell.path
                                        extent: 256
                                        allowed: filesRoot.thumbnailsReady && !previewSheet.visible
                                        inViewport: filesRoot.view === grid && !cell.pooled && cell.visible
                                            && cell.y + cell.height > grid.contentY
                                            && cell.y < grid.contentY + grid.height
                                        moving: grid.moving
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        width: 30
                                        height: 30
                                        kind: cell.kind === "tag"
                                              || cell.kind === "untagged" ? "tag"
                                            : (cell.kind === "file" ? "file"
                                            : (cell.kind === "parent" ? "parent" : "folder"))
                                        ink: cell.kind === "file" ? Theme.dim : Theme.accent
                                    }
                                    Label {
                                        id: cellName
                                        width: parent.width
                                        text: (cell.marked ? "\u2022 " : "") + cell.name
                                        font.pixelSize: 11
                                        opacity: cell.onCursor ? 1 : 0.88
                                        horizontalAlignment: Text.AlignHCenter
                                    }
                                }

                                FileDropArea {
                                    anchors.fill: parent
                                    directory: cell.kind === "dir" || cell.kind === "parent" ? cell.path : ""
                                    blocked: win.blocked
                                }

                                HoverHandler { id: cellHover }

                                RowTapHandler {
                                    onClicked: function (position, modifiers) {
                                        filesRoot.selectRow(cell.index, modifiers)
                                    }
                                    onContextRequested: function (position) {
                                        filesRoot.rowMenu(cell.index, cell.path, cell.kind, tile, position)
                                    }
                                    onActivated: {
                                        filesRoot.selectRow(cell.index, 0)
                                        folder.activateRow(cell.index)
                                    }
                                }
                            }
                        }
                    }

                    ImmediateWheel {
                        anchors.fill: parent
                        view: filesRoot.view
                        step: filesRoot.viewMode === "icons" ? grid.cellHeight : listing.rowHeight
                    }

                    RubberBand {
                        id: band
                        anchors.fill: parent
                        view: filesRoot.view
                        folder: folder
                        cellWidth: filesRoot.view === grid ? grid.cellWidth : listing.width
                        cellHeight: filesRoot.view === grid ? grid.cellHeight : listing.rowHeight
                        columns: filesRoot.view === grid
                                 ? Math.max(1, Math.floor(grid.width / grid.cellWidth)) : 1
                        // The pane's catch-all has focused the pane already.
                        onStarted: win.takeFocus()
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
                    // A tag on the graph opens as a tags tab and not as a
                    // filtered folder: the graph is a map of which tags go
                    // together, and the tab is the same fact walked into.
                    win.openTagsTab([label])
                }
                onNodeMenuRequested: function (id, kind, path, x, y) {
                    if (kind !== "file" || !path)
                        return
                    const pos = mapToItem(root, x, y)
                    win.openFileMenuForPath(path, "file", pos.x, pos.y)
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
