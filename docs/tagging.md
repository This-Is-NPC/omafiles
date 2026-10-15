# Editing tags in the window

Select a file or folder, press `t` to enter the tag panel, then `a` to add a tag,
`d` to remove one, or `r` to rename one. This works with an empty vocabulary,
so a new installation can create its first tag. `Enter` and `Space` in the
panel toggle the selected browsing filter; `h`, Left and Escape return to the
file list.

The command palette (`:` or Ctrl+P) offers Add tag, Remove tag and Rename tag.
The row menu offers the same actions. The `+` beside the tag vocabulary opens
Add tag. Clicking or tapping a tag chip in a row or the details panel opens the
editor with that tag selected for removal; choose Rename to replace it instead.
No chip click changes tags until you apply the edit.

An edit opened over a marked row acts on the marked set. An edit over an
unmarked row acts on that row alone. The editor names the file or shows the
number of selected files. Its targets are fixed when it opens. It accepts one
tag at a time; existing tags are suggestions, and a new name is equally valid.
Up and Down choose a suggestion, Tab completes a typed prefix or chosen
suggestion, and Enter accepts a chosen suggestion before applying. In Rename,
Enter in the source field moves to the
replacement field. Tab and Shift+Tab stay inside the editor and reach its mode
buttons, Apply and Close; Space or Enter activates a focused button. Escape
closes without starting an edit.

Add preserves other tags. Remove takes away the named tag. Rename replaces the
source only on selected files that carry it, preserves other tags, and leaves
selected files without the source untouched. Renaming to an existing tag merges
the two on those files. This selected-file operation is different from
`omafiles tag merge old new`, which renames throughout the collection.

The window runs the same asynchronous CLI verbs as a terminal: `tag add`,
`tag rm` and `tag rename old new -- paths…`. The CLI owns tag normalization,
validation, index locking, xattr merging, refusal and commit rules. The window
reloads the index after a result, including an incomplete operation, and shows
the CLI refusal in the editor and status bar. A refusal keeps the entered names
for correction and retry. An empty field starts nothing.

While a write runs, Apply and the edit fields are disabled. Close or Escape can
leave the editor; this does not cancel a write already started. Its receipt
still appears on the status bar, and completion does not take focus away from
what you are doing. The targets remain the ones named when you applied.

Menus and sheets stay inside the client area, including after resizing the
window. Overflowing content scrolls with the mouse wheel or a touch drag.
Menus support Home, End, Page Up and Page Down; the command palette uses
Ctrl+Home and Ctrl+End so the query retains its text-editing keys. Sheets
support unmodified Page Up, Page Down, Home and End when their content overflows. In the
tag editor these keys then move focus to the scrolling area, whose border marks
the keyboard focus; Tab returns to its controls. When the editor fits, Home and
End retain ordinary text-caret navigation. Keyboard navigation keeps the selected menu item, completion
suggestion or focused editor control visible. In the key reference, arrows and
page keys scroll, and Escape closes it.
These rules keep every action reachable when a window or a long receipt leaves
less room for its contents. Long tooltips also accept wheel and touch scrolling;
unmodified arrows, Home, End and page keys on their focused host scroll an overflowing tip
without taking focus away. A tooltip whose text fits leaves those keys to the
window, so hovering a control cannot block file navigation. The decision follows
the current text and available height.

Properties and default-manager receipts use the same bounded scrolling area.
Page Up and Page Down scroll a page; holding Shift extends the receipt's text
selection by a page, including with Ctrl. Ctrl+Home and Ctrl+End reach the text
ends. Other modified navigation keeps the text control's native behavior;
a declined gesture does not scroll an ancestor panel.
Mouse selection and Ctrl+A/Ctrl+C keep the receipt available for copying, while
touch drags scroll its text. Escape closes the dialog and returns to the files.

The details sidebar scrolls as a whole, so its tag heading and `+` stay
reachable in short windows. Entering tag or saved-view navigation reveals that
section's heading and selected row. Moving the selection or resizing the window
keeps them visible. Wheel and touch can browse the other details without changing
the selected file.

Tag completion has its own bounded list. Resizing preserves the chosen
suggestion and keeps it beside the visible text field, rather than scrolling
the caret out of view. Up and Down reveal the chosen suggestion inside that
list. Tab navigation reveals each mode, field and action even with a long
refusal below the fields.

A successful CLI run can still report an xattr warning: the index is the record,
and the file's tag mirror may be unwritable or contain words that require
`tag clear` to repair. The window retains these CLI notes with the successful
receipt, including notes from each file in a transfer. Click or tap the status
receipt, or reach it with Tab and press Enter, to read and copy its full text
in a bounded scrolling dialog. Hovering the receipt also shows the full text.
