# How to install and remove omafiles

**The question:** How do I install the released omafiles, and take it off
again? This page is enough on its own.

## Before you start

- An Arch-based system with `pacman`, such as Omarchy, on x86_64.
- `curl` and `sudo`.
- To build from a checkout instead, see [docs/design.md §7](design.md#7-building-and-development).

## Install the latest release

1. Run the installer:

   ```bash
   curl -fsSL https://raw.githubusercontent.com/This-Is-NPC/omafiles/master/install.sh | bash
   ```

   It finds the latest GitHub release, downloads its Arch package and its
   `SHA256SUMS`, refuses the package when the checksum does not match, and
   hands it to `sudo pacman -U`. pacman asks for confirmation and installs the
   dependencies the package declares.

2. Open **omafiles** from the app launcher, or run `omafiles-studio`. The
   command line is `omafiles`.

## Install a given release, or check before installing

- `VERSION=0.2.0` installs that release instead of the latest.
- `DRY_RUN=1` downloads and verifies, then prints the pacman command instead of
  running it.

```bash
curl -fsSL https://raw.githubusercontent.com/This-Is-NPC/omafiles/master/install.sh | VERSION=0.2.0 DRY_RUN=1 bash
```

## Update

Run the installer again. pacman replaces the installed version.

## Remove

```bash
sudo pacman -R omafiles
```

pacman removes every file the package installed. The tag index under
`$XDG_DATA_HOME/omafiles/` and your saved views and roots under
`$XDG_CONFIG_HOME/omafiles/` stay: they are your data.

## What can go wrong

- **"there is no pacman here"**: the installer only installs an Arch package.
- **"there is only an x86_64 package"**: no package is built for other
  architectures.
- **"does not match the release's SHA256SUMS; nothing was installed."**: the
  download was damaged or changed. Run it again; nothing was installed.
- **"has no release yet"**: no release has been published.

## Where to go next

- [Start here](../README.md#start-here) for the first keys.
- [How to make it the default file manager](how-to-make-it-the-default-file-manager.md).
