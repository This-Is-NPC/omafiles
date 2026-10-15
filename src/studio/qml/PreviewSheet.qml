pragma ComponentBehavior: Bound

import QtQuick
import "Focus.js" as Focus
import omafiles

Rectangle {
    id: sheet
    objectName: "previewSheet"
    readonly property Item focusTarget: scroller
    property string path: ""
    property bool frontmost: true
    property bool initialized: false
    property Preview renderer: null
    property OverlayScroll scroller: null
    property bool previousAvailable: false
    property bool nextAvailable: false
    signal stepRequested(int direction)
    visible: false
    color: Theme.fill(Theme.background, 0.72)
    Accessible.ignored: true

    function openFor(file) {
        path = file
        visible = true
        if (scroller) scroller.contentY = 0
        if (focusTarget) focusTarget.forceActiveFocus()
    }
    function close() { visible = false; path = "" }
    onVisibleChanged: if (visible) initialized = true
    onFrontmostChanged: if (!frontmost && renderer) renderer.pause()

    MouseArea { anchors.fill: parent; onClicked: sheet.close() }
    FocusScope {
        id: scope
        anchors.fill: parent
        Keys.onPressed: function (event) {
            const navigation = event.modifiers === Qt.NoModifier
            if (event.key === Qt.Key_Escape || event.text === "i") sheet.close()
            else if (navigation && (event.key === Qt.Key_Right || event.text === "j")) sheet.stepRequested(1)
            else if (navigation && (event.key === Qt.Key_Left || event.text === "k")) sheet.stepRequested(-1)
            else if (event.text === "p" || event.text === "P") sheet.renderer.togglePlayback()
            else if (event.text === "[") sheet.renderer.seek(-5)
            else if (event.text === "]") sheet.renderer.seek(5)
            else if (navigation && sheet.scroller && sheet.scroller.handleKey(event)) return
            else return
            event.accepted = true
        }
        Loader {
            anchors.fill: parent
            active: sheet.initialized
            onLoaded: if (sheet.visible) sheet.focusTarget.forceActiveFocus()
            sourceComponent: OverlayPanel {
                id: panel
                preferredWidth: 900
                preferredHeight: 640
                Accessible.role: Accessible.Dialog
                Accessible.name: "Quick preview"
                MouseArea { anchors.fill: parent; onClicked: function (mouse) { mouse.accepted = true } }
                OverlayScroll {
                    id: scroll
                    Component.onCompleted: sheet.scroller = scroll
                    anchors.fill: parent
                    anchors.margins: 12
                    Keys.onTabPressed: function (event) { Focus.move(event, scroll.nextItemInFocusChain(true), previewText) }
                    Keys.onBacktabPressed: function (event) { Focus.move(event, previewText) }
                    activeFocusOnTab: true
                    contentHeight: contents.implicitHeight
                    Column {
                        id: contents
                        width: scroll.width
                        spacing: 10
                        Label {
                            width: parent.width
                            text: sheet.path
                            wrapMode: Text.WrapAnywhere
                            maximumLineCount: 2
                            elide: Text.ElideMiddle
                        }
                        Flow {
                            width: parent.width
                            spacing: 8
                            Chip { objectName: "previewPrevious"; label: "Previous"; key: "k / ←"; usable: sheet.previousAvailable; onClicked: sheet.stepRequested(-1) }
                            Chip { objectName: "previewNext"; label: "Next"; key: "j / →"; usable: sheet.nextAvailable; onClicked: sheet.stepRequested(1) }
                            Chip {
                                objectName: "previewPlay"
                                visible: preview.kind === "audio" || preview.kind === "video"
                                label: preview.playing ? "Pause" : "Play"
                                key: "P"
                                onClicked: sheet.renderer.togglePlayback()
                            }
                            Chip { objectName: "previewSeekBack"; label: "−5 s"; key: "["; visible: preview.kind === "audio" || preview.kind === "video"; onClicked: sheet.renderer.seek(-5) }
                            Chip { objectName: "previewSeekForward"; label: "+5 s"; key: "]"; visible: preview.kind === "audio" || preview.kind === "video"; onClicked: sheet.renderer.seek(5) }
                            Chip {
                                objectName: "previewClose"
                                label: "Close"
                                key: "Esc / i"
                                onClicked: sheet.close()
                            }
                        }
                        Preview {
                            id: preview
                            objectName: "quickPreview"
                            Component.onCompleted: sheet.renderer = preview
                            path: sheet.visible ? sheet.path : ""
                            width: parent.width
                            height: hasImage ? Math.max(120, Math.min(420, panel.height - 150)) : 0
                        }
                        ReadOnlyText {
                            id: previewText
                            objectName: "previewText"
                            width: parent.width
                            text: preview.loading ? "Loading preview…" : preview.text
                            focusFrame: scroll
                            Keys.onTabPressed: function (event) { Focus.move(event, scroll, previewText.nextItemInFocusChain(false)) }
                            Keys.forwardTo: [scope]
                            wrapMode: TextEdit.WrapAnywhere
                            font.pixelSize: 13
                        }
                        Label {
                            width: parent.width
                            quiet: true
                            wrapMode: Text.Wrap
                            text: "i / Esc close · j k / ← → next, previous · PgUp PgDn scroll · P play/pause · [ ] seek"
                        }
                    }
                }
        }
        }
    }
}
