# omafiles Design

omafiles is a file tagger and browser: CLI, window and helper processes that read, write and search a text index over the files on one or more local filesystems.

## 1. What omafiles is and is not

omafiles tags files and lists them by tag. A tag is a word stored in the file's extended attributes and in a text index. The index is a single `jsonl` file one row per file, so many files can carry the same tag and be listed together. Nothing is moved, renamed or wrapped — the filesystem stays the filesystem. Files opened through the window use your system's configured applications.

Tags persist across copies and moves of the file when the filesystem and the tool doing the copying both support extended attributes. Tags can be recovered after external deletion or rearrangement by scanning the file's own copy (xattr) and merging it into the index. A root is a mount point or attachment point: declared roots let the index survive remounting the same storage elsewhere. A view is a saved folder, tag query or combination of the two.

The window is a standalone Qt Quick application that browses folders, tags and views. The CLI is for scripting and automation. A helper for previews (images, PDFs, text, audio, video) runs on demand in a separate process. Helpers for archive compression/extraction and removable-device enumeration also run as needed. The window is the default file manager if you ask it to be, and implements the portal file chooser for other applications.

## 2. The model

### Files and tags

A file can carry any number of tags. A tag is a word: lower-case letters, digits and hyphens, 64 characters at most, no leading hyphen. The same tag and applied to different files lists all those files together. A file moved, copied or restored carries its tags in extended attributes (xattr) and the index row. Tags are stored in the file's `user.omafiles.tags` xattr as a space-separated list, and are always sorted and without duplicates both in the index row and in the xattr. See [tagging.md](tagging.md) for how tags are edited in the window.

### The index

The index is one `jsonl` file at `$XDG_DATA_HOME/omafiles/index.jsonl` (or `~/.local/share/omafiles/index.jsonl` if `XDG_DATA_HOME` is unset). Each line is one JSON object representing one file: its absolute path, the tags on it, and the Unix timestamp of the last tag write. A writer holds `index.jsonl.lock` from before it reads the file until after it commits, following git's lockfile discipline. Readers never wait or take the lock. A second writer waits up to five seconds, then exits with an error. The lock also prevents stale writes: a writer that waited must verify the index hasn't changed before applying its changes.

Paths in the index are normally stored as absolute paths. For declared roots, paths are stored as the root ID and the path relative to it, so the index survives remounting that storage elsewhere.

### Roots

A root is a mount point, attachment point, or any path declared as a base. Each root has an ID, an absolute path, and an optional check file to verify the storage is mounted. When the check file exists at the declared location, the root is considered here. Roots let tagged files on removable drives remain indexed even after unmounting and remounting elsewhere.

Roots are stored in `$XDG_CONFIG_HOME/omafiles/roots.jsonl`. Only the root ID, path and check file matter for index survival. Multiple roots can point to different storage, and the same file can be tagged regardless of which root its storage is mounted under.

### Views

A view is a saved folder, tag query, or combination of both. `omafiles view add --name name` saves the current folder or tag criteria. Views are listed in the window under `v` and in the CLI with `omafiles view`. Views are stored in `$XDG_CONFIG_HOME/omafiles/views.jsonl`.

The window's presentation of a view — sort order, filters, hidden files — is kept per-tab and not saved with the view itself. Opening a view in the window always shows all rows, then the window's current filters narrow them.

## 3. Architecture

The project is split into a core library, a CLI binary, a window, and three helper processes.

### Core library (`src/core`)

The core is a Qt C++ library with no GUI. It handles the index (`Store` and `Record`), extended attributes (`Xattr`), declared roots (`Roots`), saved views (`View`), the tag vocabulary and verbs (`Tag`), filesystem operations (`Paths`, `Glob`), recent files (`Recent`), opening files (`Open`), and trash operations (`Trash`). It defines how paths resolve, how tags are normalized, how the index is locked and read, and how xattr is read and written. This is where the contracts for interruption, error recovery and rollback live.

### CLI (`src/cli`)

The CLI is a command-line tool that invokes core library verbs: `ls`, `tags`, `tag add`, `tag rm`, `tag rename`, `tag merge`, `tag cp`, `tag clear`, `export`, `import`, `batch`, `prune`, `reindex`, `sweep`, `verify`, `chmod`, `mv`, `cp`, `rm`, `trash`, `open`, `has`, `json`, `view`, `roots`, `recent`, `completions`. The CLI owns the semantics of flags, arguments and output formatting. It can run entirely offline and synchronously (no GUI, no helpers). See [cli.md](cli.md) for the complete CLI reference.

