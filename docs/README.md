# Documentation map

This is every page in the documentation, arranged in the order you're likely to meet it.

## The shortest useful path

1. Read [../README.md](../README.md) at the root for what omafiles is and how to install it.
2. Pick a feature from the "Big ideas" section that interests you — each one links to its page.
3. Use the "When you know what you want" table below to find a how-to guide.

## In order you'll meet them

**Getting started:**
- [Design and architecture](design.md) — project goals, design decisions, building and development.
- [Screens and layouts](screens.md) — visual tour of the window: list, details, icons, the tag panel, graph view.

**Features — by topic:**
- [Browsing large folders](browsing.md) — how folder names load without waiting, and refresh when the filesystem changes.
- [Tagging](tagging.md) — adding, removing and renaming tags; tags in xattrs; the tag panel.
- [Quick preview](preview.md) — the `i` key to preview text, images, PDFs and archive contents.
- [File thumbnails](thumbnails.md) — desktop cache thumbnailers, installed thumbnailers, resource limits.
- [Trash](trash.md) — safe deletion to the desktop trash, restore and permanent erase.
- [Undo in the window](undo.md) — Ctrl+Z, operation history, what can be undone.
- [File conflicts](conflicts.md) — Replace, Skip, Keep both, and batch choices when copying or moving.
- [Compressing and extracting](archives.md) — formats, progress, conflicts, portable tags, safe extraction.
- [Removable drives and phones](devices.md) — GIO/GVfs, mount points, declared roots, status.
- [File chooser portal](file-chooser.md) — OpenFile, SaveFile and SaveFiles for other applications.
- [Dragging files](drag-and-drop.md) — folder, tab and pane destinations, modifiers, keyboard equivalents.
- [Desktop integration](desktop.md) — default file manager, browser integration, D-Bus activation.
- [Command line](cli.md) — every verb and option for the CLI.

**How-to guides — by task:**
- [How to install and remove](how-to-install-and-remove.md) — the released package, a given version, updates and removal.
- [How to tag files](how-to-tag-files.md) — adding, organizing and using tags from the window or CLI.
- [How to find files](how-to-find-files.md) — filtering by name, type, tag, date and other criteria.
- [How to save a view](how-to-save-a-view.md) — creating saved views for quick access to folders and queries.
- [How to make it the default file manager](how-to-make-it-the-default-file-manager.md) — choosing omafiles for directory opens.
- [How to pick files for other apps](how-to-pick-files-for-other-apps.md) — using omafiles as the file chooser.
- [How to mount a device](how-to-mount-a-device.md) — connecting and using removable drives and phones.
- [How to compress and extract](how-to-compress-and-extract.md) — creating archives and unpacking them.
- [How to restore from the trash](how-to-restore-from-the-trash.md) — finding and undeleting files.
- [How to undo a change](how-to-undo-a-change.md) — using Ctrl+Z and the history.
- [How to run the benchmark](how-to-run-the-benchmark.md) — performance testing and results.

**Development and testing:**
- [Testing](testing.md) — test suite, test data, running tests.
- [Benchmark](benchmark.md) — detailed methodology, performance characteristics.
- [Benchmark results](benchmark-results.md) — performance numbers for specific operations.
- [Benchmark: startup](benchmark-startup.md) — launch time breakdown.
- [Benchmark: filtering](benchmark-filter.md) — filtering large folders.
- [Benchmark: directory parallelism](benchmark-directory-parallel.md) — name sorting and chunked workers.
- [Benchmark: memory](benchmark-memory.md) — memory use and paging.
- [Benchmark: preview](benchmark-preview.md) — preview helper resource limits.
- [Benchmark: observer overhead](benchmark-observer-overhead.md) — observer instrumentation impact.

## When you know what you want

| You want to… | Start with… |
|---|---|
| Install or remove omafiles | [How to install and remove](how-to-install-and-remove.md) |
| Tag a file with a label | [How to tag files](how-to-tag-files.md) |
| Find files by name or type | [How to find files](how-to-find-files.md) |
| Save a folder or query for quick access | [How to save a view](how-to-save-a-view.md) |
| Make omafiles open when you click a folder | [How to make it the default file manager](how-to-make-it-the-default-file-manager.md) |
| Choose files for another app | [How to pick files for other apps](how-to-pick-files-for-other-apps.md) |
| Use an external drive or phone | [How to mount a device](how-to-mount-a-device.md) |
| Create a zip or tar file | [How to compress and extract](how-to-compress-and-extract.md) |
| Get a file back from trash | [How to restore from the trash](how-to-restore-from-the-trash.md) |
| Undo an accident | [How to undo a change](how-to-undo-a-change.md) |
| Measure performance | [How to run the benchmark](how-to-run-the-benchmark.md) |
| Understand the design | [Design and architecture](design.md) |
| See the visual layouts | [Screens and layouts](screens.md) |
| Contribute to omafiles | [Design and architecture](design.md) |
| Run the test suite | [Testing](testing.md) |

## Conventions

Every page in this documentation follows these rules:

- **Facts only.** Nothing is promised that is not in the code; no features are mentioned that do not exist.
- **Shortest explanation.** A concept is explained once, in the page where it first matters; other pages link to it.
- **One question per page.** Each page answers one clear question.
- **Keyboard first.** Pointer actions are mentioned only to show they exist; keyboard is the primary interface.
- **Links within the project.** Links point to other docs pages, the command-line reference, or code.
- **No marketing.** Pages say what the tool does, not why it is better than something else.
- **Plain English.** Short sentences, active voice, no jargon without explanation.
