# File conflicts

Pasting, moving to the other pane and dropping files ask when a destination name
is occupied. Choose **Replace**, **Skip**, **Keep both**, or **Cancel operation**.
**Apply to all** uses the chosen decision for the remaining conflicts in this
operation; the next operation asks again. Cancel leaves completed files in
place, with one Undo entry for the completed portion. Skip leaves both files and
their tags untouched. Keep both names the incoming file `name (1).ext`, then
`name (2).ext`, checking occupied entries including dangling links. Dotfiles
without another dot keep their whole name before the suffix.

Replace moves the old entry aside before creating the incoming one. It never
writes through a destination symlink. The source's tags follow the incoming
file; displaced tags remain with the displaced file. Undo restores both files
and their indexed and on-file tags. If either file changed independently, Undo
refuses and explains why. The operation uses the session journal described in
[undo.md](undo.md). The old entry is held in a hidden `.omafiles-replaced-*`
sibling on the destination filesystem until Undo or session cleanup; a crash
can leave that recovery entry on disk. Do not edit it while Undo is needed.

The CLI offers the same decisions as flags:

| Flag | Occupied destination |
| --- | --- |
| `-f`, `--force` | Replace the entry |
| `-n`, `--no-clobber` | Skip successfully, with no stdout receipt |
| `-i`, `--interactive` | Ask `Replace? [y/N]` on a terminal |
| `--keep-both` | Number the incoming name |
| `-b`, `--backup` | Replace, retaining the old entry as `name.~N~` |
| `-T`, `--no-target-directory` | Use the destination as an exact name |

`-n` before the verb is the global dry-run abbreviation. After `cp` or `mv`,
it means no-clobber; use `--dry-run` anywhere for an unambiguous preview.

Without a decision flag, an occupied destination is refused. The last decision
flag wins. `--backup` cannot accompany skip or keep-both. Backups are numbered;
GNU's optional backup methods, suffix options and environment settings are not
implemented. JSON for a retained backup is an array: the displacement receipt
first, then the incoming transfer receipt. Ordinary transfers keep their
`from`, `to`, `rows` object. A skipped file has no receipt. A dry run checks the
same conflicts and types and reports both planned transfers for a retained
backup without changing either file.

Copy still accepts regular files only. Move still refuses cross-filesystem
folders. Replace refuses self-replacement, incompatible file/folder types and
nonempty folders; choose Keep both or Skip instead. It does not merge folders.
Late unapproved collisions are refused without overwriting them. Pattern rename
keeps its all-or-nothing collision checks and does not accept conflict flags.

Every button supports pointer/touch, Tab and Shift+Tab, visible focus, and
Enter/Space. Escape cancels the operation. The sheet stays within the window;
wheel, touch and Page Up/Down/Home/End scroll its content and focus stays visible.

The choices and batch scope follow
[Nautilus's conflict dialog](https://gitlab.gnome.org/GNOME/nautilus/-/blob/main/src/nautilus-file-conflict-dialog.c)
and [Dolphin's KIO RenameDialog](https://api.kde.org/kio.html).
Numbering follows Nautilus's `name (1).ext` suggestion. The flags follow
[GNU cp](https://www.gnu.org/software/coreutils/manual/html_node/cp-invocation.html),
[GNU mv](https://www.gnu.org/software/coreutils/manual/html_node/mv-invocation.html)
and [GNU numbered backups](https://www.gnu.org/software/coreutils/manual/html_node/Backup-options.html).
The safe default and entry replacement preserve omafiles' no-clobber and
no-writes-through-links rules.
