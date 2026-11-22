# Testing Synchr 0.2

Synchr keeps a drawer on AROS the same as a folder on another computer that
runs Syncthing. Version 0.2 also reaches computers outside the local
network, through Syncthing's public servers and relays. This guide says
what you need, what to try and what to send back.

## What you need

- One AROS computer (real hardware is most welcome; QEMU, VirtualBox or
  similar also helps). Pick the archive for it:

  | System | Archive |
  |---|---|
  | AROS x86_64 ABIv11 (an ABIv11 distribution) | `Synchr-0.2-r1-aros-x86_64-abiv11-pc.zip` |
  | AROS x86_64 ABIv1 (mainline, nightly from 2026-11-06 or later) | `Synchr-0.2-r1-aros-x86_64-abiv1-pc.zip` |
  | AROS Raspberry Pi AArch64 ABIv1 (mainline) | `Synchr-0.2-r1-aros-aarch64-abiv1-raspi.zip` |

- A working network on AROS (TCP/IP stack running, a web page or `ping`
  works).
- A second computer, phone or tablet with Syncthing (Windows, macOS,
  Linux, Android). Nothing else has to be installed there.
- Test folders only. Deleting a file on one side deletes it on the other:
  do not point Synchr at drawers you cannot afford to lose.

In a virtual machine, use the rtl8139 network card, not e1000 (see Known
issue below).

## Setting it up

1. Unpack the archive where you want the program (`UnZip` on AROS). It
   makes a drawer `Synchr`.
2. Double-click Synchr. The first start takes a moment; the window then
   shows this computer's Device ID at the top.
3. On the other computer, in Syncthing: Actions, Show ID. In Synchr:
   Add device, give it a one-word name and paste its Device ID. Leave the
   address empty.
4. Syncthing on the other computer asks to add the new device: accept it.
   If it does not ask, add it there by hand (Copy in Synchr puts the ID on
   the clipboard).
5. In Syncthing, share a folder with the AROS device and note its Folder
   ID (the folder's Edit window).
6. In Synchr, select the device, Add folder: the same Folder ID and the
   AROS drawer to keep in step. The drawer is made if it is missing.

The device should show Connected and the folder Up to date.

## What to try

Each of these is useful on its own; do as many as you like.

1. **Same local network.** Both computers on one network. Add, change and
   delete files on each side, including a few larger ones (tens of MB).
   Check that the other side ends up the same.
2. **Different networks.** Put the other computer on another network, for
   example a phone on mobile data with Syncthing, or a computer at a
   friend's place. The device should connect again, usually showing
   "Connected through a relay". Copy files both ways.
3. **Public servers off.** Untick "Use Syncthing's public servers". On a
   different network the device should stay waiting; tick it again and it
   should connect again. On the same network it should keep working.
4. **Changes while apart.** Stop Synchr (menu Project, Stop synchronising
   and quit) or switch off the other computer, change files on both sides,
   then start both again. Changes should arrive; a file changed on both
   sides keeps the other version next to it as `name.sync-conflict-...`.
5. **Start with the system.** Tick "Start with the system", reboot, and
   check that synchronising runs without opening the window. Untick it
   afterwards if you do not want it.
6. **Long run.** Leave it running for an hour or more with occasional
   changes.

## What to send back

For each try: worked or not, and what you saw, through
[Patreon](https://www.patreon.com/reperspective) or
[Ko-fi](https://ko-fi.com/enbewu). Please add:

- the system and version (for example AROS One 2.x, mainline nightly of a
  given date, Raspberry Pi model), and whether it is real hardware or a
  virtual machine (with its network card);
- the other side: operating system and Syncthing version;
- for a fault, the file `Synchr/Data/engine.log` and what you did just
  before it; a screenshot of the window helps.

Please do not send the files `key.pem` and `cert.pem` from `Synchr/Data`:
they are this computer's identity.

## Known issue

On AROS in QEMU with the e1000 network card, the whole system can stop
while Synchr is connecting or transferring. The rtl8139 card works.
Please report it if you see this on a real computer.

## Not in this version

- A folder is shared with one other computer only.
- No IPv6, QUIC or UPnP: when routers stand between the computers, the
  connection goes through a relay.
- File names with letters outside ASCII have not been tested.
- Links and other special files are not synchronised.

## Removing it

Untick "Start with the system", choose Stop synchronising and quit, then
delete the `Synchr` drawer. Your synchronised drawers are not touched.
