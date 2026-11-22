Synchr @VERSION@ (@TARGET@)

Keeps drawers on this AROS computer the same as folders on your other
computers that run Syncthing (https://syncthing.net). A file added,
changed or deleted on one side appears on the other.

Synchr speaks Syncthing's own protocol: nothing else has to be installed
on the other computer. It is not made by the Syncthing project.


Installing

Unpack the archive where you want the program (UnZip on AROS, or any
unzip tool on another computer, then copy the drawer over). It makes a
drawer Synchr; nothing is written anywhere else until you ask for it.


Getting started

You need the other computer's Device ID. In Syncthing there: Actions,
Show ID.

1. Double-click Synchr. The first start takes a moment: it makes this
   computer's own Device ID, shown at the top of the window.

2. Add device: give the other computer a name (one word) and paste or
   type its Device ID. Leave the address empty: Synchr finds the other
   computer on the local network, or anywhere through Syncthing's public
   servers, as Syncthing does. A numeric address (for example
   192.168.1.10) and port (22000 unless changed) can be typed instead.

3. On the other computer, Syncthing asks whether to add this new device:
   accept it. (If it does not ask, add it there by hand with the Device ID
   shown in Synchr: Copy puts it on the clipboard.)

4. On the other computer, share a folder with this device. Note the
   folder's ID (in Syncthing: the folder's Edit window, Folder ID).

5. In Synchr, select the device, Add folder: the same folder ID, and the
   drawer to keep in step with it. The drawer is made if it is not there.

The lists show what is happening: Connected, Up to date, files received
and deleted. Pause / resume stops one folder for a while; Read again now
looks for changes at once (otherwise every minute).

Double-click a device or a folder (or use Edit device, Change folder) to
change it.


Finding and reaching the other computer

With "Use Syncthing's public servers" ticked (the default), Synchr works
like Syncthing: it tells Syncthing's global discovery servers where this
computer can be reached, looks up the other computers there, and when
they cannot connect directly (a router in between), the connection goes
through one of Syncthing's public relays. The relay passes the data on
without being able to read it: both computers check each other's Device
ID and encrypt everything between them. The answers of these servers
are not trusted for anything else.

Synchr also waits for connections on TCP port 22000, and announces
itself on the local network, so the other computer can connect to it.

Untick the box to use only the local network and typed addresses.


Closing the window

Closing the window does not stop synchronising. Start Synchr again to see
the window. To stop synchronising: menu Project, Stop synchronising and
quit.

Start with the system (in the window) makes synchronising start when the
computer starts, without opening the window. It puts the icon
Synchr.info in SYS:WBStartup.


What it does not do yet

- A folder is shared with one other computer.
- Connections over IPv6 and QUIC are not made, and routers are not asked
  to open a port (UPnP): without one, connections go through a relay.
- File names with letters outside ASCII have not been tested.
- Links and other special files are not synchronised.

When the same file was changed on both sides, the other side's version is
kept next to it as a conflict copy (name.sync-conflict-...), as Syncthing
does.

Known issue: on AROS in QEMU with the e1000 network card, the whole
system can stop while Synchr is connecting or transferring. The rtl8139
card works. Please report it if you see this on a real computer.


Files

Everything Synchr keeps is in the drawer Data next to it: this computer's
identity (key.pem, cert.pem: keep them, they are its Device ID), the list
of devices and folders (engine.conf), what it knows about each folder
(state-*.txt) and a log (engine.log, at most two files of 400 KB).
SynchrEngine is the part that synchronises; the window starts and stops
it.


Removing it

Switch off Start with the system first (or delete Synchr.info from
SYS:WBStartup), choose Stop synchronising and quit, then delete the
drawer. Your synchronised drawers are not touched.


Problems

When something goes wrong, the file Data/engine.log says what happened;
please send it with a report, and say which system you ran Synchr on.


Support and feedback

Reports, ideas and support for further work are welcome at
https://www.patreon.com/reperspective and https://ko-fi.com/enbewu


Licences

Synchr is MIT-licensed (LICENSE.txt). SynchrEngine contains OpenSSL
(Apache License 2.0) and jitterentropy (BSD-3-Clause); one part derived
from Syncthing is under the Mozilla Public License 2.0. Details in
NOTICE.txt.


Requirements

@REQUIRES@
A running network (TCP/IP stack) and rexxsyslib.library, as installed
with the system.
