pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic as C
import omafiles

Window {
    id: chooser
    objectName: "fileChooser"
    readonly property var request: Portal.current
    readonly property bool saving: request && request.kind === "save"
    readonly property bool folderOnly: request && (request.directory || request.kind === "savefiles")
    readonly property bool confirming: request && request.overwritePaths.length > 0
    property bool helpVisible: false
    readonly property FolderModel folderModel: folder
    readonly property ListView fileView: files
    width: 760
    height: 560
    minimumWidth: 240
    minimumHeight: 180
    color: Theme.background
    title: request ? request.title : "File chooser"
    visible: request !== null

    Shortcut {
        sequence: "Tab"
        enabled: chooser.visible
        onActivated: chooser.cycle(false)
    }
    Shortcut {
        sequences: ["Shift+Tab", "Backtab"]
        enabled: chooser.visible
        onActivated: chooser.cycle(true)
    }

    function focusStops() {
        if (request && request.overwritePaths.length > 0)
            return [keepEditing, replaceFile];
        const stops = saving ? [filename.focusTarget] : [files];
        stops.push(up, home, hidden, help, location.focusTarget, search.focusTarget);
        if (saving)
            stops.push(files);
        if (request && request.filters.length > 0)
            stops.push(filter);
        for (let i = 0; i < choiceRows.count; ++i) {
            const row = choiceRows.itemAt(i) as ChoiceRow;
            if (row)
                stops.push(row.control);
        }
        if (request && request.multiple)
            stops.push(mark);
        stops.push(cancel, accept);
        return stops.filter(item => item.visible && item.enabled);
    }
    function cycle(reverse) {
        const stops = focusStops();
        if (stops.length === 0)
            return;
        if (filter.popup.visible) {
            filter.popup.close();
            filter.forceActiveFocus();
        }
        for (let i = 0; i < choiceRows.count; ++i) {
            const row = choiceRows.itemAt(i) as ChoiceRow;
            if (row && row.closePopup())
                row.control.forceActiveFocus();
        }
        let focused = activeFocusItem;
        let at = stops.indexOf(focused);
        while (at < 0 && focused) {
            focused = focused.parent;
            at = stops.indexOf(focused);
        }
        stops[(at + (reverse ? -1 : 1) + stops.length) % stops.length].forceActiveFocus(reverse ? Qt.BacktabFocusReason : Qt.TabFocusReason);
    }
    function startRequest() {
        helpVisible = false;
        search.text = "";
        folder.filter = "";
        folder.showHidden = false;
        folder.clearMarks();
        if (!request)
            return;
        Workspace.currentTab.cwd = request.currentFolder;
        filename.text = request.currentName;
        if (request.currentFile !== "")
            folder.pointCursorAt(request.currentFile);
        request.attachParent(chooser);
        Qt.callLater(function () {
            if (!chooser.request)
                return;
            if (chooser.saving)
                filename.focusEntry();
            else
                files.forceActiveFocus();
        });
    }
    function chooseRow(row, toggle) {
        if (row < 0 || row >= folder.count)
            return;
        files.currentIndex = row;
        folder.rememberCursor(row);
        if (saving && folder.kindAt(row) === "file")
            filename.text = folder.entryAt(row).name;
        if (toggle && request && request.multiple && (folderOnly ? folder.kindAt(row) === "dir" : folder.kindAt(row) === "file"))
            folder.toggleMarkAt(row);
        files.forceActiveFocus();
    }
    function enterRow(row) {
        if (row < 0 || row >= folder.count)
            return;
        const kind = folder.kindAt(row);
        if (kind === "parent")
            folder.goUp();
        else if (kind === "dir")
            Workspace.currentTab.cwd = folder.pathAt(row);
        else {
            chooseRow(row, false);
            submit();
        }
    }
    function submit() {
        if (!request || request.busy)
            return;
        if (saving) {
            const name = filename.text;
            request.finish([name.startsWith("/") ? name : folder.directory + "/" + name]);
        } else if (folder.markedCount > 0 && request.multiple) {
            request.finish(folder.markedPaths());
        } else if (folderOnly) {
            const row = files.currentIndex;
            request.finish([row >= 0 && folder.kindAt(row) === "dir" ? folder.pathAt(row) : folder.directory]);
        } else {
            const row = files.currentIndex;
            if (row >= 0 && folder.kindAt(row) !== "file")
                enterRow(row);
            else
                request.finish(row >= 0 ? [folder.pathAt(row)] : []);
        }
    }
    onRequestChanged: startRequest()
    onClosing: function (event) {
        event.accepted = false;
        if (request)
            request.cancel();
    }

    FolderModel {
        id: folder
        tab: Workspace.currentTab
        onDirectoryChanged: {
            location.text = directory;
        }
        onChanged: {
            files.currentIndex = Math.max(0, Math.min(folder.cursorRow(), folder.count - 1));
            files.positionViewAtIndex(files.currentIndex, ListView.Contain);
        }
    }
    onConfirmingChanged: Qt.callLater(function () {
        if (chooser.confirming)
            keepEditing.forceActiveFocus();
        else if (chooser.request && chooser.saving)
            filename.focusEntry();
    })
    FocusScope {
        id: surface
        anchors.fill: parent
        focus: true
        Keys.onPressed: function (event) {
            if (!chooser.request)
                return;
            if (event.key === Qt.Key_Escape) {
                if (chooser.request.overwritePaths.length > 0)
                    chooser.request.confirmOverwrite(false);
                else if (chooser.helpVisible)
                    chooser.helpVisible = false;
                else
                    chooser.request.cancel();
            } else if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_L) {
                location.focusEntry();
            } else if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_F) {
                search.focusEntry();
            } else if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_H) {
                folder.showHidden = !folder.showHidden;
            } else if ((event.modifiers & Qt.AltModifier) && event.key === Qt.Key_Up) {
                folder.goUp();
            } else
                return;
            event.accepted = true;
        }
        OverlayScroll {
            id: form
            objectName: "chooserForm"
            anchors.fill: parent
            anchors.margins: 10
            contentHeight: contents.height
            Column {
                id: contents
                width: form.width
                spacing: 8
                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    text: chooser.request ? chooser.request.title : ""
                    wrapMode: Text.Wrap
                    color: Theme.foreground
                    font.family: Theme.fontFamily
                    font.pixelSize: 15
                }
                Flow {
                    width: parent.width
                    spacing: 8
                    Chip {
                        id: up
                        objectName: "chooserUp"
                        label: "Up"
                        onClicked: folder.goUp()
                    }
                    Chip {
                        id: home
                        objectName: "chooserHome"
                        label: "Home"
                        onClicked: folder.goHome()
                    }
                    Chip {
                        id: hidden
                        objectName: "chooserHidden"
                        label: "Hidden files"
                        on: folder.showHidden
                        onClicked: folder.showHidden = !folder.showHidden
                    }
                    Chip {
                        id: help
                        objectName: "chooserHelp"
                        label: "Keys"
                        onClicked: chooser.helpVisible = !chooser.helpVisible
                    }
                }
                Text {
                    textFormat: Text.PlainText
                    visible: chooser.helpVisible
                    width: parent.width
                    text: "Tab / Shift+Tab: controls · ↑/↓ or j/k: rows · Enter: enter folder or choose file · Space: mark (multiple files) · Ctrl+Enter: choose folder · Ctrl+L: location · Ctrl+F: search · Ctrl+H: hidden files · Alt+↑: parent · Escape: cancel. Double-click a row to enter or choose; touch a row, then use the action button."
                    wrapMode: Text.Wrap
                    color: Theme.dim
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
                Entry {
                    id: location
                    objectName: "chooserLocation"
                    width: parent.width
                    placeholder: "Location"
                    onAccepted: function (text) {
                        folder.goTo(text);
                        files.forceActiveFocus();
                    }
                    onCancelled: if (chooser.request)
                        chooser.request.cancel()
                }
                Entry {
                    id: search
                    objectName: "chooserSearch"
                    width: parent.width
                    placeholder: "Search names"
                    onTextChanged: folder.filter = text
                    onAccepted: files.forceActiveFocus()
                    onCancelled: if (chooser.request)
                        chooser.request.cancel()
                }
                ListView {
                    id: files
                    objectName: "chooserFiles"
                    width: parent.width
                    height: Math.max(100, chooser.height * 0.38)
                    model: folder
                    clip: true
                    activeFocusOnTab: true
                    boundsBehavior: Flickable.StopAtBounds
                    onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
                    Keys.onPressed: function (event) {
                        let row = currentIndex;
                        if (event.key === Qt.Key_Down || event.key === Qt.Key_J)
                            row++;
                        else if (event.key === Qt.Key_Up || event.key === Qt.Key_K)
                            row--;
                        else if (event.key === Qt.Key_Home)
                            row = 0;
                        else if (event.key === Qt.Key_End)
                            row = count - 1;
                        else if (event.key === Qt.Key_PageDown)
                            row += Math.max(1, Math.floor(height / 32));
                        else if (event.key === Qt.Key_PageUp)
                            row -= Math.max(1, Math.floor(height / 32));
                        else if (event.key === Qt.Key_Space) {
                            chooser.chooseRow(row, true);
                            event.accepted = true;
                            return;
                        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            if (event.modifiers & Qt.ControlModifier)
                                chooser.submit();
                            else
                                chooser.enterRow(row);
                            event.accepted = true;
                            return;
                        } else
                            return;
                        chooser.chooseRow(Math.max(0, Math.min(count - 1, row)), false);
                        event.accepted = true;
                    }
                    delegate: Rectangle {
                        id: fileRow
                        required property int index
                        required property string name
                        required property string kind
                        required property bool marked
                        readonly property bool matches: !chooser.request || kind !== "file" || (chooser.request.selectedFilter < 0 || chooser.request.matches(name))
                        width: files.width
                        height: 32
                        color: files.currentIndex === index ? Theme.fill(Theme.accent, 0.18) : "transparent"
                        Text {
                            textFormat: Text.PlainText
                            anchors.fill: parent
                            anchors.margins: 6
                            text: (fileRow.marked ? "✓ " : "") + (fileRow.kind !== "file" ? "▸ " : "") + fileRow.name
                            elide: Text.ElideMiddle
                            color: fileRow.matches ? Theme.foreground : Theme.dim
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                        RowTapHandler {
                            onClicked: function (position, modifiers) {
                                chooser.chooseRow(fileRow.index, (modifiers & Qt.ControlModifier) !== 0);
                            }
                            onActivated: chooser.enterRow(fileRow.index)
                        }
                    }
                    ImmediateWheel {
                        anchors.fill: parent
                        view: files
                        step: 32
                    }
                    C.ScrollBar.vertical: C.ScrollBar {}
                    Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                        border.width: files.activeFocus ? 1 : 0
                        border.color: Theme.accent
                    }
                }
                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    visible: folder.loading || folder.error !== "" || (chooser.request && chooser.request.error !== "")
                    text: chooser.request && chooser.request.error !== "" ? chooser.request.error : folder.error !== "" ? folder.error : "Loading…"
                    color: Theme.urgent
                    wrapMode: Text.Wrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
                Entry {
                    id: filename
                    objectName: "chooserFilename"
                    width: parent.width
                    visible: chooser.saving
                    placeholder: "File name"
                    onAccepted: chooser.submit()
                    onCancelled: if (chooser.request)
                        chooser.request.cancel()
                }
                ChoiceCombo {
                    id: filter
                    objectName: "chooserFilter"
                    width: parent.width
                    visible: chooser.request && chooser.request.filters.length > 0
                    model: chooser.request ? chooser.request.filters : []
                    textRole: "name"
                    currentIndex: chooser.request ? chooser.request.selectedFilter : -1
                    onActivated: if (chooser.request)
                        chooser.request.selectedFilter = currentIndex
                }
                Repeater {
                    id: choiceRows
                    model: chooser.request ? chooser.request.choices : []
                    delegate: ChoiceRow {}
                }

                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    visible: chooser.request && chooser.request.kind === "savefiles"
                    text: chooser.request ? chooser.request.files.join("\n") : ""
                    color: Theme.dim
                    wrapMode: Text.Wrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
                Flow {
                    width: parent.width
                    spacing: 8
                    Chip {
                        id: mark
                        objectName: "chooserMark"
                        visible: chooser.request && chooser.request.multiple
                        label: "Mark row"
                        onClicked: chooser.chooseRow(files.currentIndex, true)
                    }
                    Chip {
                        id: cancel
                        objectName: "chooserCancel"
                        label: "Cancel"
                        onClicked: if (chooser.request)
                            chooser.request.cancel()
                    }
                    Chip {
                        id: accept
                        objectName: "chooserAccept"
                        label: chooser.request ? chooser.request.acceptLabel : "Choose"
                        width: Math.min(implicitWidth, contents.width)
                        clip: true
                        usable: chooser.request && !chooser.request.busy
                        onClicked: chooser.submit()
                    }
                }
            }
        }
        Rectangle {
            anchors.fill: parent
            visible: chooser.request && chooser.request.overwritePaths.length > 0
            color: Theme.background
            OverlayScroll {
                anchors.fill: parent
                anchors.margins: 12
                contentHeight: confirmation.height
                Column {
                    id: confirmation
                    width: parent.width
                    spacing: 12
                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: chooser.request ? "Replace this existing file?\n" + chooser.request.overwritePaths.join("\n") : ""
                        color: Theme.foreground
                        wrapMode: Text.Wrap
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                    Flow {
                        width: parent.width
                        spacing: 8
                        Chip {
                            id: keepEditing
                            objectName: "chooserKeepEditing"
                            label: "Keep editing"
                            onClicked: if (chooser.request)
                                chooser.request.confirmOverwrite(false)
                        }
                        Chip {
                            id: replaceFile
                            objectName: "chooserReplace"
                            label: "Replace"
                            onClicked: if (chooser.request)
                                chooser.request.confirmOverwrite(true)
                        }
                    }
                }
            }
        }
    }
    component ChoiceCombo: C.ComboBox {
        id: combo
        palette.text: Theme.foreground
        palette.buttonText: Theme.foreground
        palette.highlight: Theme.accent
        palette.highlightedText: Theme.foreground
        palette.base: Theme.panel
        palette.window: Theme.panel
        palette.button: Theme.panel
        font.family: Theme.fontFamily
        font.pixelSize: 12
        background: Rectangle {
            color: Theme.panel
            implicitHeight: 32
            border.color: combo.activeFocus ? Theme.accent : Theme.line
            border.width: 1
        }
        delegate: C.ItemDelegate {
            id: option
            required property var model
            required property int index
            width: combo.popup.width
            text: model[combo.textRole]
            highlighted: combo.highlightedIndex === index
            contentItem: Text {
                text: option.text
                textFormat: Text.PlainText
                color: Theme.foreground
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
        }
        popup.height: Math.min(200, Math.max(40, chooser.height - 40))
        popup.parent: surface
        popup.x: 10
        popup.y: Math.max(10, (chooser.height - popup.height) / 2)
        popup.width: chooser.width - 20
    }
    component ChoiceRow: Column {
        id: choiceRow
        required property var modelData
        readonly property string selected: chooser.request ? chooser.request.choiceValues[modelData.id] : ""
        readonly property Item control: modelData.options.length > 0 ? choice : booleanChoice
        function closePopup() {
            if (!choice.popup.visible)
                return false;
            choice.popup.close();
            return true;
        }
        width: contents.width
        spacing: 4
        Text {
            textFormat: Text.PlainText
            width: parent.width
            text: choiceRow.modelData.label
            color: Theme.foreground
            wrapMode: Text.Wrap
            font.family: Theme.fontFamily
            font.pixelSize: 12
        }
        ChoiceCombo {
            id: choice
            objectName: "chooserChoice"
            width: parent.width
            visible: choiceRow.modelData.options.length > 0
            model: choiceRow.modelData.options
            textRole: "label"
            currentIndex: {
                for (let i = 0; i < choiceRow.modelData.options.length; ++i)
                    if (choiceRow.modelData.options[i].id === choiceRow.selected)
                        return i;
                return 0;
            }
            onActivated: if (chooser.request)
                chooser.request.setChoice(choiceRow.modelData.id, choiceRow.modelData.options[currentIndex].id)
        }
        Chip {
            id: booleanChoice
            objectName: "chooserBooleanChoice"
            visible: choiceRow.modelData.options.length === 0
            label: on ? "Yes" : "No"
            on: choiceRow.selected === "true"
            onClicked: if (chooser.request)
                chooser.request.setChoice(choiceRow.modelData.id, on ? "false" : "true")
        }
    }
}
