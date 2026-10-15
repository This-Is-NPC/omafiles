pragma ComponentBehavior: Bound

import QtQuick
import omafiles

Item {
    id: grip
    objectName: "grip"
    required property FolderModel folder
    required property string path
    signal started()

    DragHandler {
        id: drag
        target: null
        acceptedButtons: Qt.LeftButton
        grabPermissions: PointerHandler.CanTakeOverFromAnything
                         | PointerHandler.ApprovesTakeOverByAnything
        onActiveChanged: {
            if (active) {
                const paths = grip.folder.targetPaths(grip.path)
                grip.started()
                Qt.callLater(FileTransfer.startDrag, grip, paths)
            }
        }
    }
}