### Studio (`src/studio`)

The studio is a Qt Quick window over the core library. It implements folder browsing, tag editing, thumbnail display, preview, trash, undo, desktop file manager integration, file operations with conflict resolution, and the portal file chooser. It runs the CLI and core verbs asynchronously in background tasks. The window can run without the helpers; helpers are started on demand when their features are used. Large folders are loaded in the background with names cached and metadata read on demand. See [browsing.md](browsing.md) for how large folder browsing works, [thumbnails.md](thumbnails.md) for thumbnail caching, [drag-and-drop.md](drag-and-drop.md) for file operations, and [conflicts.md](conflicts.md) for handling conflicting paths during copy and move.

### Helpers

Three helper processes run separately when needed. Each is a small, focused CLI tool that communicates with the window over stdin/stdout.

**Preview** (`src/preview`) decodes images, PDFs, archives (as text listings), text, audio and video. The window sends a file path, receives decoded packets over a framed binary protocol, and paints them. Preview work is CPU-intensive and memory-bounded; the helper can be killed and restarted without affecting the index. See [preview.md](preview.md) for the preview feature.

**Archive** (`src/archive`) lists, extracts and compresses archive files. The window sends paths and a destination, receives progress and entry names over newline-delimited JSON, and shows progress. The helper owns the libarchive integration. See [archives.md](archives.md) for the archive feature.

**Devices** (`src/devices`) enumerates removable drives and phones using GLib and UDisks2. The window sends device enumeration requests, receives JSON describing available devices, and shows them in a list. Mounting is handled by gvfs and system tools. See [devices.md](devices.md) for the devices feature.

Helpers are separate processes so that expensive or unsafe work (untrusted file parsing, privilege-requiring operations) cannot crash the window or stall the GUI. Each helper can use one allowed CPU, inherit CPU affinity and cgroup membership, and is memory-bounded.

## 4. State on disk

### Index and store

The index is `$XDG_DATA_HOME/omafiles/index.jsonl`. Each line is one JSON row: a file's absolute path (or root+relative for declared roots), its tags (sorted), and when the tags last changed. The writer locks `index.jsonl.lock` to prevent lost updates. A reader never locks or waits. The lock guarantees atomicity: readers see the old index or the new one, never a half-written file.

Declarations and views are separate `jsonl` files: `roots.jsonl` and `views.jsonl` in `$XDG_CONFIG_HOME/omafiles`, not the index's directory. These are read and written the same way as the index.

### Thumbnail cache

Thumbnails are stored in `$XDG_CACHE_HOME/thumbnails` (or `~/.cache/thumbnails` if unset), following the freedesktop Thumbnail Managing Standard. The window requests thumbnails from the system's registered thumbnailers and caches what they produce. The cache is not cleaned by omafiles; the system's cache-management tools handle expiration.

### Roots

Each row in `roots.jsonl` has an ID, the absolute path of the mounted storage, and optionally a check file (a filename to look for to verify the storage is mounted). It is the index rows under that root, not the root's own row, that are stored as the root ID plus a path relative to it; that is what lets the index survive the same storage being remounted elsewhere.

### Trash

Trash locations are managed by the freedesktop Trash specification. The user's trash is `$XDG_DATA_HOME/Trash`; removable storage may have `.Trash/<uid>` and `.Trash-<uid>` directories. Moving a file into Trash is done by the CLI's `trash` verb, which runs `gio trash`; the window reaches it the same way. Restoring from Trash and emptying it are done by omafiles itself: a move with `renameat2` back to the original location, or a direct removal, with no call to `gio`. The window browses trash and can restore or permanently delete entries. Tags in the xattr travel with restored files; index rows for trashed files are moved back with them. See [trash.md](trash.md) for the Trash feature and [undo.md](undo.md) for how Undo interacts with Trash.

### What is never written

omafiles never writes files it doesn't own. It does not write to:
- Configuration files beyond `$XDG_CONFIG_HOME/omafiles/scripts` (user scripts) and `$XDG_STATE_HOME/omafiles/studio.conf` (window state: sort order, column widths, recent folders).
- Thumbnail caches (thumbnailers write those).
- Desktop or system files outside the user's data home.
- The target file's contents. Tagging a file writes only its `user.omafiles.tags` extended attribute, never the file's bytes.

