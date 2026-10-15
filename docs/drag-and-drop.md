# Dragging files

Drag a file or folder by its icon or name in list, details or icon view. A marked
row carries the marked set; an unmarked row carries that row alone. The window
exports local file URLs, so applications such as file managers, browsers and
editors can receive them. The receiving application decides what to do with an
outgoing drag.

Drop local files from another application or this window onto a folder row,
its parent row, a folder tab, or the open folder in either pane. The destination
is highlighted for an offered file-URL drag; release validates the URLs and
paths and reports any refusal. Dropping onto an inactive tab uses
that tab's folder without switching the current tab. Tag, search, recent and
graph tabs have no background destination; a concrete folder row can still be
a destination. A modal dialog blocks drops.

For drops into this window:

- Ctrl copies.
- Shift moves.
- Ctrl+Shift creates symbolic links.
- Without a modifier, files on the destination's device move; files from another
  device copy. A set spanning devices copies.

The destination and file list are fixed when you release. Device checks and
writes run asynchronously; the status bar reports completion or refusal.
Copies and moves use the CLI's `cp` and `mv`, including tag propagation, xattrs
and refusal to overwrite an existing name. Links keep the source unchanged and
refuse an existing destination, including a dangling link. If some link names
are refused, the receipt states how many links were created. A native drag
completing does not instruct this window to delete source files.

Copies take regular files, as the CLI does; a folder copy is refused without
copying its tree. Same-device folder moves use the CLI folder rename;
cross-device folder moves are refused. Outgoing folder drags remain available
to applications that support their own folder operations.

Only local file URLs are accepted. A mixed local/remote set is refused as a
whole. Such a clipboard set is unavailable for paste or linking. The source
must offer copying, which lets this window own the complete operation without
asking the source to delete files before a move succeeds.
Native link negotiation with other applications depends on the desktop's Qt
backend; Ctrl+Shift links into this window work through its own link action.

# The same operations by keyboard

Copy the row or marked set with Ctrl+C, or cut it with Ctrl+X. Reach the
destination folder, then Ctrl+V copies or moves the clipboard files. Ctrl+Shift+V
creates links there and keeps the clipboard available.

To reach a folder row, select it and press Enter. To reach another tab, use
Ctrl+Tab or Ctrl+Shift+Tab. To reach another pane, use Alt+Left or Alt+Right.
Then paste or create links with the same keys. The palette and row menu also
offer copy, cut, paste and link actions. These keyboard and drop paths share
the same transfer implementation and refusals.

For another application, use Ctrl+C, focus that application and paste. It
chooses how to handle the clipboard's file URLs, just as it chooses how to
handle an incoming drag.
