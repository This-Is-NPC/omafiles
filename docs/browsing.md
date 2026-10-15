# Browsing large folders

Folder names load in a background task. Changing the folder or the query cancels
outdated work; details and status belong to the current folder. Name filtering
with **/** searches the entire folder, including entries outside the viewport.
Typing updates the query without waiting for the directory to be read again.
Creating, deleting, renaming or replacing an entry refreshes the listing
automatically. Changes to an entry's permissions, owner or timestamps appear
after an explicit refresh.

The status line describes current work first: device, archive, file-operation
and Trash progress take precedence over completed results. Current loading or
storage errors also take precedence over a result. A completed result is a
receipt, not a permanent description of the folder. It disappears when a new
action starts, the folder or query changes, the active tab or pane changes,
storage or indexed records change, or the folder's directory entries change.
Explicit file-browser refresh also dismisses it. Moving the cursor alone keeps
the receipt. [Trash](trash.md) retains its own receipt across an unchanged scan.
Background work finishing after its context was replaced cannot put an old
receipt back into the status line. Open result dialogs and sheets keep their
detailed text.

This follows the separation of progress and contextual messages in
[Dolphin's status bar](https://invent.kde.org/system/dolphin/-/blob/master/src/statusbar/dolphinstatusbar.cpp)
and the explicit dismissal of completed notifications in
[Nautilus](https://gitlab.gnome.org/GNOME/nautilus/-/blob/main/src/nautilus-window.c)
and [libadwaita toasts](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1-latest/method.Toast.dismiss.html).

Names and directory types are kept in a compact snapshot. File details are read
when a visible row or the details panel needs them, with a bounded cache. Sorting
by metadata or applying metadata criteria examines matching files in the
background, since those operations need information beyond their names.

Large name sorts and queries use contiguous chunks on a shared bounded worker
pool. Each chunk contains at least 32,768 entries; small queries and a one-worker
budget execute the same processing directly without extra tasks or merge
buffers. Adjacent chunks that are already ordered need only a boundary comparison.
Each scanning worker accumulates on its own stack and publishes its chunk once,
avoiding shared counters and vector headers in the hot loop.
Sorted chunks merge with the same folders-first, case-insensitive
comparison. Filter results join in source order, preserving parent rows,
descending groups, tags and progressive narrowing of completed queries. Only a
complete result for the current query is published; changing folders or typing
a replacement cancels queued chunks. An active standard sort finishes its
current chunk before observing cancellation.

Directory and thumbnail pools share one CPU-budget policy. It combines the
process's `sched_getaffinity` mask with the smallest visible ancestor CPU quota
(`cpu.max` on cgroup v2, `cpu.cfs_quota_us` / `cpu.cfs_period_us` on v1).
Fractional quotas round down with a minimum of one worker, and each pool is
capped at four. Unavailable or malformed control metadata selects one worker.
A one-CPU affinity mask takes the serial policy path without reading cgroup
files. Each pool establishes its budget when it is created. Native image
thumbnail workers retain the full process affinity mask so the scheduler can
balance them with the window. Private directory chunk workers take disjoint
CPU slots and set their thread affinity to the assigned slot; the GUI and
global pool retain their affinity. External thumbnail helpers use an explicitly
selected allowed CPU. See [directory worker affinity and serial thresholds](benchmark-directory-parallel.md).


**Home**, **End**, **Page Up**, **Page Down**, and the cursor keys keep the selected
row visible in list, details, and icon views. Returning to a tab restores its
cursor into view. Mouse-wheel notches move the viewport immediately so the
requested rows are visible in the next frame; touchpad gestures and touch dragging
use the view's native scrolling. Cursor highlights have no movement or resize
animation, keeping keyboard navigation and opening menus immediate.

[Quick preview](preview.md) opens on **i** without leaving the file list.

Sheets, menus, inline text prompts, the command palette and dialogs form a focus
stack. Each opens on the first control of its Tab cycle, including the first
opening of content loaded on demand. Closing the frontmost overlay returns the keyboard to the last focused
control in the visible overlay underneath, preserving its text and selection.
This applies whether a keyboard action, pointer action or desktop D-Bus request opened the
overlay. **Escape** closes one layer at a time; the underlying sheet remains
usable without clicking it. When the stack is empty, focus returns to the
available control that opened it, or to the file list if that control is gone.
Successful sheet actions return focus by the same rule as cancellation.
When a list rebuilds its rows, focus returns to the list that owns the row.
A footer action remains the focus origin of an overlay it opens.
A command that opens a new text prompt keeps focus in that prompt. **Tab** and
**Shift+Tab** cycle through its text field, Cancel and the visible confirmation
action; **Escape** cancels it and returns to the
file list. This includes rename, batch rename, new folder, marking by pattern,
find, naming a saved view, the live filter and the path entry. Text and selection
survive dialogs opened above these fields. Clicking a pane finishes the live
filter while keeping its query, so pointer selection returns to the list. A
visible cancel hint always has a keyboard path. Keeping focus with the visible
layer prevents asynchronous desktop receipts from stranding an editor or the
help sheet. Each Properties or default-manager result opens its own dialog, so
closing a later result keeps earlier receipts and Undo
instructions available.

**Tab** and **Shift+Tab** visit the visible, enabled controls within a dialog or
sheet. Read-only receipt and preview text remain selectable, with a visible
focus outline, and let Tab leave the text without discarding its selection.
Preview text participates in the same Tab cycle as its scrolling viewport and
buttons, so text can be selected and copied entirely by keyboard.
**Enter** or **Space** activates the focused button. A permission sheet that
refuses a change skips its disabled Apply button and reaches Cancel instead.

The traversal follows Qt's [native focus chain](https://doc.qt.io/qt-6/qtquickcontrols-focus.html)
and [Tab navigation](https://doc.qt.io/qt-6/qml-qtquick-keynavigation.html).
Read-only [TextEdit](https://doc.qt.io/qt-6/qml-qtquick-textedit.html) fields
use that chain; the preview text wraps forward to its scrolling viewport and
backward to Close.
Dialogs are recognized through Qt's common Popup base so their footer controls
belong to the same focus layer. Buttons share the window's focus and keyboard
activation component.

Keyboard navigation and key hints follow the focused control. **t** and **v**
focus the tag or saved-view list, revealing the sidebar at narrow widths; an
empty list focuses its heading. Clicking or tapping a sidebar heading focuses
its list. **j/k**, Home/End, Page Up/Down and Ctrl+D/U then
navigate that list. Returning focus to the file-list control restores file
navigation, including after a forward and reverse Tab round trip. Toolbar,
details and footer controls use file-view shortcuts; Enter/Space activate the
focused button. Sidebar Enter hints describe the focused tag or saved-view
list. Footer actions describe the file view while a footer button has focus,
so an action's label does not depend on the last sidebar section visited.
Tapping a hint preserves the current control's focus while invoking its
displayed command, including a sidebar tag toggle.

### Touch menus and footer actions

Press and hold a file, tag or saved-view row to open its context menu at the
contact point. A hold does not open the file, toggle the tag filter, change
marks or run a write. Moving past the platform drag threshold cancels the
hold so dragging and scrolling can continue. List, details and icon layouts
share this gesture; the same file rows serve directory, search, tag and saved
view results. Right-click opens the same menu with a mouse. **m**, **Menu** and
**Shift+F10** open it from the keyboard, including the tag vocabulary panel. Its menu toggles the tag filter; tag editing
actions appear when a file or directory is selected for editing.

The footer's visible key-and-label hints are buttons: tap either part, or use
**Tab** and **Enter / Space**. They invoke the displayed keyboard command. For
a grouped hint such as **j k move**, tapping invokes its first key (**j**, move
down); both directions remain available on the keyboard. Hints wrap within
the window so every action remains reachable. Prompt
**cancel** and **rename / make / mark / find / save** actions use the same
cancellation and submission paths as **Escape / Enter**.

The gesture follows the conventional touch-and-hold context menu described by
[Android](https://developer.android.com/develop/ui/views/components/menus)
and used for files and folders in [Dolphin](https://bugs.kde.org/show_bug.cgi?id=441419).
Qt's [TapHandler](https://doc.qt.io/qt-6/qml-qtquick-taphandler.html) supplies the
platform hold interval and passive drag-threshold cancellation, so it cooperates
with scrolling and drag handlers instead of introducing another gesture timer.

### Keyboard controls inside panels

Every editable permission bit is a separate **Tab / Shift+Tab** stop, in
owner, group, others order, with read, write, execute within each group.
**Enter / Space** flips the focused bit. In the Permissions sheet these are
draft changes: focus **Apply** to save or **Cancel** to discard. **h / l** and
the left/right arrows move between bits. The details sidebar applies a bit
change immediately, just as clicking it does; links and non-owned rows have
disabled bits and are skipped.

Scroll containers consume **Up / Down / Page Up / Page Down / Home / End**
only without modifiers and while their content overflows the viewport. When details fit, these keys
from a focused permission bit or other details button reach the file view.
An overflowing panel keeps keyboard scrolling, including at its boundaries.
Menus and suggestion lists retain selection navigation even when every row
fits. Modified text-navigation keys belong to the focused text control: its
supported gestures move the caret or extend selection, and unsupported gestures
do not become ancestor scrolling or move focus to the panel. This follows
[Qt Quick's key propagation and acceptance rules](https://doc.qt.io/qt-6/qml-qtquick-keys.html).
Tag suggestions and command results also preserve this ownership: Shift and
Ctrl+Shift arrows/pages/documents stay with the text editor, even when Qt declines
the gesture. They do not select a result or reveal a different panel position.
The command palette's explicit Ctrl+Home/End commands select its first/last result;
Ctrl+N/P also choose the next/previous result. Unmodified arrows/pages keep their
documented result or panel navigation.

Sidebar tag chips, the add-tag button, and the saved-view and tag lists join
the Tab order. A list has one focus stop; arrows choose its rows and
**Enter / Space** opens the saved view or toggles the tag. Its focus outline
and current row remain visible while scrolling or resizing. Menu selection
uses **Tab / Shift+Tab** or arrows, and both **Enter** and **Space** activate
the highlighted item. Help has a visibly focused reading viewport with page
navigation.

An inline prompt's Tab cycle contains its text field and any visible footer
submit/cancel buttons. **Escape** cancels from every stop. Text fields retain
ordinary typing: **Space** inserts a space, while **Enter** submits. The
command palette and tag suggestions keep their editor as the keyboard proxy:
arrows choose a result, Enter chooses it, and Tab completes a tag suggestion.
Space in those editors remains text input. Passive labels, images, tooltips
and scrollbars do not introduce extra action stops; the containing viewport
provides keyboard scrolling.

An active inline prompt gives its Cancel and Submit actions priority over folder
counts and pending receipts; the receipt remains available after the prompt closes.

This follows Qt's [KeyNavigation](https://doc.qt.io/qt-6/qml-qtquick-keynavigation.html)
and [keyboard focus](https://doc.qt.io/qt-6/qtquick-input-focus.html) conventions:
disabled or hidden controls are skipped, and text input keeps its editing
semantics while buttons share visible focus and keyboard activation.

### Compact footer and unreadable images

Footer action hints wrap onto additional lines as the window narrows. File counts
and descriptive text share a capped portion of the width so every action remains
visible and reachable by touch, pen and keyboard, including at 420 × 300.

An image that cannot be decoded reports that its data is incomplete or unreadable
when Qt provides no specific cause. Specific decoder and device errors retain
their reported cause; source-size and pixel limits are reported only when exceeded.
This follows QImageReader's distinction between image dimensions and a failed
read, and uses Qt Quick Flow for the conventional wrapping layout.

### Reversing focus navigation

Shift+Tab retraces Tab's order through the sidebar and footer as well as sheets,
dialogs and inline prompts. In menus and the command palette, Tab wraps from the
last action to the first and Shift+Tab wraps from the first to the last, keeping
the highlighted action visible. Arrow-key navigation retains its endpoint limits.

Both native Backtab events and Shift-modified Tab events follow the reverse
order. Explicit links normalize the direction before moving focus, rather than
letting a forward-only KeyNavigation handler disagree with Qt's default traversal
at a group's open end. The order follows [Qt keyboard focus conventions](https://doc.qt.io/qt-6/qtquick-input-focus.html).

Open **Trash** in the `+` tab menu or from commands (`:` / `Ctrl+P`) to browse
and restore deleted entries, or explicitly empty the Trash. See
[Trash](trash.md) for keyboard controls, tag recovery and refusal rules.

### Archive operations

The row menu and command palette offer [Compress and Extract](archives.md) for
files and marked sets, with a keyboard-accessible destination/format sheet and
cancellable background preparation.

**d** focuses [Devices](devices.md), including at narrow window widths. Its
list follows the same keyboard navigation and touch menu conventions as saved views.

Tooltips appear after the pointer moves within the window and rests on a control
for 480 ms. Opening a window under a stationary cursor does not request help,
following Qt's motion-triggered tooltip convention. Mouse and hovering stylus
input use the same rule; keyboard navigation does not open tooltips.

### Watching the folder on screen

The listing and visible previews watch a directory's own entries only through
[inotify](https://man7.org/linux/man-pages/man7/inotify.7.html); previews also
watch their source file. Watching
events about the entries themselves, such as attribute changes, makes the kernel
[flag every cached child](https://github.com/torvalds/linux/blob/master/fs/notify/fsnotify.c)
when the watch is added. In a folder of a million files that costs about 90 ms
of CPU on every open. Creation, deletion, renames and atomic replacement, which
is how editors save, remain immediate.
