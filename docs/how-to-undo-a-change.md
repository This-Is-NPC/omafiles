# How to undo a change

**The question:** How do I undo a file operation? This page is enough on its own.

## Before you start

Undo reverses the last file operation you performed in this window: rename, move, copy, new folder, delete/trash, or tag edit. Undo works for this window session only; it does not persist after closing the window. There is no redo.

## Undo an operation

Press `Ctrl+Z`:
- The last operation is reversed
- A status message describes what was undone
- Tags are restored with the file
- The file is removed from Trash if it was a trash operation
- Moved files return to their original locations

Or use the Undo button:
- Click the Undo button in the toolbar (beside the navigation buttons)
- It shows the name of the last operation
- Press Enter or Space to activate it

Or use the command palette:
- Press `:` or `Ctrl+P`
- Choose **Undo** and press Enter
- The palette shows the name of the last operation

## What you can undo

- **Rename** — file returns to its original name, tags preserved
- **Move** — file returns to its original location, tags preserved
- **Copy or duplicate** — the copy is deleted
- **New folder** — the empty folder is deleted
- **Trash/Delete** — file returns to its original location, tags and metadata restored
- **Tag edit** — tags return to their previous values (add, remove or rename reversed)
- **Clipboard operations** — drops and pastes using Ctrl+C, Ctrl+X, Ctrl+V are undone

Multiple operations are reversed one at a time in reverse order. The history holds at most 50 operations, and is cleared when you close the window.

## What can go wrong

- **`Cannot undo: PATH changed since the operation; it was left untouched`**: The file was modified since the operation. Undo is conservative and refuses to replace a changed file. Keep the file and undo later, or delete it manually and undo to restore the old version.
- **`Cannot undo: PATH is occupied; it was left untouched`**: The destination of an undo operation is now occupied. Move the file first, then undo.
- **Permission denied**: You do not have permission to write to the destination. Check permissions on the destination directory.
- **`Nothing to undo in this window session`**: Opening a file or a view never enters the undo journal — only file operations do. If no file operation has happened yet in this session, there is nothing to undo.
- **Undo does not appear in the command palette**: An operation that succeeded but did nothing (like tagging a file it already has a tag with) does not enter the undo journal. Undo is only available after operations that actually changed something.

## See also

[Undo in the window](undo.md) explains what is checked before undoing, Trash entry identification, tag recovery and the undo journal limit.
