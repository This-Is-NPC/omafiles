# How to mount a device

**The question:** How do I use removable drives and phones in omafiles? This page is enough on its own.

## Before you start

Removable storage (external drives, USB sticks, SD cards, phones) appears in the devices list. Mounting makes the device readable and browseable. Once you declare a device as a root, omafiles remembers its tags even when it is unplugged.

## In the window

Open the devices panel:
1. Press `d` to focus the devices list
   - The list updates automatically when you plug in a new device
   - An internal drive is not listed
2. Use `j`/`k` (or arrow keys) to move to the device you want
3. Press Enter, Space or `l` to open the device in a folder tab
   - The device is mounted automatically if necessary
   - A status message appears while mounting

Manage a device:
1. Open the devices list with `d`
2. Press `m`, right-click or touch/pen hold on the device
3. Choose an action:
   - **Mount** — mount it without opening a tab
   - **Unmount** — release it (safe to unplug)
   - **Eject safely** — stop or eject the drive
   - **Cancel device operation** — stop a current operation like mounting

## Declaring a root so tags follow the device

When a device moves to a different mount point, its tags are lost if you have not declared it as a root. To keep tags:

1. Mount the device
2. Open a terminal and declare it:
   ```sh
   omafiles roots add --id myexternal --check .device-id -- /path/to/mount
   ```
   Where `/path/to/mount` is the current mount point, and `.device-id` is a file name the device always has (or create one yourself)
3. Open the device in omafiles; tags are now saved
4. When the device remounts elsewhere, it is still recognized and tags stay

List your declared roots:
```sh
omafiles roots ls
```

## What can go wrong

- **"Unmount: device is busy"**: The device has open files or folders. Close all tabs showing the device, and any open file dialogs choosing from it, then try again.
- **"Authorization required"**: Your user account needs permission to mount this device. Ask your system administrator, or the device may need special handling (like unlocking an encrypted drive).
- **The device appears but has no contents**: The device may be empty, or the filesystem is not supported by your system. Open a terminal and check: `lsblk -f` shows devices and filesystem types. If it says `vfat` or `ext4` but the folder is empty, the device may be corrupted; try a file recovery tool.
- **The phone's files do not appear**: Install the appropriate backend: `gvfs-mtp` for MTP, `gvfs-gphoto2` for cameras, `gvfs-afc` for iPhones. Unlock the phone and permit file access when prompted on the device.
- **A declared root with `--check` says "is not there"**: The check file does not exist at the current mount point, or the path is wrong. Create the file or update the declaration.

## See also

[Removable drives and phones](devices.md) explains GIO/GVfs integration, mount locations, absent roots and storage checks.
