# How to tag files

**The question:** How do I add tags to files? This page covers both the CLI and the window.

## Before you start

Tags identify files with words, not by moving them. A tag is lower case letters, digits and hyphens, up to 64 characters. Tags are stored in the index and mirrored in each file's own extended attributes, so they follow the file even if something else moves, copies or restores it.

## In the window

1. Select a file or folder
2. Press `t` to enter the tag panel
3. Press `a` to add a tag, type the tag name, and press Enter
   - The tag appears beside the file name
   - If you type a new tag for the first time, it is created
   - Existing tag names appear as suggestions; press Up/Down to choose one, Tab to complete it
4. To remove a tag, press `d` and choose the tag from the list, then Enter
5. To rename a tag across all selected files, press `r`, type the old name, press Enter, then type the new name and Enter
6. Press `h`, Escape or Left to return to the file list

Adding tags to many files at once: Select them first (Space marks a row, `J`/`K` mark and step, `*` marks by pattern, Shift+click marks a range), then press `t` and add the tag to the marked set.

## On the command line

Tag a single file:
```sh
omafiles tag add --tag work -- ~/docs/invoice.pdf
```

Tag multiple files in one command:
```sh
omafiles tag add --tag project-x -- ~/docs/file1.txt ~/docs/file2.txt
```

Tag multiple files with multiple tags:
```sh
omafiles tag add --tag work --tag urgent -- ~/docs/file1.txt ~/docs/file2.txt
```

Remove a tag:
```sh
omafiles tag rm --tag draft -- ~/docs/invoice.pdf
```

Rename a tag everywhere it appears:
```sh
omafiles tag merge old new
```

See all tags in use:
```sh
omafiles tags
```

## What can go wrong

- **Tag has uppercase letters or spaces**: Tags are lower case. Type `Work` and it becomes `work`; type `my tag` and it is refused as invalid. The window displays what is stored and completes from existing tags.
- **The file is on a read-only filesystem or in a read-only directory**: The tag is still written to the index, and a note appears in the status line. The index is the record; the file's copy is optional.
- **Tags remain in the file after renaming a tag from the command line**: CLI `tag merge` renames a tag everywhere in the index. Use it instead of renaming the tag in individual file operations. The file's own tag copy is a secondary record and may take time to update.
- **`store: cannot lock INDEX: another writer holds it`**: The index is locked while another command runs. Wait a moment and try again. Only one `omafiles` command can write the index at a time.

## See also

[Editing tags in the window](tagging.md) explains tab completion, selected-file changes and receipt details.
