pragma ComponentBehavior: Bound

import QtQuick
import omafiles

Item {
    id: grip
    objectName: "grip"
    required property FolderModel folder
    required property string path
    // The row's icon, drawn into the drag image.
    required property Item icon
    signal started()

    DragHandler {
        id: drag
        target: null
        cursorShape: Qt.ClosedHandCursor
        acceptedButtons: Qt.LeftButton
        grabPermissions: PointerHandler.CanTakeOverFromAnything
                         | PointerHandler.ApprovesTakeOverByAnything
        onActiveChanged: {
            if (active) {
                const paths = grip.folder.targetPaths(grip.path)
                grip.started()
                Qt.callLater(FileTransfer.startDrag, grip, paths, grip.icon, grip.path,
                             grip.folder.tagsFor(grip.path))
            }
        }
    }
}
