# Undo in the window

Press **Ctrl+Z**, choose **Undo** in the command palette (`:` or Ctrl+P), or use
the Undo button beside the navigation buttons. The palette and button name the
last operation. The button has the same Tab, Enter, Space and pointer activation
as the other toolbar controls. Text fields keep their own editing shortcuts;
leave the field or sheet before undoing a file operation.

Rename (including a pattern), move, copy/duplicate, new folder, trash and file
tag edits enter an in-memory journal in this window session. Clipboard and drop
operations use the same journal. A batch is one entry containing the items that
actually succeeded; a refusal or a no-op adds no successful step. Undo reverses
the newest entry first. There is no redo and no history after closing the
window. The journal retains at most 50 entries, each with a 1 MiB payload limit.
An unavailable entry reports why rather than reaching past it and undoing an
older operation unexpectedly.

Undo checks the result before touching it. Changed content, file identity,
permissions, ownership, symbolic-link target, on-disk tags or indexed tags cause
a refusal identifying the affected target or index. Combined file-state checks
report “changed since the operation” without diagnosing which attribute changed.
Content is checked with a streaming SHA-256
hash, so restoring a file's modification time does not conceal an edit. Folder
checks include their contents; a newly created folder must still be empty.
Directory modification times alone are ignored because creating and undoing a
child changes them. Scans and hashes run outside the GUI thread.

A move, rename or trash restore refuses an occupied original name, including a
dangling symbolic link. A copy undo removes only the copy it created. Tags and
their index records return with the file; undoing a tag edit restores its prior
value, including the absence of tags. No tag write follows a symbolic link.
[Replacing a conflict](conflicts.md) journals the displaced entry before the
incoming file. Undo checks both before removing the incoming file, then restores
the old destination and its tags.
Local Trash entries are identified by their [freedesktop `.trashinfo`](https://specifications.freedesktop.org/trash/1.0/) original
path and the trashed object's identity, excluding records that existed before the operation.

A refusal leaves that entry available for retry after the condition is resolved.
If a batch reversal encounters a later failure, completed reversals are retained
and its remaining work stays in the journal. The status receipt says why it
stopped. While an operation or Undo is running, another journaled operation is
refused with a wait message; navigation remains available.

## Prior art and choices

[KIO's FileUndoManager](https://api.kde.org/kio-fileundomanager.html) records
completed jobs; its [UI interface](https://api.kde.org/kio-fileundomanager-uiinterface.html)
asks about modified copied files. Omafiles refuses changed results instead of
offering to delete them, making Ctrl+Z conservative.
[Nautilus](https://github.com/GNOME/nautilus/blob/main/src/nautilus-file-undo-operations.c)
implements inverse operations and locates Trash items by original path and
trash metadata in a worker. Omafiles keeps its feature in memory for the window
session, avoiding a persistent recovery protocol.
