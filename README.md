# omafiles

Tags on files, CLI + Qt Quick window over the same text index. Nothing is moved,
renamed or wrapped — the filesystem stays the filesystem.

![the studio](docs/img/studio.png)
![the graph view](docs/img/graph.png)

## Install

Not published as an Omarchy default yet. From a checkout:

```bash
mise install
mise run deps      # Qt 6 + qmake
mise run build     # build/bin/omafiles and build/bin/omafiles-studio
mise run studio    # open the window
```

`mise run install` builds and puts both binaries, the desktop entry and the icon
under `~/.local`, so the window is in the app menu and `omafiles` is a command
anywhere — no root, and nothing under `/usr` for a package to collide with. Run
it again to update, `mise run uninstall` to take it back out; the tag index is
left where it is either way. `PREFIX=/usr/local mise run install` installs for
everyone instead, building as you and sudoing only the copies.

Released under the MIT license. `pkgbuild/omafiles.desktop` and `pkgbuild/omafiles.svg`
are the desktop entry and icon. The Arch recipe is in the sibling `omarchy-pkgs` repo
at `pkgbuilds/omafiles`.

## Shortcuts

- `j` / `k` — move the cursor
- `l` / `Enter` — open a file or enter a folder
- `h` — go back up
- `t` — tag panel
- `/` — filter the folder
- `:` — commands
- `Esc` — back to the list
- `1` / `2` / `3` — list, details, and icons
- `m` — menu
- `Ctrl+L` — edit the path
- `Tab` — next control (path, chips, tabs)
- `]` / `[` — next, previous tab
- Graph: `j` / `k` move nodes, `l` open, `Shift+h` / `Shift+j` / `Shift+k` / `Shift+l` pan, `+` / `-` / `=` zoom, `0` reset
- `Ctrl+\` — split the pane
- `Ctrl+,` / `Ctrl+.` — resize the split
- `Ctrl+W` / `Delete` — close the tab
- `?` — the key map

![The key sheet](docs/img/keys.png)

```bash
omafiles tag add --tag work -- ~/docs/invoice.pdf
omafiles ls --tag work
omafiles tags
```

Every verb is in [the command line](docs/cli.md).

The list is the folder — a folder shut, one that is missing, and one that is
genuinely empty are three different answers on the status line. Fifos and dangling
symlinks are listed; a fifo is not handed to the desktop. `mv` and `cp` refuse to
write over something already at the destination.

Colours come from the current omarchy theme and corner rounding from hyprland's
`looknfeel.lua`, both live.

## Requirements

- Qt 6: `qt6-base`, `qt6-declarative`
- `xdg-utils` for `omafiles open`
- `glib2` for `omafiles trash`
- `xdg-desktop-portal`
- [mise](https://mise.jdx.dev/) for the tasks above

`docs/img` is regenerated from the studio itself with `mise run shots`.
