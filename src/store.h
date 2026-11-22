/* Writing received files under one destination directory.
 *
 * Names come from the peer and are untrusted. A name is accepted only if it
 * stays inside the destination, uses a conservative character set, and does
 * not collide with another accepted name under case-insensitive comparison
 * (AROS file systems ignore case), and existing symbolic links in the path
 * are refused. A file is written to a temporary name in its own directory
 * and gets its real name only after it is complete, so an interrupted
 * download leaves the previous version in place. The replacement itself is
 * two renames, not an atomic operation; an interruption between them leaves
 * the previous version moved aside, and store_recover() puts it back on the
 * next run. Nothing is synced to disk: data the file system has not written
 * out at a power loss is not covered. */
#ifndef ST_STORE_H
#define ST_STORE_H

#include <stddef.h>
#include <stdint.h>

/* Longest path component accepted: the temporary name adds 12 characters
 * and must still fit the ~100-character limit of common AROS file systems. */
#define STORE_MAX_COMPONENT 80
#define STORE_MAX_PATH 1000

typedef struct store store;
typedef struct store_file store_file;

store *store_open(const char *root, char *why, size_t why_len);
void store_close(store *s);

/* Validates a peer-supplied name (Syncthing form: '/'-separated, relative)
 * and registers it as a file or directory. Re-registering the same name
 * with the same kind is allowed (a newer version). Returns 0, or -1 with a
 * reason in why. */
int store_accept(store *s, const char *name, int is_dir, char *why, size_t why_len);

/* What is at name under the root, checked like a write would be (no
 * symbolic link anywhere in the path): 0 nothing, 1 regular file,
 * 2 directory, -1 something else or the path cannot be checked. */
int store_local_kind(store *s, const char *name);
/* Full path of name under the root (malloc'd), for reading. */
char *store_full_path(store *s, const char *name);

/* Creates directory name (and missing parents) under the root. */
int store_mkdir(store *s, const char *name, char *why, size_t why_len);

/* Starts writing file name: creates missing parents and an empty
 * temporary file next to the final one. */
store_file *store_begin(store *s, const char *name, char *why, size_t why_len);
/* Appends data; offset must equal the bytes written so far. */
int store_write(store_file *f, int64_t offset, const void *data, size_t len);
/* What may be under the final name when a download is committed. */
typedef int (*store_check_fn)(void *ctx, const char *path); /* 0: path holds the expected content */

/* Closes the temporary file and moves it to the final name.
 *
 * check NULL: nothing may exist under the final name; if something does
 * (created here during the download), it is left alone.
 * check set: the final name must hold the version the caller expects. The
 * file is moved aside first and check() is run on the moved file, so no
 * change made before the move can be lost; if it fails, the file is put
 * back unchanged.
 *
 * Returns 0 (*replaced tells whether a previous file was replaced; why is
 * empty, or says that the previous version could not be deleted and where
 * it is kept), 1 when
 * what is here is not what was expected (nothing changed, temporary file
 * removed), or -1 on failure (reason in why). */
int store_commit(store_file *f, store_check_fn check, void *ctx, int *replaced, char *why, size_t why_len);

/* Closes the temporary file and moves it to the final name; a regular
 * file under the final name is first renamed to conflict_name (relative to
 * the root, in the same directory; it must not exist), keeping its
 * content. Nothing under the final name: as a new file. 0 (*replaced: a
 * conflict copy was made), 1 when the final name is not a regular file or
 * the conflict name is taken (nothing changed), -1 on failure (why says
 * where our version is). */
int store_commit_conflict(store_file *f, const char *conflict_name, int *replaced, char *why, size_t why_len);

/* Deletes regular file name if check() accepts it: it is moved aside first
 * and checked there, so nothing written to it before is lost; otherwise it
 * is put back. 0 deleted or already gone, 1 left as it is (changed, not a
 * regular file, an earlier moved-aside copy present), -1 on failure. */
int store_remove_file(store *s, const char *name, store_check_fn check, void *ctx, char *why, size_t why_len);
/* Renames regular file name to conflict_name (same directory). 0 renamed or
 * nothing there, 1 not a regular file or the conflict name is taken, -1. */
int store_rename_conflict(store *s, const char *name, const char *conflict_name, char *why, size_t why_len);
/* Removes directory name if it is empty. 0 removed or gone, 1 not empty
 * (or not a directory): kept, -1 on failure. */
int store_remove_dir(store *s, const char *name, char *why, size_t why_len);
/* Deletes one of this client's temporary files (".stpoc-<x>.tmp", left by
 * an interrupted download). 0 or -1. */
int store_remove_temp(store *s, const char *name, char *why, size_t why_len);

/* Puts back a previous version that an interrupted replacement left moved
 * aside (nothing under the final name, only its moved-aside copy).
 * Returns 1 when restored, 0 when there was nothing to do, 2 when both the
 * file and a moved-aside copy are here (nothing is changed: the caller
 * decides; store_commit refuses to replace the file while the copy is
 * there), -1 on failure. */
int store_recover(store *s, const char *name, char *why, size_t why_len);
/* Deletes the moved-aside copy of name, after the caller has established
 * that it holds nothing that would be lost (0, or -1 on failure). */
int store_discard_old(store *s, const char *name, char *why, size_t why_len);
/* Path of name's moved-aside copy (malloc'd), for reading. */
char *store_old_path(store *s, const char *name);
/* Discards the temporary file (an incomplete or invalid download). */
void store_abort(store_file *f);
/* Leaves the temporary file as it is and frees the handle: what a crash
 * would do. Test hook for interrupted downloads. */
void store_abandon(store_file *f);

#endif
