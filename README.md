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
- `f` — find by name across the declared roots, `*.pdf` and the like, out of
  the list `sweep` wrote; the status line says how old that list is, and `h`
  comes back out
- `F` — narrow what the tab lists: folders or files, the type or a tag of
  the row under the cursor, tagged or untagged, modified in the last day,
  week, month or year
- `Space` — mark the row; `J` / `K` mark it and step; `*` marks by a pattern
  like `*.pdf`; Shift and a click marks a range
- `r` / `F2` — rename, which is `mv` with the tags kept; over a marked set it
  asks for a template instead, `{stem}-{n}{ext}` and the like, and runs
  `mv --pattern`
- `n` — new folder; the row menu also makes a link beside a file, or links
  what is on the clipboard into the folder on screen
- `Permissions…` in the row menu or at `:` — rwx for owner, group and others
  over the row or the marked set, shown before it is applied and run as
  `omafiles chmod`; in the details panel a click on a mode bit does the same
  for one row. Greyed where the file is not yours or is a link
- `Ctrl+C` / `Ctrl+X` / `Ctrl+V` — copy, cut and paste files, through the
  same clipboard Nautilus and Dolphin use, so it works between them and here
- `:` — commands; among them `Recently opened on the desktop`, which lists
  what the GNOME recent list holds, the way `omafiles recent --desktop` does
- `Esc` — back to the list
- `1` / `2` / `3` — list, details, and icons
- Details columns: right-click a heading to show or hide tags, type, size,
  modified, owner and mode, or move the column left or right; `:` has the same
  entries. Name is always first. Owner and mode headings do not sort. The
  choice is one for the window, kept in `$XDG_STATE_HOME/omafiles/studio.conf`
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

`F` narrows whatever a tab is listing — a folder, a tag, a view, a `f`
search — by what each row is, the tags it carries and when the file was last
written. Every criterion must hold, and so must the `/` filter; each one is a
chip above the status line that takes it off when clicked, beside `Clear`. The
date is the file's own, the one `ls --changed-within` reads, and never when it
was tagged, which is `--since`. Only the rows already listed are narrowed:
nothing is looked for in the folders below.

Filters belong to the tab and are not saved into a view. A view names what to
list, the way `view add` spells it with a folder or `--tag`; how it is being
looked at right now — the sort, hidden files, `/`, these — is kept by the tab
that is looking, so opening a view always shows all of it.

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
verb that drops one, and it still asks — and a run that finds another writer
holding the index lock steps aside rather than making the window wait.

Every tag write keeps what the file carries the same way: `tag add` on a file
whose xattr holds a tag its row lacks takes that tag into the row rather than
writing over it. A word in the xattr that is not a tag is left alone by every
verb but `tag clear`, the one allowed to drop it.

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

A script of your own goes in `~/.config/omafiles/scripts/`, beside the views,
executable and named for what it does. The row menu lists it under `Run a
script`, over the row or the marked set, and runs it with those paths as its
arguments and their folder as its working directory:

```bash
cat > ~/.config/omafiles/scripts/shrink <<'EOF'
#!/bin/sh
for f; do magick "$f" -resize 50% "${f%.*}-small.${f##*.}"; done
EOF
chmod +x ~/.config/omafiles/scripts/shrink
```

Every verb is in [the command line](docs/cli.md).

The list is the folder — a folder shut, one that is missing, and one that is
genuinely empty are three different answers on the status line. Fifos and dangling
symlinks are listed; a fifo is not handed to the desktop. `mv` and `cp` refuse to
write over something already at the destination.

Colours come from the current omarchy theme and corner rounding from hyprland's
`looknfeel.lua`, both live.

Dragging over the list or the grid draws a band and marks every row it
touches. It starts anywhere but a row's icon and name, which are kept for the
row, as in Dolphin: past the name, beside a tile, or below the last row. A plain
drag replaces the marks, `Shift` adds to them and `Ctrl` turns each row it
crosses over — Dolphin's and Qt's convention (GTK's `Ctrl` only adds). Held at
the top or bottom edge, the list scrolls; `Esc` before letting go puts the marks
back as they were.

## Requirements

- Qt 6: `qt6-base`, `qt6-declarative`
- `xdg-utils` for `omafiles open`
- `glib2` for `omafiles trash`
- `xdg-desktop-portal`
- [mise](https://mise.jdx.dev/) for the tasks above

`docs/img` is regenerated from the studio itself with `mise run shots`.
