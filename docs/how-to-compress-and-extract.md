# How to compress and extract

**The question:** How do I create archives and extract them? This page is enough on its own.

## Before you start

Omafiles can compress files into zip, tar.gz or tar.zst archives, and extract them back. The archive receives all the tags from the files you compressed. When you extract, tags in the archive come back into the index.

## Compress files in the window

Select one or more files:
1. Click or select files with Space (to mark a row), `J`/`K` (to mark and step), or `*` (to mark by pattern)

Open the compression dialog:
1. Press `m` to open the row menu, or right-click/touch hold on a selected file
2. Choose **Compress…**
3. Or use the command palette: press `:` or `Ctrl+P`, type `Compress` and press Enter

Choose format and destination:
1. The sheet shows a destination path or folder
2. Use Tab to move between the path field and format buttons
3. Choose a format: **zip**, **tar.gz** or **tar.zst**
4. Edit the destination path if needed
5. Press Enter or click the confirmation button
6. A status message shows progress
7. The archive appears in the folder when done

Cancel:
- Press Escape to cancel before the archive is published
- During publication, pressing Escape stops after the current file but keeps what was already written

## Extract an archive in the window

Open an archive:
1. Navigate to an archive file (zip, tar.gz, tar.zst)
2. Press `m`, right-click or touch hold
3. Choose **Extract archive…**
4. Or use the command palette: press `:` or `Ctrl+P`, type `Extract` and press Enter

Choose destination:
1. The sheet shows the default destination (the archive's own folder)
2. Click a folder to select a different destination
3. The folder must be accessible without symbolic links
4. Press Enter or click the button to extract
5. A status message shows progress
6. Files appear in the destination when done

Handle conflicts:
- If files with the same name already exist, a conflict sheet appears
- Choose **Replace** (overwrite), **Skip** (keep the existing file), **Keep both** (rename the incoming file), or **Apply to all** (use the same choice for all remaining conflicts)

Cancel:
- Press Escape during extraction to stop after the current file

## What can go wrong

- **"Destination folders must be accessible without symbolic links"**: You chose a destination with a symlink in its path. Navigate through the actual directory instead, or edit the path to remove the symlink.
- **Unsupported, encrypted or damaged contents**: Extraction reports the reader's reason and stops. Try opening the archive with another tool to verify it. A partially extracted archive can be deleted and extraction retried.
- **Undo refuses the extraction**: If contents, identity or indexed tags have changed since extraction, Undo refuses and explains why rather than guessing. Manually delete the extracted files or choose to keep them.
- **Files in the archive have absolute paths or parent-directory components**: The archive is damaged or malicious. Extraction is refused for safety. Extract manually with a trusted tool if you must.
- **ZIP archive does not preserve tags**: ZIP format does not support portable metadata like tags. Only tar archives (tar.gz, tar.zst) carry tags as metadata. Use tar if you need to preserve tags.

## See also

[Compressing and extracting](archives.md) explains formats, conflict handling, portable metadata, extraction safety and Undo integration.
