# Trash

Open **Trash** from the `+` tab menu or press `:` (or `Ctrl+P`) and choose
**Open Trash**. It opens a separate tab, leaving your folder tabs intact.
The list combines the user's `$XDG_DATA_HOME/Trash` with the current user's
`.Trash/<uid>` and `.Trash-<uid>` on mounted filesystems. Unmounted volumes
appear when mounted and refreshed. Discovery runs in the background only while
a Trash tab exists; switching away cancels its pending scan.

Each row shows the original path and local deletion date from its `.trashinfo`
record. Long paths are elided to keep every row reachable; Enter or the row
menu opens complete entry details in a scrollable dialog. The internal stored filename is not treated as the original name.
Missing, malformed or unreadable metadata is shown explicitly; restoring such
an entry is refused. An unreadable location is reported, rather than counted
as an empty Trash. **Details** opens the complete operation receipt or scan
error in a scrollable dialog.
An operation receipt remains after its refresh and while the listing stays
unchanged. A later change to entries, metadata or scan errors clears it.
Refresh progress and current scan errors take precedence over that receipt.

Use `j`/`k` or Up/Down to move, Home/End to reach the ends and Page Up/Down to
scroll. The first row is selected when the tab opens. Refresh keeps the selected
entry; restoring it selects a remaining neighbour. Space marks a row, `a` marks all and `A` clears marks. `r` or **restore**
restores the marked entries, or the current row if none are marked. `m`, Menu,
Shift+F10, right-click or pressing and holding a row by touch/pen opens its menu.
Wheel and touch scroll the list. Tab and Shift+Tab visit its controls in both
directions; Escape returns to the list. `F5` or **refresh** rereads the locations.
The command palette also offers **Restore from Trash** and **Empty Trash**.

Restoration uses the recorded original path. An occupied destination is never
overwritten: move or rename it before trying again. A missing original parent
must be recreated first. Restoration refuses symbolic links in destination
parent directories; use the actual directory path. A filesystem that cannot
perform the safe rename is reported and leaves the item in Trash.

File attributes, including extended attributes containing tags, travel with
the restored entry. Index records retained at its original path remain there;
records indexed at the stored Trash path are moved back with the entry,
including descendants and index-only tags. Tags removed from the index by a
trashing operation cannot be reconstructed by this browser. During the same
window session, **Undo** (`Ctrl+Z`) for that trashing operation retains its
captured tag records and can restore them; the browser and Undo share the
Trash metadata and location checks. Independent changes make Undo refuse
rather than overwrite them.

Press `e` or choose **empty…**, then explicitly choose **Permanently delete all
Trash entries**. Cancel is selected first. This permanently deletes the entries
captured when confirmation opens, including malformed entries, from all listed locations.
It does not remove new or changed entries that arrive after that scan. Errors
and partial completion are reported. Emptying Trash cannot be undone. To keep
some entries, restore them before emptying it.

These choices follow the [freedesktop Trash specification](https://specifications.freedesktop.org/trash/1.0/)
and the shared Trash-location approach in
[Dolphin/KIO](https://github.com/KDE/kio/blob/master/src/kioworkers/trash/trashimpl.cpp),
[Nautilus](https://github.com/GNOME/nautilus/blob/main/src/nautilus-file-operations.c),
and [Nemo](https://github.com/linuxmint/nemo/blob/master/src/nemo-trash-bar.c).
The separate location and restore/empty actions also match
[Omanta's places](https://github.com/28allday/omanta) and
[Thunar's Trash integration](https://github.com/xfce-mirror/thunar/blob/master/plugins/thunar-tpa/thunar-tpa.c).

The reader verifies ownership, the shared directory's sticky bit and directory
components without following links. Restore and empty recheck the listed
metadata and entry before writing. Emptying a symbolic link removes the link,
including broken links; it never deletes its target. Tests inject mounted
roots and use private XDG data directories, so neither tests nor screenshots
scan or mutate the user's Trash.
