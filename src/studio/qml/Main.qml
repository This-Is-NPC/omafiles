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
    property string focusRegion: "list"
    property bool pendingG: false
    property var paneItems: ({})
    /// Where the last menu was opened, so a second page of one lands on it.
    property real menuX: 0
    property real menuY: 0


    readonly property var activeTab: Workspace.currentTab
    readonly property var activeFolder: activeFilesRoot ? activeFilesRoot.folder : null
    readonly property var activeListing: activeFilesRoot ? activeFilesRoot.view : null
    readonly property var activeTagsList: activeDock ? activeDock.tagsList : null
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
    readonly property bool onTags: onFiles && activeTab.kind === "tags"
    readonly property bool onGraph: activeTab !== null && activeTab !== undefined
                                    && activeTab.kind === "graph" && activeGraph !== null
    readonly property bool blocked: commandPalette.visible || helpSheet.visible
                                    || fileMenu.visible || tabMenu.visible
                                    || openWithMenu.visible || scriptsMenu.visible
                                    || viewMenu.visible
                                    || newTabMenu.visible || editingPath || filtering
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
            { key: "r", label: "rename" },
            { key: "F2", label: "rename" },
            { key: "n", label: "new folder" },
            { key: "Ctrl Shift n", label: "new folder" },
            { key: "Ctrl c", label: "copy the row or the marked" },
            { key: "Ctrl x", label: "cut the row or the marked" },
            { key: "Ctrl v", label: "paste into this folder" } ] },
        { title: "find", keys: [
            { key: "/", label: "filter this folder" },
            { key: "f", label: "find by name across the roots" },
            { key: "Enter", label: "keep filter" },
            { key: "Esc", label: "clear filter" },
            { key: "t", label: "tag panel" },
            { key: "v", label: "saved views" },
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
        if (prompt !== "")
            return [{ key: "Esc", label: "cancel" },
                    { key: "Enter", label: renaming || renamingSet ? "rename"
                                         : makingFolder ? "make"
                                         : matching ? "mark"
                                         : finding ? "find" : "save" }]
        if (onGraph)
            return [{ key: "?", label: "keys" },
                    { key: ":", label: "cmd" },
                    { key: "0", label: "reset view" },
                    { key: "+ - =", label: "zoom" },
                    { key: "m", label: "menu" },
                    { key: "l", label: "open" },
                    { key: "j k", label: "node" }]
        if (focusRegion === "views")
            return [{ key: "?", label: "keys" },
                    { key: "Esc", label: "back" },
                    { key: "m", label: "menu" },
                    { key: "j k", label: "move" },
                    { key: "Enter", label: "open" }]
        if (focusRegion === "tags")
            return [{ key: "?", label: "keys" },
                    { key: "Esc", label: "back" },
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

    // Saving what is on screen: the folder, or the tag query. The name is the
    // only thing that cannot be read off the window, so it is the only thing
    // asked for.
    function startNaming() {
        if (!onFiles)
            return
        prompt = "view"
        focusRegion = "filter"
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
        focusRegion = "filter"
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
    readonly property bool canPaste: onDirectory && activeTab.selectedTag === ""
                                     && activeTab.pattern === "" && Clipboard.holdsFiles

    function paste() {
        const folder = activeFolder
        if (!canPaste || !folder)
            return false
        const paths = Clipboard.files()
        const dir = folder.directory
        // One file lands under the cursor; a set lands where it lands.
        if (paths.length === 1)
            folder.pointCursorAt((dir === "/" ? "" : dir) + "/" + nameOf(paths[0]))
        if (Clipboard.cutting()) {
            IndexModel.moveInto(paths, dir)
            Clipboard.clear()
        } else {
            IndexModel.copyInto(paths, dir)
        }
        return true
    }

    // A shell pattern over every name under the declared roots, answered out
    // of the lists `sweep` writes: `ls --name <glob> --cached`, as a listing.
    // The tab stays where it was, and `h` brings it back there.
    function startFinding() {
        if (!onDirectory || !activeFolder)
            return
        prompt = "find"
        focusRegion = "filter"
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
        activeTab.pattern = pattern
    }

    // A shell pattern over the names on screen, added to the marks. The count
    // it made is on the status bar already, so the prompt has no receipt.
    function startMarking() {
        if (!onFiles || !activeFolder)
            return
        prompt = "match"
        focusRegion = "filter"
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
        focusRegion = "filter"
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

    // Only where the list is a folder: a tag listing spans the index and has
    // nowhere to put one.
    function startMakingFolder() {
        if (!onDirectory || !activeFolder || activeTab.selectedTag !== ""
                || activeTab.pattern !== "")
            return
        prompt = "folder"
        focusRegion = "filter"
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
        focusRegion = "path"
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
                }
                activeFolder.directory = text
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

    // Focused even with nothing in it, so `v` shows where views live rather
    // than doing nothing at all -- which is the same key answering the same
    // question either way.
    function focusViews() {
        if (!activeViewsList)
            return
        focusRegion = "views"
        // The focus as well as the region. `t` and `v` are pressed while the
        // window already holds it, so setting the region alone was enough --
        // but this is also what the view menu returns to when it closes, and
        // there the focus is on a scope that has just gone invisible. Without
        // this the keyboard went dead after using that menu once.
        root.forceActiveFocus()
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
        fileMenu.returnFocus = takeFocus
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
        openWithMenu.returnFocus = takeFocus
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
        scriptsMenu.returnFocus = takeFocus
        scriptsMenu.openAt(menuX, menuY)
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
        focusRegion = "views"
        if (activeViewsList)
            activeViewsList.currentIndex = row
        viewMenu.targetRow = row
        viewMenu.returnFocus = win.focusViews
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
        if (focusRegion === "views")
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
        if (focusRegion === "views") {
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
            case Qt.Key_N:
                if (shift)
                    startMakingFolder()
                return shift
            case Qt.Key_C: return copyToClipboard(false)
            case Qt.Key_X: return copyToClipboard(true)
            case Qt.Key_V: return paste()
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

    function handleViewKey(event, text) {
        const views = activeViewsList
        if (!views)
            return false
        if (text === "h" || event.key === Qt.Key_Left) {
            takeFocus()
            return true
        }
        if (handleMoveKey(event, text))
            return true
        if (handleEndKey(event, text, true))
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

        const handled = focusRegion === "views" ? handleViewKey(event, text)
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
                        C.SplitView.preferredWidth: panePreferredWidth
                    }
                }
            }

            // -------------------------------------------------------- filter
            Item {
                id: filterBar
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
                pastable: Clipboard.holdsFiles ? Clipboard.files().length : 0
                cutting: Clipboard.holdsFiles && Clipboard.cutting()
                tag: win.activeTab ? win.activeTab.selectedTag : ""
                filter: win.activeTab ? win.activeTab.filter : ""
                // The folder first: it is about the pane in front of you and
                // it goes away when you walk out of it, where an index error
                // stays until the index is fixed and will be there when you do.
                // The receipt first. It answers something somebody just asked
                // and it is gone as soon as they ask the next thing; the two
                // errors under it describe a condition that will still be there
                // to read afterwards.
                message: Views.error !== ""
                         ? Views.error
                         : IndexModel.notice !== ""
                         ? IndexModel.notice
                         : (win.activeFolder && win.activeFolder.error !== ""
                            ? win.activeFolder.error : IndexModel.error)
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
                { id: "add-tags", label: "New tags tab", key: "" },
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
                { id: "leave-search", label: "Leave the search", key: "h" },
                { id: "rename", label: "Rename", key: "r" },
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
                { id: "keys", label: "Show keys", key: "?" }
            ]
            onChosen: function (id) {
                switch (id) {
                case "add-files": Workspace.addTab("files"); break
                case "add-tags": Workspace.addTab("tags"); break
                case "save-view": Qt.callLater(win.startNaming); break
                case "views": Qt.callLater(win.focusViews); break
                case "add-graph": Workspace.addTab("graph"); break
                case "close-tab": Workspace.closeCurrentTab(); break
                case "split": Workspace.splitRight("files"); break
                case "close-pane": Workspace.closePane(Workspace.focusedPane); break
                case "reload": IndexModel.reload(); break
                case "home": if (win.activeFolder) win.activeFolder.goHome(); break
                case "up": if (win.activeFolder) win.activeFolder.goUp(); break
                case "path": Qt.callLater(win.startPathEdit); break
                case "filter": Qt.callLater(win.startFilter); break
                case "find": Qt.callLater(win.startFinding); break
                case "leave-search": if (win.activeTab) win.activeTab.pattern = ""; break
                case "rename": Qt.callLater(win.startRename); break
                case "new-folder": Qt.callLater(win.startMakingFolder); break
                case "copy-files": win.copyToClipboard(false); break
                case "cut-files": win.copyToClipboard(true); break
                case "paste": win.paste(); break
                case "mark-all": if (win.activeFolder) win.activeFolder.markAll(); break
                case "clear-marks": if (win.activeFolder) win.activeFolder.clearMarks(); break
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
                // A set is renamed by a template rather than a name.
                { id: "rename",
                  label: fileMenu.counted("Rename", "Rename %1 files by a pattern"),
                  key: "r", when: fileMenu.isRow },
                { id: "new-folder", label: "New folder", key: "n",
                  when: win.onDirectory && win.activeTab.selectedTag === ""
                        && win.activeTab.pattern === "" },
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
                // On the next tick, because this menu is still closing: the
                // close that follows this handler puts the focus back on the
                // list, and it would take it off the menu opened here.
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
                // On the next tick, like `open-with`: the menu is still closing
                // and its close would take the focus off the prompt opened here.
                case "rename": Qt.callLater(win.startRename, path); break
                case "new-folder": Qt.callLater(win.startMakingFolder); break
                case "trash": IndexModel.trash(paths); break

                case "reindex": IndexModel.reindexTree(fileMenu.hereDir); break
                case "prune": IndexModel.pruneTree(fileMenu.hereDir); break
                case "verify": IndexModel.verifyTree(fileMenu.hereDir); break
                case "reload": IndexModel.reload(); break
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
            id: newTabMenu
            anchors.fill: parent
            property int targetPaneIndex: 0
            actions: [
                { id: "files", label: "Files tab" },
                { id: "tags", label: "Tags tab" },
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
            // The panel belongs to both list tabs: the views in it are how you
            // get anywhere, and a tags tab needs them as much as a folder does.
            readonly property bool holdsTags: paneRoot.pane.current
                                              && (paneRoot.pane.current.kind === "files"
                                                  || paneRoot.pane.current.kind === "tags")
            showTags: paneRoot.pane.current
                      && paneRoot.pane.current.kind === "files"
            // Dropped whole in a pane too narrow to hold it and a list at the
            // same time -- splitting the window is asking for two lists, not
            // for two copies of the same tag vocabulary where the names should
            // be -- and back for as long as `t` is walking it, because a key
            // the sheet names without a condition has to work at every size.
            visible: dock.holdsTags && (paneRoot.width >= 640 || dock.walking
                                        || dock.walkingViews)

            tab: paneRoot.pane.current
            // A Loader's payload has no static type -- it is whichever component
            // is loaded -- so nothing can tell the linter that this one has an
            // `entry`. The check is disabled for the line rather than for the
            // file, and the runtime guard above is what actually keeps it safe.
            // qmllint disable missing-property
            entry: content.item && content.item.entry !== undefined ? content.item.entry : ({})
            // qmllint enable missing-property
            walking: win.focusRegion === "tags" && win.activeDock === dock
            walkingViews: win.focusRegion === "views" && win.activeDock === dock

            onViewActivated: function (row) { win.openView(row) }
            onViewMenuRequested: function (row, x, y) {
                Workspace.focusedPane = paneRoot.paneIndex
                win.activeDock = dock
                win.openViewMenuAt(row, x, y)
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
                facets: filesRoot.tab ? filesRoot.tab.facets : []
                untagged: filesRoot.tab ? filesRoot.tab.untagged : false
                pattern: filesRoot.tab ? filesRoot.tab.pattern : ""
                showHidden: filesRoot.tab ? filesRoot.tab.showHidden : false
                sortRole: filesRoot.tab ? filesRoot.tab.sortRole : "name"
                sortAscending: filesRoot.tab ? filesRoot.tab.sortAscending : true
                onDirectoryChanged: if (filesRoot.tab) filesRoot.tab.cwd = directory
                // Walking into a folder and going home both leave a tag
                // listing, so the tag travels back to the tab the way the
                // directory does.
                onSelectedTagChanged: if (filesRoot.tab) filesRoot.tab.selectedTag = selectedTag
                // `h` out of a search ends it in the model, and the tab has
                // to hear so its title and path bar follow.
                onPatternChanged: if (filesRoot.tab) filesRoot.tab.pattern = pattern
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
                                // A tag has no size but it does have a count,
                                // and this is the column that answers "how
                                // much".
                                text: row.facet ? row.tally : row.sizeText
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
                            }

                            HoverHandler { id: rowHover }

                            TapHandler {
                                id: rowTap
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onTapped: function (point, button) {
                                    Workspace.focusedPane = filesRoot.paneIndex
                                    // Shift marks from the cursor to here
                                    // before the cursor moves here.
                                    if (button === Qt.LeftButton
                                            && (rowTap.point.modifiers & Qt.ShiftModifier))
                                        folder.markRange(listing.currentIndex, row.index)
                                    filesRoot.setCursor(row.index)
                                    win.takeFocus()
                                    if (button === Qt.RightButton) {
                                        const pos = row.mapToItem(root, point.position.x,
                                                                  point.position.y)
                                        win.openFileMenuForPath(row.path, row.kind, pos.x, pos.y)
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
                            required property int count
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
                                        kind: cell.kind === "tag"
                                              || cell.kind === "untagged" ? "tag"
                                            : (cell.kind === "file" ? "file"
                                            : (cell.kind === "parent" ? "parent" : "folder"))
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
                                    id: cellTap
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onTapped: function (point, button) {
                                        Workspace.focusedPane = filesRoot.paneIndex
                                        if (button === Qt.LeftButton
                                                && (cellTap.point.modifiers & Qt.ShiftModifier))
                                            folder.markRange(grid.currentIndex, cell.index)
                                        filesRoot.setCursor(cell.index)
                                        win.takeFocus()
                                        if (button === Qt.RightButton) {
                                            const pos = cell.mapToItem(root, point.position.x,
                                                                       point.position.y)
                                            win.openFileMenuForPath(cell.path, cell.kind, pos.x, pos.y)
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
