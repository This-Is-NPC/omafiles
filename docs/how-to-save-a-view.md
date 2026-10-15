# How to save a view

**The question:** How do I save a folder, tag search, or tag combination as a shortcut? This page is enough on its own.

## Before you start

A saved view is a shortcut to a folder, a set of tags, or a query. Open it from the **v** (saved views) panel and it takes you straight there. A view remembers what to list; how you are looking at it right now (sort order, filters, hidden files) is saved with the tab, not with the view.

## In the window

Save the current folder as a view:
1. Open the row menu with `m`, right-click or touch/pen hold
2. Choose **Save this as a view**
3. Type a name for the view
4. Press Enter

Or use the command palette:
1. Press `:` or `Ctrl+P`
2. Type `Save` and press Enter on **Save as a view**
3. Type a name and press Enter

Open a saved view:
1. Press `v` to open the saved views panel
2. Use `j`/`k` (or arrow keys) to move to the view you want
3. Press `l`, Enter or Space to open it

## On the command line

Save a folder as a view:
```sh
omafiles view add --name "scans" -- ~/scans
```

Save a tag combination as a view:
```sh
omafiles view add --name "receipts" --tag finance --tag invoice
```

Save a tag as a view:
```sh
omafiles view add --name "work" --tag work
```

List all your views:
```sh
omafiles view
```

Output is view name and the command that opens it, one per line.

## What can go wrong

- **The view disappeared after using it**: Views are persistent. They are saved in a file in your configuration directory. Check `omafiles view` to list them all.
- **Opening a view shows no files**: The folder may be empty, or all files may have been untagged or deleted. The view definition is still there and will show new matching files when you add them.
- **`store: cannot lock INDEX: another writer holds it`**: A view write is being processed. Wait a moment and try again.

## See also

[Editing tags in the window](tagging.md) explains how to edit tags to refine the files a view shows.
