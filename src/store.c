#include "store.h"

#include "absent.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TEMP_PREFIX ".stpoc-"


struct entry {
    char *folded; /* lower-case path, for collision checks */
    char *exact;
    int is_dir;
};

struct store {
    char *root;
    struct entry *entries;
    size_t n, cap;
};

struct store_file {
    store *s;
    FILE *fp;
    char *final_path, *temp_path, *old_path;
    int64_t written;
};

#ifdef STORE_TEST_HOOKS
/* Test builds only: bit n set makes the n-th rename() of a commit fail. */
unsigned store_test_fail_renames;
static unsigned rename_calls;
static int do_rename(const char *from, const char *to)
{
    if (store_test_fail_renames & (1u << rename_calls++)) {
        errno = EIO;
        return -1;
    }
    return rename(from, to);
}
/* 1: every write fails; 2: writes succeed but the commit's flush fails
 * (as with buffered data on a full disk). */
int store_test_fail_writes;
/* Nonzero: deleting the previous version after a replacement fails, as it
 * does on AROS while the file is open somewhere. */
int store_test_keep_old;
#else
#define do_rename rename
#endif

static void why_set(char *why, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

static char *join(const char *a, const char *b)
{
    size_t la = strlen(a);
    /* An AROS root may end in a volume or assign ("RAM:"), taking no '/'. */
    const char *sep = (la == 0 || a[la - 1] == '/' || a[la - 1] == ':') ? "" : "/";
    size_t n = la + strlen(sep) + strlen(b) + 1;
    char *p = malloc(n);
    if (p)
        snprintf(p, n, "%s%s%s", a, sep, b);
    return p;
}

static int is_dir_path(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

store *store_open(const char *root, char *why, size_t why_len)
{
    if (!is_dir_path(root)) {
        why_set(why, why_len, "destination %s is not an existing directory", root);
        return NULL;
    }
    store *s = calloc(1, sizeof *s);
    if (!s || !(s->root = strdup(root))) {
        free(s);
        why_set(why, why_len, "out of memory");
        return NULL;
    }
    return s;
}

void store_close(store *s)
{
    if (!s)
        return;
    for (size_t i = 0; i < s->n; i++) {
        free(s->entries[i].folded);
        free(s->entries[i].exact);
    }
    free(s->entries);
    free(s->root);
    free(s);
}

/* Case-insensitive prefix test: on a case-insensitive file system
 * ".STPOC-x.tmp" names the same file as ".stpoc-x.tmp". */
static int prefix_ci(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

static int check_component(const char *c, size_t len, char *why, size_t why_len)
{
    if (len == 0) {
        why_set(why, why_len, "empty path component");
        return -1;
    }
    if ((len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.')) {
        why_set(why, why_len, "'.' or '..' component");
        return -1;
    }
    if (len > STORE_MAX_COMPONENT) {
        why_set(why, why_len, "component longer than %d bytes", STORE_MAX_COMPONENT);
        return -1;
    }
    if (len >= strlen(TEMP_PREFIX) && prefix_ci(c, TEMP_PREFIX)) {
        why_set(why, why_len, "name uses this client's temporary prefix");
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)c[i];
        /* ':' separates a volume or assign on AROS, '\\' is a separator on
         * other systems, control characters are not portable, and names
         * outside ASCII would need a decision about encoding on AROS that
         * this PoC does not make. */
        if (ch < 0x20 || ch >= 0x7f || ch == ':' || ch == '\\') {
            why_set(why, why_len, "unsupported character 0x%02x", ch);
            return -1;
        }
    }
    if (c[len - 1] == ' ' || c[len - 1] == '.') {
        why_set(why, why_len, "component ends in a space or dot");
        return -1;
    }
    return 0;
}

static char *fold(const char *p)
{
    char *f = strdup(p);
    for (char *q = f; q && *q; q++)
        if (*q >= 'A' && *q <= 'Z')
            *q = (char)(*q - 'A' + 'a');
    return f;
}

int store_accept(store *s, const char *name, int is_dir, char *why, size_t why_len)
{
    size_t len = strlen(name);
    if (len == 0 || len > STORE_MAX_PATH) {
        why_set(why, why_len, "empty or too long name");
        return -1;
    }
    if (name[0] == '/') {
        why_set(why, why_len, "absolute name");
        return -1;
    }
    for (const char *c = name;;) {
        const char *e = strchr(c, '/');
        size_t cl = e ? (size_t)(e - c) : strlen(c);
        if (check_component(c, cl, why, why_len) != 0)
            return -1;
        if (!e)
            break;
        c = e + 1;
    }

    char *f = fold(name);
    if (!f) {
        why_set(why, why_len, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < s->n; i++) {
        struct entry *e = &s->entries[i];
        size_t el = strlen(e->folded);
        if (strcmp(e->folded, f) == 0) {
            if (strcmp(e->exact, name) != 0 || e->is_dir != is_dir) {
                why_set(why, why_len, "collides with \"%s\"", e->exact);
                free(f);
                return -1;
            }
            free(f); /* same name, same kind: a newer version */
            return 0;
        }
        /* A registered file cannot also be a parent directory, and the
         * other way round. */
        if (!e->is_dir && el < len && strncmp(f, e->folded, el) == 0 && f[el] == '/') {
            why_set(why, why_len, "a parent is the file \"%s\"", e->exact);
            free(f);
            return -1;
        }
        if (!is_dir && len < el && strncmp(e->folded, f, len) == 0 && e->folded[len] == '/') {
            why_set(why, why_len, "is a parent of \"%s\"", e->exact);
            free(f);
            return -1;
        }
    }
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 32;
        struct entry *ne = realloc(s->entries, cap * sizeof *ne);
        if (!ne) {
            free(f);
            why_set(why, why_len, "out of memory");
            return -1;
        }
        s->entries = ne;
        s->cap = cap;
    }
    s->entries[s->n].folded = f;
    s->entries[s->n].exact = strdup(name);
    s->entries[s->n].is_dir = is_dir;
    s->n++;
    return 0;
}

/* Refuses a path under the root if any part of it that already exists is
 * a symbolic link (or, for a parent, not a directory): following a link
 * placed in the destination would write outside it. Names from the index
 * never contain links themselves; this protects against ones that already
 * exist on disk. Checked before use, so a link created in between (by
 * another program, at the same moment) is not caught. */
static int check_no_links(store *s, const char *rel, char *why, size_t why_len)
{
    char *path = strdup(rel);
    if (!path) {
        why_set(why, why_len, "out of memory");
        return -1;
    }
    for (char *p = path;; p++) {
        if (*p != '/' && *p != 0)
            continue;
        char save = *p;
        *p = 0;
        char *full = join(s->root, path);
        struct stat st;
        if (!full) {
            why_set(why, why_len, "out of memory");
            free(path);
            return -1;
        }
        int r = lstat(full, &st);
        /* Only "does not exist" ends the check; any other failure (no
         * permission, I/O error, ...) means the path could not be checked. */
        int e = errno;
        if (r != 0 && !path_absent(full, e)) {
            why_set(why, why_len, "cannot check %s (errno %d)", full, e);
            free(full);
            free(path);
            return -1;
        }
        if (r == 0 && S_ISLNK(st.st_mode)) {
            why_set(why, why_len, "%s is a symbolic link", full);
            free(full);
            free(path);
            return -1;
        }
        if (r == 0 && save == '/' && !S_ISDIR(st.st_mode)) {
            why_set(why, why_len, "%s is not a directory", full);
            free(full);
            free(path);
            return -1;
        }
        free(full);
        *p = save;
        if (r != 0 || save == 0)
            break; /* the rest does not exist yet, or the whole path is checked */
    }
    free(path);
    return 0;
}

/* Creates every directory of rel up to (not including) its last component
 * when files_parent is set, or including it otherwise. */
static int make_dirs(store *s, const char *rel, int files_parent, char *why, size_t why_len)
{
    char *path = strdup(rel);
    if (!path)
        return -1;
    char *end = path + strlen(path);
    if (files_parent) {
        char *slash = strrchr(path, '/');
        if (!slash) {
            free(path);
            return 0;
        }
        end = slash;
    }
    for (char *p = path;; p++) {
        if (*p == '/' || p == end) {
            char save = *p;
            *p = 0;
            char *full = join(s->root, path);
            if (!full) {
                free(path);
                return -1;
            }
            if (!is_dir_path(full) && mkdir(full, 0755) != 0 && !is_dir_path(full)) {
                why_set(why, why_len, "cannot create directory %s (errno %d)", full, errno);
                free(full);
                free(path);
                return -1;
            }
            free(full);
            *p = save;
            if (p == end)
                break;
        }
    }
    free(path);
    return 0;
}

int store_local_kind(store *s, const char *name)
{
    char why[64];
    if (check_no_links(s, name, why, sizeof why) != 0)
        return -1;
    char *full = join(s->root, name);
    struct stat st;
    int r = full ? lstat(full, &st) : -1;
    int e = errno;
    int absent = r != 0 && full && path_absent(full, e);
    free(full);
    if (r != 0)
        return absent ? 0 : -1;
    if (S_ISREG(st.st_mode))
        return 1;
    if (S_ISDIR(st.st_mode))
        return 2;
    return -1;
}

char *store_full_path(store *s, const char *name)
{
    return join(s->root, name);
}

int store_mkdir(store *s, const char *name, char *why, size_t why_len)
{
    if (check_no_links(s, name, why, why_len) != 0)
        return -1;
    return make_dirs(s, name, 0, why, why_len);
}

/* lstat-based: does a regular file (not a link) exist at path? */
static int kind_at(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return 0;
    if (S_ISLNK(st.st_mode))
        return 2;
    if (S_ISDIR(st.st_mode))
        return 3;
    return S_ISREG(st.st_mode) ? 1 : 4;
}

static char *sibling(const char *final_path, const char *suffix)
{
    /* dir/name -> dir/.stpoc-name<suffix> */
    const char *base = strrchr(final_path, '/');
    const char *colon = strrchr(final_path, ':');
    if (!base || (colon && colon > base))
        base = colon;
    size_t dirlen = base ? (size_t)(base - final_path + 1) : 0;
    const char *name = final_path + dirlen;
    size_t n = dirlen + strlen(TEMP_PREFIX) + strlen(name) + strlen(suffix) + 1;
    char *p = malloc(n);
    if (p)
        snprintf(p, n, "%.*s%s%s%s", (int)dirlen, final_path, TEMP_PREFIX, name, suffix);
    return p;
}

store_file *store_begin(store *s, const char *name, char *why, size_t why_len)
{
    if (check_no_links(s, name, why, why_len) != 0 || make_dirs(s, name, 1, why, why_len) != 0
        || check_no_links(s, name, why, why_len) != 0)
        return NULL;
    store_file *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->s = s;
    f->final_path = join(s->root, name);
    f->temp_path = f->final_path ? sibling(f->final_path, ".tmp") : NULL;
    f->old_path = f->final_path ? sibling(f->final_path, ".old") : NULL;
    if (!f->temp_path || !f->old_path) {
        why_set(why, why_len, "out of memory");
        store_abandon(f);
        return NULL;
    }
    if (is_dir_path(f->final_path)) {
        why_set(why, why_len, "%s is a directory", f->final_path);
        store_abandon(f);
        return NULL;
    }
    int tk = kind_at(f->temp_path), ok = kind_at(f->old_path);
    if ((tk != 0 && tk != 1) || (ok != 0 && ok != 1)) {
        why_set(why, why_len, "%s or %s exists and is not a regular file", f->temp_path, f->old_path);
        store_abandon(f);
        return NULL;
    }
    /* A temporary file left by an interrupted run is removed and created
     * again exclusively: O_EXCL does not follow a link put in its place. */
    if (tk == 1)
        remove(f->temp_path);
    int fd = open(f->temp_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0 || !(f->fp = fdopen(fd, "wb"))) {
        why_set(why, why_len, "cannot create %s (errno %d)", f->temp_path, errno);
        if (fd >= 0)
            close(fd);
        store_abandon(f);
        return NULL;
    }
    return f;
}

int store_write(store_file *f, int64_t offset, const void *data, size_t len)
{
    if (offset != f->written)
        return -1;
#ifdef STORE_TEST_HOOKS
    if (store_test_fail_writes == 1) {
        errno = ENOSPC;
        return -1;
    }
#endif
    if (len && fwrite(data, 1, len, f->fp) != len)
        return -1;
    f->written += (int64_t)len;
    return 0;
}

/* Closes the temporary file. 0, or -1 (reason in why, temporary removed). */
static int close_temp(store_file *f, char *why, size_t why_len)
{
    int bad = fflush(f->fp) != 0 || ferror(f->fp);
#ifdef STORE_TEST_HOOKS
    if (store_test_fail_writes == 2)
        bad = 1, errno = ENOSPC;
#endif
    int e = errno;
    if (fclose(f->fp) != 0)
        bad = 1, e = errno;
    f->fp = NULL;
    if (bad) {
        why_set(why, why_len, "writing %s failed (errno %d)", f->temp_path, e);
        remove(f->temp_path);
        return -1;
    }
    return 0;
}

int store_commit(store_file *f, store_check_fn check, void *ctx, int *replaced, char *why, size_t why_len)
{
    int rc = -1;
    *replaced = 0;
    if (close_temp(f, why, why_len) != 0)
        goto out;
#ifdef STORE_TEST_HOOKS
    rename_calls = 0;
#endif
    int k = kind_at(f->final_path);
    if (!check) {
        if (k != 0) {
            /* Appeared during the download: it belongs to someone here. */
            why_set(why, why_len, "%s was created here during the download; left as it is", f->final_path);
            remove(f->temp_path);
            rc = 1;
            goto out;
        }
        if (do_rename(f->temp_path, f->final_path) != 0) {
            why_set(why, why_len, "rename to %s failed (errno %d)", f->final_path, errno);
            remove(f->temp_path);
            goto out;
        }
        rc = 0;
        goto out;
    }
    if (k != 1) {
        why_set(why, why_len, "%s is %s; left as it is", f->final_path,
                k == 0 ? "no longer here" : "no longer a regular file");
        remove(f->temp_path);
        rc = 1;
        goto out;
    }
    /* A moved-aside copy that is already here was left by an earlier run
     * (an interruption, or a restore that failed) and may hold the only
     * copy of something. It is never deleted here: the replacement waits
     * until it has been dealt with (store_recover, store_discard_old). */
    if (kind_at(f->old_path) != 0) {
        why_set(why, why_len, "an earlier copy %s is still here; %s left as it is", f->old_path, f->final_path);
        remove(f->temp_path);
        rc = 1;
        goto out;
    }
    /* Replacing: rename() over an existing file is not something to rely on
     * on every AROS file system, so move the old version aside first and
     * put it back if the new one cannot take its place. The window in
     * which no file has the final name is these two renames. */
    if (do_rename(f->final_path, f->old_path) != 0) {
        why_set(why, why_len, "cannot move the old %s aside (errno %d); it was not changed",
                f->final_path, errno);
        remove(f->temp_path);
        goto out;
    }
    /* Checked after the move: whatever was written to it until now is in
     * the file checked, and nothing can be written under the final name
     * without us noticing (it would make the second rename fail or be
     * found by the next check). */
    if (check(ctx, f->old_path) != 0) {
        remove(f->temp_path);
        if (do_rename(f->old_path, f->final_path) == 0) {
            why_set(why, why_len, "%s was changed here during the download; left as it is", f->final_path);
            rc = 1;
        } else {
            why_set(why, why_len,
                    "%s was changed here during the download, and putting it back failed (errno %d); "
                    "it is kept as %s",
                    f->final_path, errno, f->old_path);
        }
        goto out;
    }
    *replaced = 1;
    if (kind_at(f->final_path) != 0 || do_rename(f->temp_path, f->final_path) != 0) {
        int re = kind_at(f->final_path) != 0 ? EEXIST : errno;
        remove(f->temp_path);
        if (kind_at(f->final_path) == 0 && do_rename(f->old_path, f->final_path) == 0) {
            why_set(why, why_len, "rename to %s failed (errno %d); old version restored", f->final_path, re);
        } else {
            /* Say where the previous version is: it must not be lost
             * silently. */
            why_set(why, why_len,
                    "rename to %s failed (errno %d) and restoring the old version failed (errno %d); "
                    "the previous version is kept as %s",
                    f->final_path, re, errno, f->old_path);
        }
        *replaced = 0;
        goto out;
    }
    /* The previous version is no longer needed. If it cannot be deleted
     * (on AROS: still open somewhere), it stays, and the caller is told:
     * whatever is written to it through that handle is not lost, and the
     * next run finds the copy and keeps it (store_recover). */
    why[0] = 0;
#ifdef STORE_TEST_HOOKS
    if (store_test_keep_old)
        errno = EBUSY;
    else
#endif
        remove(f->old_path);
    if (kind_at(f->old_path) != 0)
        why_set(why, why_len, "the previous version could not be deleted (errno %d, in use?); it is kept as %s",
                errno, f->old_path);
    rc = 0;
out:
    store_abandon(f);
    return rc;
}

int store_commit_conflict(store_file *f, const char *conflict_name, int *replaced, char *why, size_t why_len)
{
    int rc = -1;
    char *cpath = NULL;
    *replaced = 0;
    if (close_temp(f, why, why_len) != 0)
        goto out;
    if (check_no_links(f->s, conflict_name, why, why_len) != 0) {
        remove(f->temp_path);
        goto out;
    }
    int k = kind_at(f->final_path);
    if (k != 0 && k != 1) {
        why_set(why, why_len, "%s is no longer a regular file; left as it is", f->final_path);
        remove(f->temp_path);
        rc = 1;
        goto out;
    }
    if (k == 1) {
        if (!(cpath = join(f->s->root, conflict_name))) {
            why_set(why, why_len, "out of memory");
            remove(f->temp_path);
            goto out;
        }
        if (kind_at(cpath) != 0) {
            why_set(why, why_len, "%s exists already; %s left as it is", cpath, f->final_path);
            remove(f->temp_path);
            rc = 1;
            goto out;
        }
        /* Ours keeps its content under the conflict name; nothing is
         * checked, because nothing of it is lost. */
        if (do_rename(f->final_path, cpath) != 0) {
            why_set(why, why_len, "cannot rename %s to %s (errno %d); it was not changed", f->final_path, cpath,
                    errno);
            remove(f->temp_path);
            goto out;
        }
        *replaced = 1;
    }
    if (kind_at(f->final_path) != 0 || do_rename(f->temp_path, f->final_path) != 0) {
        int re = kind_at(f->final_path) != 0 ? EEXIST : errno;
        remove(f->temp_path);
        why_set(why, why_len, "rename to %s failed (errno %d)%s%s", f->final_path, re,
                *replaced ? "; our version is kept as " : "", *replaced ? cpath : "");
        goto out;
    }
    why[0] = 0;
    rc = 0;
out:
    free(cpath);
    store_abandon(f);
    return rc;
}

int store_remove_file(store *s, const char *name, store_check_fn check, void *ctx, char *why, size_t why_len)
{
    if (check_no_links(s, name, why, why_len) != 0)
        return -1;
    char *final = join(s->root, name);
    char *old = final ? sibling(final, ".old") : NULL;
    int rc = -1;
    if (!old) {
        why_set(why, why_len, "out of memory");
        goto out;
    }
    int k = kind_at(final);
    if (k == 0) {
        rc = 0; /* already gone */
        goto out;
    }
    if (k != 1) {
        why_set(why, why_len, "%s is not a regular file; left as it is", final);
        rc = 1;
        goto out;
    }
    if (kind_at(old) != 0) {
        why_set(why, why_len, "an earlier copy %s is still here; %s left as it is", old, final);
        rc = 1;
        goto out;
    }
    if (do_rename(final, old) != 0) {
        why_set(why, why_len, "cannot move %s aside (errno %d); it was not changed", final, errno);
        goto out;
    }
    /* Checked after the move, as in store_commit: whatever was written to
     * it until now is in the file checked. */
    if (check(ctx, old) != 0) {
        if (do_rename(old, final) == 0) {
            why_set(why, why_len, "%s was changed here; left as it is", final);
            rc = 1;
        } else {
            why_set(why, why_len, "%s was changed here, and putting it back failed (errno %d); it is kept as %s",
                    final, errno, old);
        }
        goto out;
    }
    why[0] = 0;
    remove(old);
    if (kind_at(old) != 0)
        why_set(why, why_len, "%s could not be deleted (errno %d, in use?); it is kept as %s", final, errno, old);
    rc = 0;
out:
    free(final);
    free(old);
    return rc;
}

int store_rename_conflict(store *s, const char *name, const char *conflict_name, char *why, size_t why_len)
{
    if (check_no_links(s, name, why, why_len) != 0 || check_no_links(s, conflict_name, why, why_len) != 0)
        return -1;
    char *final = join(s->root, name), *cpath = join(s->root, conflict_name);
    int rc = -1;
    if (!final || !cpath) {
        why_set(why, why_len, "out of memory");
    } else if (kind_at(final) == 0) {
        rc = 0; /* nothing to keep */
    } else if (kind_at(final) != 1) {
        why_set(why, why_len, "%s is not a regular file; left as it is", final);
        rc = 1;
    } else if (kind_at(cpath) != 0) {
        why_set(why, why_len, "%s exists already; %s left as it is", cpath, final);
        rc = 1;
    } else if (do_rename(final, cpath) != 0) {
        why_set(why, why_len, "cannot rename %s to %s (errno %d); it was not changed", final, cpath, errno);
    } else {
        rc = 0;
    }
    free(final);
    free(cpath);
    return rc;
}

int store_remove_dir(store *s, const char *name, char *why, size_t why_len)
{
    if (check_no_links(s, name, why, why_len) != 0)
        return -1;
    char *path = join(s->root, name);
    int rc = -1;
    if (!path) {
        why_set(why, why_len, "out of memory");
        return -1;
    }
    int k = kind_at(path);
    if (k == 0) {
        rc = 0;
    } else if (k != 3) {
        why_set(why, why_len, "%s is not a directory; left as it is", path);
        rc = 1;
    } else if (rmdir(path) == 0) {
        rc = 0;
    } else if (kind_at(path) == 3) {
        /* Something we do not know is still in it (or it cannot be
         * removed): the directory stays. */
        why_set(why, why_len, "%s is not empty or cannot be removed (errno %d); kept", path, errno);
        rc = 1;
    } else {
        rc = 0;
    }
    free(path);
    return rc;
}

int store_remove_temp(store *s, const char *name, char *why, size_t why_len)
{
    /* Only our own temporary files: ".stpoc-<x>.tmp". */
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    size_t n = strlen(base);
    if (!prefix_ci(base, TEMP_PREFIX) || n < strlen(TEMP_PREFIX) + 4 || strcmp(base + n - 4, ".tmp") != 0) {
        why_set(why, why_len, "%s is not a temporary file of this client", name);
        return -1;
    }
    if (check_no_links(s, name, why, why_len) != 0)
        return -1;
    char *path = join(s->root, name);
    int rc = -1;
    if (!path)
        why_set(why, why_len, "out of memory");
    else if (kind_at(path) != 1)
        why_set(why, why_len, "%s is not a regular file", path);
    else if (remove(path) != 0)
        why_set(why, why_len, "cannot delete %s (errno %d)", path, errno);
    else
        rc = 0;
    free(path);
    return rc;
}

int store_recover(store *s, const char *name, char *why, size_t why_len)
{
    if (check_no_links(s, name, why, why_len) != 0)
        return -1;
    char *final = join(s->root, name);
    char *old = final ? sibling(final, ".old") : NULL;
    int rc = 0;
    if (!old) {
        why_set(why, why_len, "out of memory");
        rc = -1;
    } else if (kind_at(old) != 0 && kind_at(old) != 1) {
        why_set(why, why_len, "%s exists and is not a regular file", old);
        rc = -1;
    } else if (kind_at(old) == 1 && kind_at(final) == 0) {
        if (rename(old, final) == 0) {
            rc = 1;
        } else {
            why_set(why, why_len, "cannot put %s back as %s (errno %d)", old, final, errno);
            rc = -1;
        }
    } else if (kind_at(old) == 1) {
        why_set(why, why_len, "%s and an earlier copy %s are both here", final, old);
        rc = 2;
    }
    free(final);
    free(old);
    return rc;
}

int store_discard_old(store *s, const char *name, char *why, size_t why_len)
{
    if (check_no_links(s, name, why, why_len) != 0)
        return -1;
    char *final = join(s->root, name);
    char *old = final ? sibling(final, ".old") : NULL;
    int rc = 0;
    if (!old) {
        why_set(why, why_len, "out of memory");
        rc = -1;
    } else if (kind_at(old) == 1 && remove(old) != 0) {
        why_set(why, why_len, "cannot delete %s (errno %d)", old, errno);
        rc = -1;
    }
    free(final);
    free(old);
    return rc;
}

char *store_old_path(store *s, const char *name)
{
    char *final = join(s->root, name);
    char *old = final ? sibling(final, ".old") : NULL;
    free(final);
    return old;
}

void store_abort(store_file *f)
{
    if (!f)
        return;
    if (f->fp) {
        fclose(f->fp);
        f->fp = NULL;
    }
    if (f->temp_path)
        remove(f->temp_path);
    store_abandon(f);
}

void store_abandon(store_file *f)
{
    if (!f)
        return;
    if (f->fp)
        fclose(f->fp);
    free(f->final_path);
    free(f->temp_path);
    free(f->old_path);
    free(f);
}
