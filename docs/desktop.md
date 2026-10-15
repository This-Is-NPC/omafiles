# Desktop integration

Open the command palette with **Ctrl+P** or **:**, type **Make omafiles**, and
press **Enter** to choose **Make omafiles the default file manager**. The same
entry can be selected with a pointer. This action installs a user desktop entry
and a user FileManager1 activation service in `XDG_DATA_HOME`, both pointing at
the studio executable you are running, then calls
`xdg-mime default omafiles.desktop inode/directory` and checks the selection.
The work runs in a background task so the window keeps answering input.

The result sheet lists the installed files and undo commands. Close it with
**Escape** or its **Close** button. Existing registrations are preserved in
`.omafiles-backup` files; repeated use retains those originals. Use the shown
`mv` commands to restore them, or the shown `rm` commands for new registrations.
To choose a different directory handler, run
`xdg-mime default OTHER.desktop inode/directory`, replacing `OTHER.desktop`
with that manager's desktop entry. Keeping MIME selection and activation
registration explicit allows the two desktop mechanisms to be restored
independently. If MIME selection fails, the sheet reports the failure and the
registrations that remain installed, including how to undo them.

A user activation service takes precedence over system service registrations,
so browser “Show in folder” can activate omafiles when it is closed. A competing
running FileManager1 owner keeps its name until it exits; close its windows to
hand ownership over. No file chooser or desktop keybinding is changed.

The first launch opens the requested folders/files before QML creates their
views. Subsequent new tabs start in the home directory. Later launches forward
`XDG_ACTIVATION_TOKEN`, or `DESKTOP_STARTUP_ID` when no activation token is set.
D-Bus FileManager1 requests carry their `startupId` to window presentation.
On Wayland the studio provides that value through the Qt 6.11 activation-token
channel before showing and activating the window, then discards the supplied
value. The compositor decides whether the token authorizes focus. This does
not grant focus to callers without a valid token.

Registration paths, their saved originals and every parent directory must be
regular files or real directories as appropriate. The action refuses symbolic
links, including dangling links, and special files before installing either
registration. It also checks the MIME association paths and requires the MIME
tool's temporary `.new` paths to be absent. A refusal names the path; resolve it
explicitly and retry. This keeps registration and MIME writes from changing a
link target, and keeps each saved original suitable for the displayed undo.
Regular registrations and their backups retain their permission bits.

The desktop entry uses `Exec=<absolute studio path> %U` without quotes, so both
standard desktop launchers and generic `xdg-open` can execute it directly.
The action refuses before writing if the executable path contains Desktop Entry
reserved characters, whitespace, control characters, `%` or `=`. These paths
cannot be shared safely between the launchers' different quoting rules. Run
`mise run install`, then start the installed `omafiles-studio` from PATH and
retry. Use an installation prefix whose path has none of those characters.
The D-Bus service also names the absolute studio executable directly.
