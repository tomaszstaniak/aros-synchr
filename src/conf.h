/* The engine's configuration file: the devices this one synchronises
 * with and the folders, each folder shared with one device. Plain text,
 * made to be read and changed by hand:
 *
 *     # a comment
 *     name aros                  this device's name, shown on the others
 *
 *     device office              a device and a name for it, used below
 *       id AAAAAAA-BBBBBBB-...   its Device ID
 *       address 192.168.1.10:22000    or: address dynamic
 *
 *     folder documents           a folder ID, the same as on the device
 *       path Work:Documents      the folder here (to the end of the line)
 *       with office              the device it is shared with
 *       rescan 60                seconds between readings (optional)
 *
 * `address dynamic` (or no address line at all) leaves the address to
 * discovery: the device is found on the local network by its own
 * announcements (disco.h), or through Syncthing's global discovery, and
 * reached directly or through a relay (reach.h).
 *
 * Before the first device, how this device can be reached (reach.h);
 * each line is optional, the defaults are Syncthing's:
 *
 *     listen 22000               the TCP port for connections that come in, or off
 *     global-discovery on        announce and look up through global discovery (on/off)
 *     relays on                  be reachable through a relay, and use relays (on/off)
 *     local-announce on          announce on the local network (on/off)
 *     discovery-server URL       another global discovery server (https://...)
 *     relay URL                  a relay of one's own instead of the public ones (relay://...)
 *
 * Indentation and blank lines mean nothing; a line belongs to the
 * `device` or `folder` line before it. Names and folder IDs have no
 * spaces. A file that is not understood is refused as a whole, with the
 * line number: nothing is guessed. */
#ifndef ST_CONF_H
#define ST_CONF_H

#include "devid.h"

#include <stddef.h>

#define CONF_RESCAN_DEFAULT 60
#define CONF_MAX_DEVICES 32
#define CONF_MAX_FOLDERS 100 /* in all; LINK_MAX_FOLDERS of link.h per device */

typedef struct {
    char *name;
    unsigned char id[DEVID_BYTES];
    char *ip; /* numeric IPv4; NULL (and port 0): dynamic, found by local discovery */
    unsigned port;
} conf_device;

typedef struct {
    char *id;
    char *path;
    size_t device; /* index into devices */
    long rescan_seconds;
} conf_folder;

#define CONF_LISTEN_DEFAULT 22000

typedef struct {
    char *name; /* this device; "aros" when not given */
    unsigned listen_port; /* 0: off */
    int global_discovery, relays, local_announce;
    char *discovery_server; /* NULL: Syncthing's */
    char *relay;            /* NULL: the public pool */
    conf_device *devices;
    size_t ndevices;
    conf_folder *folders;
    size_t nfolders;
} conf;

/* Non-zero: a file without any folder is accepted (a program that edits
 * the configuration sets it; the engine does not). */
extern int conf_allow_no_folder;

/* Reads the file. 0 on success; otherwise *c is empty and why says what
 * is wrong and where. */
int conf_load(const char *path, conf *c, char *why, size_t why_len);
/* The same from text in memory (tests). */
int conf_parse(const char *text, conf *c, char *why, size_t why_len);
/* Writes the configuration as a file that conf_load reads back the
 * same: to PATH.new first, which then replaces PATH, so a failure leaves
 * the previous file. 0 or -1 (why says what failed). Names, folder IDs
 * and paths that the format cannot hold (empty, a line break; a space in
 * a name or folder ID) are refused. */
int conf_save(const char *path, const conf *c, char *why, size_t why_len);
void conf_free(conf *c);
/* An empty configuration with the defaults (for a program that makes a
 * new one). */
void conf_init(conf *c);

#endif
