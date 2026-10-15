# How to pick files for other apps

**The question:** How do I use omafiles as the file picker when other applications ask to open or save a file? This page is enough on its own.

## Before you start

When you click "Open File" or "Save As" in another application, it opens a file chooser. By default, your system picks one. You can choose omafiles instead. The file chooser is a separate window from the main file manager.

## Set up the file chooser

1. Build and install omafiles ([Installation](design.md#installation))
2. Open a terminal and edit your portal configuration: `${XDG_CONFIG_HOME:-$HOME/.config}/xdg-desktop-portal/portals.conf`
   - If the file does not exist, copy the system one first: `cp /etc/xdg-desktop-portal/portals.conf ~/.config/xdg-desktop-portal/portals.conf`
3. Add or update this line in the `[preferred]` section:
   ```
   org.freedesktop.impl.portal.FileChooser=omafiles
   ```
4. Save the file
5. Restart the portal service: `systemctl --user restart xdg-desktop-portal.service`
6. Close and reopen the application

## Using the file chooser

When an application asks you to open or save a file:

Navigate:
- Click a folder to enter it, or press Enter on a folder
- Click `..` or press the up button to go to the parent folder
- Use `Ctrl+L` to edit the path directly
- Press `Ctrl+H` to show hidden files

Choose files:
- Click a file to select it
- For multiple files, click the file, then Ctrl+click other files, or press Space to toggle each one
- For a "Save" dialog, a text field lets you enter or edit the filename
- For a "Save as existing file" dialog, choose the file and press Enter to confirm overwriting it

Filter:
- Use `Ctrl+F` to search by name within the folder
- The filter dropdown (if the application offers filters) shows file types the application accepts

Confirm:
- Press Enter or click the button to accept your choice

Cancel:
- Press Escape or click Cancel

## What can go wrong

- **The file chooser does not appear**: The backend may not be installed. Install it ([Installation](design.md#installation)), and restart the portal service with `systemctl --user restart xdg-desktop-portal.service`.
- **The chooser opens behind the application, with no error shown**: Without a valid parent window identifier, the chooser opens independently rather than refusing. Move the application window and look for the chooser behind it. This does not prevent choosing files; it just makes the window harder to find.
- **The file I choose does not appear in the application**: The application may be filtering by file type. The chooser dims files outside the filter rather than hiding them, so you can still choose one; the dimming is the only signal, there is no warning dialog. Choose the matching file type in the filter dropdown instead, and try again.

## See also

[File chooser portal](file-chooser.md) explains the setup, protocol, keyboard controls and parenting on Wayland and X11.
