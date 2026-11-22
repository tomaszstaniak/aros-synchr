# Synchr

Synchr keeps drawers on an AROS computer the same as folders on your other
computers that run [Syncthing](https://syncthing.net). A file added,
changed or deleted on one side appears on the other.

Synchr speaks Syncthing's own protocol, so nothing else has to be
installed on the other computer. It is not made by the Syncthing project.

- Add a device by its Device ID; the address can stay empty. Synchr finds
  the other computer on the local network or through Syncthing's public
  discovery servers.
- Works across networks: when a router stands between the computers, the
  connection goes through one of Syncthing's public relays. Both sides
  check each other's Device ID and everything between them is encrypted.
- Two-way: new, changed and deleted files go both ways. A file changed on
  both sides keeps the other version next to it as a conflict copy, named
  as Syncthing names it.
- Keeps synchronising after the window is closed, and can start with the
  system without opening the window.

## Installation

Download the archive for your system from the
[releases](https://github.com/tomaszstaniak/aros-synchr/releases), unpack
it anywhere and double-click Synchr. The ReadMe in the drawer walks
through connecting the first device.

| System | Archive |
|---|---|
| ABIv11 distributions, x86_64 PC | `Synchr-<version>-aros-x86_64-abiv11-pc.zip` |
| Mainline AROS x86_64 (ABIv1), a nightly from 2026-11-06 or later | `Synchr-<version>-aros-x86_64-abiv1-pc.zip` |
| Raspberry Pi (AArch64, ABIv1) | `Synchr-<version>-aros-aarch64-abiv1-raspi.zip` |

The archives are not interchangeable. Synchr needs a running TCP/IP stack.

Not there yet: IPv6, QUIC, UPnP port mapping, a folder shared with
several devices, file names outside ASCII.

Known issue: on AROS in QEMU with the e1000 network card, the whole system
can stop while Synchr is connecting or transferring. The rtl8139 card
works. Please report it if you see this on a real computer.

## Support and feedback

Reports, ideas and support for further work are welcome at
[Patreon](https://www.patreon.com/reperspective) and
[Ko-fi](https://ko-fi.com/enbewu).

## For developers

Synchr is a native C client of Syncthing's Block Exchange Protocol; it
does not run Syncthing's Go program. The engine is `stpoc` (packaged as
`SynchrEngine`), the window is `gui/stgui.c` (Zune).

The first stage was a receiver that connects to a Syncthing device over
TLS 1.3, authenticates it by Device ID, receives one folder's complete
index (also when the peer sends it in several parts) and downloads its
files, each verified block by block and written under its final name only
when complete. After a restart unchanged files are recognised by their
hashes and not downloaded again. Files changed locally, also while a
download is running, are kept and reported as conflicts. Built for AROS
x86_64 (mainline ABIv1 and ABIv11) and for the native Raspberry Pi
(aarch64, ABIv1).

The two-way mode (`stpoc twoway`) synchronises one folder with one
Syncthing device in both directions: new and changed files, deletions,
conflicts (both contents are kept, named as Syncthing names conflict
copies) and recovery after an interrupted download or state write.
Restarts neither repeat announcements nor create conflicts.

The engine (`stpoc engine`) is the program that stays running: it keeps
several folders in sync, each with one Syncthing device, from a
configuration file (`src/conf.h`), reconnects when a connection is lost
and stops on Ctrl-C. A folder stays open for the whole run: it is read
at its interval without a connection too, so what changed meanwhile is
recorded and goes out when the device is back. A running engine answers
commands (status of every device and folder, scan now, pause and resume
a folder for this run, stop) at a public ARexx port; `stpoc ctl` sends
them, for example `stpoc ctl PAUSE folder-id` (`src/ctl.h`).

Reaching devices works as in Syncthing (`src/reach.h`): the engine
listens for connections (TCP 22000), belongs to a relay from Syncthing's
public pool and takes sessions through it, announces where it can be
reached to global discovery and on the local network, and finds devices
without a typed address through both, connecting directly or through
their relays (Relay Protocol v1, `src/relay.h`). Each can be switched
off in the configuration. Not there yet: IPv6, QUIC, UPnP port mapping,
a folder shared with several devices, names outside ASCII (they are
rejected).

`scripts/reach-host-test.sh` checks all of this on the host with
unmodified Syncthing devices and Syncthing's own relay and discovery
servers built from source; `scripts/aros-reach.sh` does the same in an
AROS guest and adds a run against the public servers.

See [documentation](docs/README.md) and [contributor instructions](docs/development.md).

### Licence

MIT (see `LICENSE`); two files derived from Syncthing are under the
Mozilla Public License 2.0, and the AROS programs include OpenSSL and
jitterentropy: see `NOTICE`.

### The window and the package

`scripts/build.sh aros gui` builds its
window (`gui/stgui.c`, Zune); `scripts/package.sh` packs the window, the
engine (as `SynchrEngine`), icons drawn by `scripts/make-icons.py`, a
ReadMe for users (`packaging/ReadMe.txt`) and the licence files into
`dist/Synchr-<version>-<target>.zip` for the configured target.

The window makes this device's identity in the drawer `Data` next to it,
shows the Device ID (Copy puts it on the clipboard), takes devices (name,
Device ID; the address may stay empty, the device is then found on the
local network or through global discovery) and folders (folder ID, drawer),
changes them (button or double click), writes `Data/engine.conf`, starts
the engine and shows what it reports through its control port `SYNCHR`.
Folders can be paused, resumed and read again. Closing the window leaves
the engine running; the menu item "Stop synchronising and quit" ends
both. "Use Syncthing's public servers" switches global discovery and
relays on or off. "Start with the system" writes `SYS:WBStartup/Synchr.info` with the
tool type HIDE: at boot the engine starts without a window.
