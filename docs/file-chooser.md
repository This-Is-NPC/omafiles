# File chooser portal

omafiles provides the `org.freedesktop.impl.portal.FileChooser` backend for
applications using the desktop portal. OpenFile chooses one or more files or a
folder. SaveFile chooses a filename and asks before returning an existing file.
SaveFiles chooses a folder and returns a destination for each requested name,
keeping their order and choosing unused names when necessary. Pending names
that differ only in letter case receive distinct names as well, so they cannot
collide on a case-insensitive destination. The chooser
returns local file URIs; it does not create, overwrite or truncate files.
The requesting application performs the actual read or write.

## Install and choose the backend

`mise run install` installs the backend descriptor and a D-Bus activation
service alongside the studio. The service runs the installed absolute path to
`omafiles-studio --portal`. The default prefix is `~/.local`; registration lives
under `share/xdg-desktop-portal/portals/omafiles.portal` and
`share/dbus-1/services/org.freedesktop.impl.portal.desktop.omafiles.service`.
The installation does not change portal preferences or restart desktop services.

xdg-desktop-portal 1.20 discovers descriptors in `$XDG_DATA_HOME` (normally
`~/.local/share`) and `$XDG_DATA_DIRS`. For a custom installation prefix, make
its `share` directory available in `XDG_DATA_DIRS` to both the session bus and
xdg-desktop-portal. Restart the session after changing its environment. The
chooser must be installed where the session bus can discover its service.

To opt in, edit the portal configuration yourself. The user configuration
folder is `${XDG_CONFIG_HOME:-$HOME/.config}/xdg-desktop-portal`. A desktop-specific
file, such as `hyprland-portals.conf` when `XDG_CURRENT_DESKTOP=Hyprland`, takes
precedence over `portals.conf` in the same folder. Configuration uses the first
matching file; a new incomplete user file can hide the system's other choices.
Use the existing effective file, or copy the matching system configuration into
the user configuration folder first. Preserve its other interface preferences
and its `default` entry. Add or update this entry in its `[preferred]` group:

```ini
[preferred]
org.freedesktop.impl.portal.FileChooser=omafiles
```

This fragment is an edit to that group, rather than a complete replacement
configuration. Then restart the frontend yourself and reopen the requesting
application:

```sh
systemctl --user restart xdg-desktop-portal.service
```

To choose another backend, change the FileChooser entry to its installed
backend name and restart the frontend. Removing omafiles registration does not
edit that preference: select another backend before uninstalling.

## Using the chooser

The same actions work with keyboard, mouse and touch. Tab and Shift+Tab move
between visible controls; focused controls remain visible when the window is
small. Enter or Space activates a focused button. Escape cancels the request.
Click or tap a row to select it; double-click a folder to enter it. Enter on a
folder navigates into it. For a directory request, Ctrl+Enter or the accept
button chooses the highlighted folder, or the current folder when no child
folder is highlighted.

For multiple-file or multiple-folder requests, Space, Ctrl+click and the
**Mark row** button toggle selection. The button is also available by touch. Use the path
field (Ctrl+L) to change folders, the search field (Ctrl+F) to narrow the list,
and Ctrl+H to show hidden entries. The up button navigates to the parent folder.
SaveFile provides a filename field; an existing destination requires explicit
overwrite consent. Canceling that question returns to the chooser.

The application's filters are available in a filter control. Files outside the
selected filter are dimmed, rather than treated as unsafe: the portal protocol
allows an explicit selection outside a filter. The chosen filter is returned
to the application. Additional application choices appear as option controls
or boolean choices and their selected values are returned with the file URIs.

Requests are bounded and served in order. Cancellation from the application
closes its request, and a disconnected caller cannot leave a chooser behind.
Directory enumeration and final filesystem validation run outside the GUI
thread. KF6WindowSystem associates the chooser with the caller's supplied X11
or Wayland parent window. Without a parent identifier it opens independently.

## Protocol and verification

The backend owns `org.freedesktop.impl.portal.desktop.omafiles` and exports
`/org/freedesktop/portal/desktop`. It implements OpenFile, SaveFile and SaveFiles
with delayed method replies, plus `org.freedesktop.impl.portal.Request.Close`
for pending requests. It is separate from the normal file-manager window and
does not claim `org.freedesktop.FileManager1`.

The portal integration tests require `dbus-daemon`, `gdbus`, Python GObject
bindings (`python-gobject` on Arch), and `xdg-desktop-portal`. They exercise the
installed frontend and its Request.Response signal on a private bus.

Tests use fresh fake HOME and XDG directories, fake audio endpoints, and private
D-Bus buses. Registration tests activate a recording executable through a
private bus, including an installation prefix containing spaces, quotes,
backslashes and shell punctuation. They never change the real portal routing.

The design follows the [FileChooser backend protocol](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.impl.portal.FileChooser.html)
and [portal configuration precedence](https://flatpak.github.io/xdg-desktop-portal/docs/portals.conf.html),
with [Omanta](https://github.com/28allday/omanta) as a desktop integration precedent.
Native parenting uses [KF6WindowSystem](https://api.kde.org/kwindowsystem/namespaceKWindowSystem.html).
