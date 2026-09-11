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
- `v` — saved views
- `/` — filter the folder
- `r` / `F2` — rename, which is `mv` with the tags kept
- `n` — new folder
- `Ctrl+C` / `Ctrl+X` / `Ctrl+V` — copy, cut and paste files, through the
  same clipboard Nautilus and Dolphin use, so it works between them and here
- `:` — commands
- `Esc` — back to the list
- `1` / `2` / `3` — list, details, and icons
- `m` — menu
- `Ctrl+L` — edit the path
- `Tab` — next control (path, chips, tabs)
- `]` / `[` — next, previous tab
- Tags tab: a tag is a folder — `l` walks into one and narrows the listing to
  the files carrying every tag in the path, `h` takes the last one off, `~`
  takes them all off. `#notes/#work` and `#work/#notes` are the same listing.
  `(no tag)` at the top is the other direction: what has not been filed at all,
  out of the list `sweep` wrote, with the age of that list on the status line
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

A folder, a tag and a query are one thing saved, so there is one verb for all
three and one list in the panel beside the files:

```bash
omafiles view add --name receipts --tag finance --tag invoice
omafiles view add --name "to file" -- ~/scans
omafiles view
```

`v` walks that list and Enter opens whichever kind the row turned out to be — a
tag view as a tags tab standing on its query, a folder view as a folder. In the
window, `Save this as a view` in the row menu saves what is on screen.

A disk that is not always plugged in wants a declared root:

```bash
omafiles roots add --id archive --check .plugged-in -- /mnt/archive
```

Rows under it are stored as the root and the rest of the way there, so remounting
the same storage somewhere else leaves every tag where it was. And a root that is
declared and not here makes its rows unreachable rather than missing — which is
the difference between `prune` doing its job and `prune` throwing away the tags
of an archive that is sitting in a drawer.

Tags travel with the file in its own xattr, so something else moving, copying or
restoring a file does not lose them — it only loses the index row. `sweep` reads
them home:

```bash
omafiles sweep            # every declared root that is here
omafiles sweep --status   # what the last run found
```

It is the routine as a verb rather than a daemon, so there is one thing to read
the behaviour of and nothing to install to try it. It only ever makes the index
know more: row and file are merged both ways and nothing is removed from either,
a row whose file is gone is counted and left where it is — `prune` is still the
verb that drops one, and it still asks — and a run that cannot take the index
lock steps aside rather than making the window wait.

`sweep` also writes down what it saw, so the one question the index cannot
answer stops costing a walk:

```bash
omafiles ls --untagged --cached      # instant, and it tells you how old it is
omafiles ls --untagged ~/scans       # walks the disk, exact
```

That list is a cache and the difference matters: the index is what you marked,
with a mirror in each file's own xattr and worth backing up; the list is what is
on the disk, lives in `$XDG_CACHE_HOME/omafiles/names`, and deleting it costs one
sweep and nothing else. Both answers are offered rather than one pretending to be
the other.

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