The index, roots and views are the only files omafiles maintains, alongside the `user.omafiles.tags` xattr it sets directly on tagged files. Desktop entries and recent-files lists are read without modification unless the user explicitly asks.

## 5. Protocols

### Helper stdin/stdout

**Archive helper**: Receives one JSON object on stdin containing `paths` (array), `destination` (string), `format` (string), `extracting` (boolean), and `stage` (string). Sends newline-delimited JSON messages with `{"entries": count, "processed": count}` during work, then `{"entry": "name"}` for each result, and finally `{"ready": true}` or `{"error": "reason"}` on completion or failure.

**Devices helper**: Receives enumeration requests as JSON on stdin. Sends newline-delimited JSON responses: a `devices` array of rows with an id, name, path, whether it is a phone, whether it is mounted, and which of mount, unmount and eject apply; and, for a requested action, `action`, `finished`, `ok` and `reason`.

**Preview helper**: Receives a file path as a command-line argument. Sends length-prefixed binary packets over stdout using Qt's QDataStream format. Each packet contains a kind (text, image, error, etc.), text content, playback state (for audio/video), loading state, and image pixels (ARGB32 format). Packets are framed with a 4-byte big-endian length prefix.

### D-Bus FileManager1

The studio implements `org.freedesktop.FileManager1` on the session bus. It listens for `ShowFolders`, `ShowItems`, and `ShowItemProperties` calls from other applications (e.g. web browsers' "Show in folder"). A user D-Bus service file points at the studio executable. This lets other applications open folders and files in omafiles without knowing its command-line arguments.

The studio also implements the file chooser portal (`org.freedesktop.impl.portal.FileChooser`) for applications using the desktop portal to ask for file open/save. The portal service is activated by D-Bus and runs the studio with `--portal`.

### CLI exit codes

- **0**: The run did what was asked.
- **1**: A usage error, a refusal by the program, or an answer the program could not finish (e.g. a write error, a file the system will not describe).
- **2**: A path that is not there (`ENOENT`). An operand, a base directory for a `[dir]` argument, or the target of a symlink.
- **3**: The index could not be read or written, or a declared root could not be read.

A failed write commits what it could and reports the failure. A write to stdout that fails (full disk, broken pipe) is `<verb>: write error` and exit 1, after the work as well as before it.

## 6. Desktop integration

### Default file manager

Run `omafiles` from the command palette in the window (`Ctrl+P` or `:`), then choose **Make omafiles the default file manager**. This installs a desktop entry and a D-Bus FileManager1 service in `$XDG_DATA_HOME` and calls `xdg-mime default omafiles.desktop inode/directory`. The window keeps answering input while this runs in the background. Existing registrations are saved in `.omafiles-backup` files so they can be restored.

The desktop entry uses the absolute path to the studio executable. The D-Bus service activates the studio with that same executable. Both are installed by `mise run install` under `~/.local` or a custom prefix. See [desktop.md](desktop.md) for the complete desktop integration setup.

### Portal file chooser

The studio implements the `org.freedesktop.impl.portal.FileChooser` portal for applications using the desktop portal to request file open/save dialogs. Install the portal backend with `mise run install`, then edit your portal configuration to choose omafiles. The configuration file is in `${XDG_CONFIG_HOME:-$HOME/.config}/xdg-desktop-portal/portals.conf` or a desktop-specific variant. Add:

```ini
[preferred]
org.freedesktop.impl.portal.FileChooser=omafiles
```

Then restart the portal: `systemctl --user restart xdg-desktop-portal.service`. See [file-chooser.md](file-chooser.md) for using the portal from applications.

### Desktop entry and icon

The desktop entry is `pkgbuild/omafiles.desktop` (installed to `$XDG_DATA_HOME/applications` or a system path). The icon is `pkgbuild/omafiles.svg`. Both are installed by `mise run install`.

## 7. Building and development

### Development environment

The project uses [mise](https://mise.jdx.dev/) to define and run build tasks. All tasks are in `mise.toml` and use `scripts/` for their implementations.

### Setup

```bash
mise install usage@6.6.0    # Install the usage CLI for help text generation
mise run deps               # Check Qt 6, qmake, and other build requirements
```

### Building

```bash
mise run build              # Build the CLI and studio binaries to build/bin
mise run studio             # Build and open the studio window
```

### Installation

```bash
mise run install            # Build and install to ~/.local (user-wide)
mise run install PREFIX=/usr/local  # Install to /usr/local (system-wide with sudo)
mise run uninstall          # Remove ~/.local installations
```

User install (`~/.local`) requires no `sudo`. System install (`PREFIX=/usr/local`) builds as the current user and sudos only the final copies. The tag index is left in place either way.

### Verification and testing

```bash
mise run test               # Build and run unit tests and CLI e2e tests
mise run lint               # Lint QML with all warnings fatal
mise run check              # Run the full gate: docs, QML, tests, and CLI verification
mise run shots              # Regenerate docs/img screenshots from the studio
```

Use `mise run check` for the full gate.

Git hooks can be installed with `mise run hooks:install`. The pre-commit hook runs the gate and refuses a commit that fails it. The pre-push hook runs `scripts/local-check`: a red gate refuses the push, and a green one posts the `local-check` commit status on GitHub.

### Usage documentation

The CLI's help text and documentation are generated from `omafiles.usage.kdl` using the `usage` tool. Edit that file, then run:

```bash
mise run usage:gen          # Regenerate docs/cli.md from the usage spec
```

## 8. Performance and what was measured

The project includes a comprehensive benchmark suite that measures the studio's performance in isolated Docker containers. Results are evidence for this machine and method, not general claims about file managers.

Measured metrics:
- **Startup latency**: Time from launch to the first frame.
- **Filter latency**: Time to type a filter and see results.
- **Scroll latency**: Time for a large scroll to settle.
- **CPU idle**: CPU activity after the folder has settled.
- **Memory**: Peak resident set size during startup and interaction.

The benchmark runs the studio with fixtures of 1,000, 10,000, 100,000, and 1,000,000 empty files. Each fixture is run twice (once for latency, once for memory with independent sampling) across two resource configurations (1 CPU / 512 MiB, and 2 CPUs / 1 GiB). Results are recorded as JSON with per-run evidence and a Markdown table.

See [docs/benchmark.md](benchmark.md) for the method and requirements. The other `benchmark-*.md` files contain detailed investigations of specific components:
- [benchmark-startup.md](benchmark-startup.md) — Launch time breakdown.
- [benchmark-filter.md](benchmark-filter.md) — Filtering large folders.
- [benchmark-directory-parallel.md](benchmark-directory-parallel.md) — Name sorting and chunked workers.
- [benchmark-memory.md](benchmark-memory.md) — Memory use and paging.
- [benchmark-preview.md](benchmark-preview.md) — Preview helper resource limits.
- [benchmark-observer-overhead.md](benchmark-observer-overhead.md) — Observer instrumentation impact.

## 9. Checks and how to run them

The project includes unit tests, QML linting, CLI verification, and link checking. All checks are run by the full gate.

### Running checks

```bash
mise run check              # Full gate: docs, QML, tests, CLI verification
mise run test               # Unit tests and CLI e2e tests only
mise run lint               # QML linting (all warnings fatal)
mise run shots              # Regenerate docs/img from the studio
scripts/link-check.py       # Check links in .md files
```

The full gate is run before each commit if hooks are installed (`mise run hooks:install`). It regenerates the screenshots into a scratch directory and fails if they differ from the committed `docs/img`, rather than overwriting them; use `mise run shots` to update the committed copies.

### Test suites

**Studio**: Qt Quick tests in `tests/tst_studio.cpp`. They use `QTRY_*` macros with a 30-second timeout for asynchronous work (folder loading, file operations, thumbnail decode, preview streaming, D-Bus handshakes). Tests wait for completed work, not elapsed time.

**CLI**: End-to-end tests in `tests/test_cli.py`, run against the built `omafiles` binary. They exercise verbs, flags, error handling, and file operations.

**Store**: Index read/write tests in `tests/tst_store.cpp`, including lock contention and concurrent writes.

**Other Python tests**: Cover the benchmark runner, incremental builds, the link checker itself, portal installation, the test harness, usage-doc generation, and the write-scope script.

The test environment is disposable: tests use fresh HOME, XDG directories, private D-Bus, and isolated /tmp. See [docs/testing.md](testing.md) for the complete testing strategy, including how tests isolate thumbnail caches and MIME data, how they handle asynchronous operations, and the resource budgets for helpers.

### Documentation checks

The `check` gate verifies that:
- This file (`docs/design.md`) and other documentation are current.
- All links in `.md` files are valid (run `scripts/link-check.py` manually).
- QML is clean and linted.
- Tests pass.

Documentation links are checked by `scripts/link-check.py`. It validates that all file references in `.md` files point to existing files and that section references (e.g. `docs/design.md §4`) refer to existing sections.
