# Compressing and extracting

Open the row menu with **m**, right-click or a touch/pen long press, then choose
**Compress…** or **Extract archive…**. The **:** / **Ctrl+P** command palette offers
both actions. Compress acts on the row or its marked set; extract takes one archive.
The same actions are available in list, details, icons and file query results.

The sheet accepts an archive path or an existing destination folder. Compression
supports **zip**, **tar.gz** and **tar.zst**. The format buttons update the extension;
the selected format determines the bytes. Extraction recognizes formats by their
contents using libarchive, including ZIP and compressed tar. Unsupported,
encrypted or damaged contents report the reader's reason. Extraction defaults to
the archive's containing folder. Choose an existing empty folder to keep its
contents together. Destination folders must be accessible without symbolic links.

**Tab** and **Shift+Tab** traverse the destination, format buttons, Cancel and the
confirmation button. **Enter** accepts the field or activates a focused button;
**Space** activates a focused button. Every control can also be clicked or tapped.
The sheet scrolls at small sizes and keeps the focused control visible.

Preparation runs in the `omafiles-archive` helper process with streamed data and
entry/byte progress. It stages privately on the destination filesystem. **Escape**
or **Cancel operation** cancels preparation without publishing any output. During
publication, cancellation stops after the current entry; completed entries remain
available to **Ctrl+Z**. Cancellation does not delete the input archive or sources.

Existing top-level names use the same **Replace**, **Skip**, **Keep both** and
**Apply to all** conflict sheet as file transfers. Folders are separate entries;
nonempty destination folders cannot be replaced or merged. Keep both chooses a
numbered name. Undo removes unchanged outputs and restores replaced bytes and
tags. It refuses and explains if contents, identity or indexed tags have changed.

The created archive receives the union of the selected entries' indexed tags.
Tar archives also carry each entry's portable `user.omafiles.tags` metadata;
extraction imports it into the index. ZIP does not promise portable tag metadata
for its members. An archive's own tags are not applied to every extracted file.
Skipped files retain their existing tags; replacement and Keep both retain the
incoming entry's tags. Sources keep their tags.

Extraction refuses absolute names, parent-directory components, ambiguous
backslashes, duplicate names, escaping or absolute links, and special files.
Contained relative symbolic links are preserved; hard links must name an earlier
regular entry without crossing a symbolic link. No extraction writes through a
symbolic link. Ownership, special permission bits, ACLs, file flags and unrelated
extended attributes are not imported. Ordinary permission bits are retained with
owner read access for files and owner read/write/search access for directories,
so outputs remain accessible for publication, cancellation cleanup and Undo.
Compression destinations inside a selected folder are refused.

The helper uses [libarchive](https://www.libarchive.org/), also used by
[GNOME Autoar](https://gnome.pages.gitlab.gnome.org/gnome-autoar/) and
[Ark](https://github.com/KDE/ark). A separate helper isolates libarchive's
disk APIs, which can change the process directory and umask, from the GUI.
