# How to find files

**The question:** How do I search for files by name or by tag? This page is enough on its own.

## Before you start

The window lets you narrow what you see right now, and find files across your entire declared storage. The CLI can list by tag or walk the disk.

## In the window

Press `/` to filter the current folder by name:
1. Type part of the name you are looking for, like `invoice`
2. The list updates as you type, showing only matching names
3. Matching text is highlighted
4. Press Escape to clear the filter and see everything again

Press `f` to find by name across your entire storage:
1. A find panel opens instead of a folder
2. Type a pattern like `*.pdf` or `invoice-??.*`
3. The search matches names in all folders in your declared roots
4. Patterns are case-insensitive
5. Press Escape or `h` to return to the file list

Filter by properties with `F`:
1. A filter panel opens above the list
2. Choose Type (folders or files), Tag (files carrying a specific tag), Modified (last day/week/month/year), or all untagged files
3. Each filter is a chip you can click to remove
4. Filters apply to what is listed now; no new folders are searched
5. Click Clear to remove all filters at once

## On the command line

Find files by tag:
```sh
omafiles ls --tag work ~/docs
```

List files with multiple tags (must have all of them):
```sh
omafiles ls --tag work --tag urgent ~/docs
```

Find files by pattern:
```sh
omafiles ls --name '*.pdf' ~/docs
```

Find untagged files:
```sh
omafiles ls --untagged ~/docs
```

Find untagged files without walking the disk (instant, but a cache from the last sweep). The folder must be inside a declared root that has been swept at least once, or the command refuses:
```sh
omafiles ls --untagged --cached ~/docs
```

Find files modified in the last 7 days:
```sh
omafiles ls --changed-within 7d ~/docs
```

Find files modified in the last 7 days AND tagged with work:
```sh
omafiles ls --changed-within 7d --tag work ~/docs
```

## What can go wrong

- **Finding by name gives no results even though the file is there**: A pattern without `*` or `?` must match the whole file name exactly; `invoice` finds nothing where `invoice*` finds `invoice-01.pdf`. Patterns are fully case-insensitive, matching regardless of case anywhere in the name. Use `/` to filter what is visible first to check the exact name.
- **"--cached answers out of a root's list of names, and no root is declared"**: `--cached` only reads a sweep's cache, which is kept per declared root. [Declare the folder's root](how-to-mount-a-device.md) with `omafiles roots add`, run `omafiles sweep` once to build the cache, then `--cached` works and reports its age.
- **`root ID is declared and not here`**: A declared root is missing (unplugged or unmounted). The CLI cannot search there. Mount the device or check its status with `omafiles roots ls`, which shows `here` or `away` for each root.

## See also

[Browsing large folders](browsing.md) explains background listing, metadata loading and viewport navigation.
