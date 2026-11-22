/* Reading the folder: every directory is read once (on AROS with
 * Examine/ExNext, which give size, date and type with the listing, so no
 * name is looked up on its own: a lookup costs about 20 ms on the Pi's
 * FAT32; elsewhere opendir/readdir plus lstat). The result lists what is
 * there and which directories were read completely; a missing entry is a
 * deletion only under a directory read completely
 * (docs/backlog/two-way-sync.md, section 1).
 *
 * Skipped, never reported: .stfolder at the root, this client's temporary
 * and moved-aside files (.stpoc-*), Syncthing's temporary files
 * (.syncthing.*.tmp, ~syncthing~*.tmp). Symbolic links and other special
 * entries are listed as unsupported. */
#ifndef ST_SCAN_H
#define ST_SCAN_H

#include <stddef.h>
#include <stdint.h>

#define SCAN_MARKER ".stfolder"

enum scan_kind { SCAN_FILE, SCAN_DIR, SCAN_OTHER };

typedef struct {
    char *name; /* relative, '/'-separated */
    enum scan_kind kind;
    int64_t size, mtime_s;
    int32_t mtime_ns;
} scan_item;

typedef struct {
    scan_item *items;
    size_t n, cap;
    char **complete; /* directories read to their end ("" is the root) */
    size_t nc, capc;
    char **leftovers; /* this client's .stpoc-<x>.tmp and .stpoc-<x>.old files */
    size_t nl, capl;
    long failed_dirs; /* directories that could not be read completely */
    int marker;       /* .stfolder is at the root */
} scan_result;

/* Reads everything under root. 0, or -1 when the root itself cannot be
 * read (nothing in out may then be used for deletions). */
int scan_folder(const char *root, scan_result *out);
/* The same in steps. begin: NULL when out of memory. step takes up to
 * that many directory entries: 1 more to read, 0 finished (out is
 * complete and sorted), -1 as scan_folder. end releases the run at any
 * time (NULL allowed); out stays with the caller (scan_free). */
typedef struct scan_run scan_run;
scan_run *scan_begin(const char *root, scan_result *out);
int scan_step(scan_run *s, int entries);
void scan_end(scan_run *s);
/* The same facts for one name (after a download, or to recheck one). 0
 * with *it filled (it->name not set), 1 when nothing is there, -1 when it
 * cannot be checked. */
int scan_one(const char *root, const char *name, scan_item *it);
/* The modification time to publish, in UTC (stat; on AROS the listing's
 * dates are local time and serve only to detect changes). 0 or -1. */
int scan_mtime_utc(const char *root, const char *name, int64_t *s, int32_t *ns);
/* May a recorded name that the scan did not list be taken as deleted?
 * 1 when the nearest listed ancestor directory was read completely (and
 * the root was), 0 otherwise. */
int scan_absence_is_deletion(const scan_result *r, const char *name);
/* The listed item of that name, or NULL. */
const scan_item *scan_find(const scan_result *r, const char *name);
/* Do size and modification date reveal every change of a file's content
 * on the volume holding root? 1 yes, 0 no: every file must then be hashed
 * at each scan. AROS fat-handler never updates the date when a file is
 * written (docs/platform/fat-write-date-not-updated-20261110.md of
 * AROS-dev), so FAT volumes get 0; SFS and the FFS/OFS family get 1; any
 * other or unknown volume gets 0. On other systems the kernel sets the
 * date on every write: 1. why receives a short description. */
int scan_dates_reliable(const char *root, char *why, size_t n);
/* 1 for names the scanner never reports (see above). */
int scan_ignored_name(const char *name);
void scan_free(scan_result *r);

#endif
