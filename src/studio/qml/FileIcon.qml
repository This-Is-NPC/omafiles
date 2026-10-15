pragma ComponentBehavior: Bound

import QtQuick
import omafiles

Icon {
    id: icon
    required property FolderModel folder
    required property int rowIndex
    required property string path
    required property bool inViewport
    property bool moving: false
    property bool allowed: false
    property bool started: false
    onAllowedChanged: if (allowed && inViewport) started = true
    onInViewportChanged: {
        if (!inViewport) started = false
        else if (allowed) started = true
    }
    property int extent: 128
    readonly property Thumbnail thumbnail: preview.item as Thumbnail
    hasPreview: thumbnail !== null && thumbnail.ready

    Loader {
        id: preview
        asynchronous: true
        anchors.fill: parent
        active: icon.kind === "file" && icon.inViewport && icon.started
        sourceComponent: Thumbnail {
            onSourceChanged: icon.folder.refreshMetadata(icon.rowIndex)
            path: icon.path
            extent: icon.extent
            requested: icon.inViewport && icon.allowed && !icon.moving
        }
    }
}
