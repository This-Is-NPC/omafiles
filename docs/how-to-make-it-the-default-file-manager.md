# How to make it the default file manager

**The question:** How do I make omafiles the default file manager for opening folders? This page is enough on its own.

## Before you start

Making omafiles the default file manager means that double-clicking a folder in your file browser, or browser "Show in folder" links, will open the folder in omafiles. The window remains in the foreground when you use it this way.

## In the window

1. Press `:` or `Ctrl+P` to open the command palette
2. Type `Make omafiles` and press Enter on **Make omafiles the default file manager**
3. The action runs in the background; the window stays responsive
4. A sheet appears showing what was installed
5. Read the undo instructions in the sheet, in case you need to restore the previous file manager
6. Press **Close** or Escape to dismiss it

The sheet shows:
- Where the desktop entry was installed
- Where the D-Bus service was installed
- The MIME type that was set
- Commands to restore the previous setup if you change your mind

## Restoring your previous file manager

The action saves backups of your previous setup with `.omafiles-backup` suffix. To restore them:

1. Open a terminal
2. Follow the `mv` commands shown in the result sheet, or manually restore the files the sheet named

Or choose another file manager:
```sh
xdg-mime default OTHER.desktop inode/directory
```

Replace `OTHER.desktop` with that manager's desktop entry name.

## What can go wrong

- **`Cannot make this executable the default: its path must be absolute and contain no reserved Desktop Entry characters`**: The window's executable path has spaces, quotes, control characters, `%`, `=` or other reserved characters that desktop launchers cannot handle. Reinstall omafiles in a location without those characters ([Installation](design.md#installation)), then start the installed `omafiles-studio` from `PATH` and retry.
- **Cannot write a registration file**: You have no permission to write to your desktop data directory. Check the permissions on `$XDG_DATA_HOME` (usually `~/.local/share`).
- **Other applications still use the old file manager**: Permissions are set by the desktop system, not by the application. The desktop sees omafiles as the default, but running copies of other file managers are still open. Close them first, and any new folder opens will use omafiles.
- **`MIME selection failed; registrations above remain installed.`**: The MIME tool is not available or failed. The desktop entry and D-Bus service registrations remain installed. You can still use the file manager, but "Show in folder" and file-browser folder doubles may not activate it until the MIME tool works again.

## See also

[Desktop integration](desktop.md) explains activation tokens, D-Bus services, and what happens when you open the application from browser links.
