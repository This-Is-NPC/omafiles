# Removable drives and phones

The **devices** sidebar lists removable storage and phones reported by GIO/GVfs.
Internal drives are excluded. Connecting or removing a device updates the list
without a refresh or an automatic mount. The selected device follows its identity
through status updates and reordering. Names come from the desktop service.

Press **d**, or choose **Devices** in the **:** command palette, to focus the
list. The mnemonic follows **t** for tags and **v** for saved views. This reveals
the sidebar even in a narrow window. **Tab / Shift+Tab** also
reach it; an empty list focuses its heading. **j/k**, arrows, **Home/End**,
**Page Up/Down** and **Ctrl+D/U** move within the list and keep the selection
visible. **Escape**, **h** or Left returns to files.

**Enter**, **Space**, **l**, a click or a tap opens the selected device in a
files tab, mounting it first when necessary. **m**, **Menu**, **Shift+F10**,
right-click, press-and-hold, or the row's **⋮** button opens its actions:

- **Mount** mounts without opening a tab.
- **Unmount** releases the selected filesystem.
- **Eject safely** asks the desktop to stop or eject the drive. Stopping a
  physical drive can unmount its other partitions as well.
- **Cancel device operation** requests cancellation of the current operation.

Only supported actions are offered. Operations are asynchronous, with status
and a visible cancellation button. Cancellation is a request: an operation
already completed by the service may have succeeded. Busy drives are refused;
the application never requests forced unmounts. Authorization, busy-device and
other service errors retain their reasons. A disappeared device cannot be
opened or operated on through a stale menu.

The status line shows a pending operation or cancellation before any previous
result. A completed device result is dismissed when the device snapshot or
storage state changes. Current absent-root warnings remain visible even if an
earlier mount or unmount succeeded.

## Desktop integration

An `omafiles-devices` helper runs the GIO volume monitor outside the GUI process, linking only GLib/GIO.
GIO's GVfs monitor uses UDisks2 for local storage and the corresponding MTP,
gphoto2 or AFC backend for phones. Install the backend matching your phone,
unlock it and permit file access on the phone. GVfs's FUSE bridge must expose
a local filesystem path for browsing; a successful mount without that bridge
gets an explicit explanation instead of opening an invented path. Password or
confirmation requests that the helper cannot present are canceled and reported;
the desktop's authorization agent handles ordinary UDisks policy prompts.

Mount locations come from the service. A declared root recognises storage when
it reappears at its configured path; visible folder and tag listings refresh
on storage changes, including the tag vocabulary, counts and absent-root warning.
Storage checks run outside the GUI thread. Root-relative tags remain in the index while the disk is
away. Declare a stable mount path, and use the root's `--mount` or `--check`
condition to distinguish an absent disk from an empty mount directory. Discovery
does not modify root declarations or assign devices to roots by label.

The window watches root declarations as well as the index. CLI changes are
reloaded automatically, including creation and atomic replacement of the
declarations file; **F5** rereads declarations before interpreting index rows.
An unreadable declarations file reports its own error rather than treating
valid root-relative index rows as corrupt.

Storage warnings are checked at startup and on every refresh, even without a
device-monitor service. An absent declared root remains visible when the index
is empty or other files are available. Completed copy, archive and Trash
receipts cannot cover that warning. Restoring the root's availability and
refreshing clears it without requiring a device event.

File and parent-directory watches follow Qt's
[filesystem watcher guidance](https://doc.qt.io/qt-6/qfilesystemwatcher.html),
including reattaching after replacement. Background readers retain synchronized
value snapshots of declarations so reloads cannot invalidate a reader's data.

This follows [Nautilus's GIO volume operations](https://gitlab.gnome.org/GNOME/nautilus/-/blob/main/src/nautilus-file-operations.c),
[GIO's volume monitor](https://docs.gtk.org/gio/class.VolumeMonitor.html) and
[UDisks2's default mount/unmount behaviour](https://storaged.org/doc/udisks2-api/latest/gdbus-org.freedesktop.UDisks2.Filesystem.html).
[Dolphin's Solid backend](https://invent.kde.org/frameworks/solid/-/blob/master/src/solid/devices/backends/udisks2/udisksstorageaccess.cpp)
also delegates storage operations to UDisks2. Using the existing GIO library
keeps both kinds of storage under one structured monitor and avoids parsing
localized command output.
