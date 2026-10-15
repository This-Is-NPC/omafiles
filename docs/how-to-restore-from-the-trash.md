# How to restore from the trash

**The question:** How do I recover deleted files? This page is enough on its own.

## Before you start

When you delete a file with the Trash action, it goes to the system Trash, not permanent deletion. You can browse the Trash and restore files to their original locations. Emptying the Trash permanently deletes them.

## Open Trash in the window

1. Press `:` or `Ctrl+P` to open the command palette
2. Type `Trash` and press Enter on **Open Trash**
3. Or click the `+` tab menu and choose **Open Trash**
4. A new tab opens showing all deleted files
5. Each row shows the original path and the date it was deleted

Navigate the Trash:
- Use `j`/`k` or arrow keys to move to a file
- Long paths are elided; press Enter or open the row menu to see the complete path
- Press `F5` or choose **Refresh** to rescan the Trash
- Press Escape to return to your folders

## Restore files

Restore a single file:
1. Use `j`/`k` to select the file
2. Press `r`, or open the row menu with `m` and choose **Restore from Trash**

Restore multiple files:
1. Mark files with Space (to toggle), `a` (to mark all) or `A` (to clear marks)
2. Press `r` to restore all marked files

Restore progress:
- A status message shows restoration progress
- If a destination already has a file with that name, restoration is refused; move the existing file first

## Empty the Trash

Delete everything permanently:
1. Press `e` or open the command palette and choose **Empty Trash**
2. A confirmation sheet appears with **Cancel** selected
3. Read what will be deleted
4. Choose **Permanently delete all Trash entries**
5. A status message shows progress
6. The Trash is now empty

**Emptying cannot be undone.** If you have files you want to keep, restore them first.

## What can go wrong

- **`Trash: original path already exists`**: A file with the same name is already in the original location. Move or rename the existing file, then restore the Trash entry.
- **`Trash: cannot restore original directory`**: The directory the file was deleted from no longer exists, or a symbolic link sits in its path. Create the missing directory, or use the actual directory path instead of the link, then restore.
- **`Trash: entry changed since it was listed; refresh and try again`**: The filesystem changed since the file was deleted (permissions, mount status, another operation). Refresh the Trash and try again.
- **Malformed or unreadable metadata**: A `.trashinfo` record can fail several checks (invalid header, missing or unsafe original path, missing deletion date). Such an entry is shown explicitly, and restoring it is refused. You may be able to recover the data with a file recovery tool.
- **Undo does not restore the file**: Undo for a trash operation uses the Trash metadata. If you have emptied the Trash or the file was already deleted by another method, Undo cannot recover it. [See Undo](undo.md) for what can be undone.

## See also

[Trash](trash.md) explains Trash locations, freedesktop metadata, tag recovery and safe refusal rules.
[Undo in the window](undo.md) explains how to undo a trash operation if you just deleted the file.
