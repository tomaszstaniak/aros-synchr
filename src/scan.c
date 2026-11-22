#include "scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __AROS__
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#else
#include <dirent.h>
#endif

#if defined(__APPLE__) && defined(_POSIX_C_SOURCE) && !defined(_DARWIN_C_SOURCE)
#define MTIME_NS(st) ((st).st_mtimensec)
#elif defined(__APPLE__)
#define MTIME_NS(st) ((st).st_mtimespec.tv_nsec)
#elif defined(__AROS__)
#define MTIME_NS(st) 0
#else
#define MTIME_NS(st) ((st).st_mtim.tv_nsec)
#endif

static int has_prefix(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

static int ends_with(const char *s, const char *e)
{
    size_t a = strlen(s), b = strlen(e);
    return a >= b && strcmp(s + a - b, e) == 0;
}

static int prefix_ci(const char *s, const char *p)
{
    for (; *p; s++, p++) {
        char a = *s;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != *p)
            return 0;
    }
    return 1;
}

#ifdef __AROS__
#define DT_FAT_MASK 0xFFFFFF00UL
#define DT_FAT 0x46415400UL /* 'FAT\0'..'FAT\2' */
#define DT_DOS 0x444F5300UL /* 'DOS\0'..'DOS\7' */
#define DT_SFS 0x53465300UL /* 'SFS\0' */

/* FAT\2, SFS\0, DOS\3: three letters, then the last byte as a number. */
static void disk_type_text(ULONG t, char out[16])
{
    char l[4];
    for (int i = 0; i < 3; i++) {
        unsigned char c = (unsigned char)(t >> (24 - 8 * i));
        l[i] = c >= 32 && c < 127 ? (char)c : '?';
    }
    l[3] = 0;
    snprintf(out, 16, "%s\\%u", l, (unsigned)(t & 0xFF));
}

int scan_dates_reliable(const char *root, char *why, size_t n)
{
    BPTR lock = Lock((CONST_STRPTR)root, SHARED_LOCK);
    if (!lock) {
        snprintf(why, n, "volume type unknown (no lock)");
        return 0;
    }
    struct InfoData *id = AllocVec(sizeof *id, MEMF_PUBLIC | MEMF_CLEAR);
    ULONG info = 0, vol = 0;
    if (id && Info(lock, id)) {
        info = (ULONG)id->id_DiskType;
        /* fat-handler answers DOS\0 here; its volume node has the FAT type. */
        if (id->id_VolumeNode) {
            struct DosList *dl = (struct DosList *)BADDR(id->id_VolumeNode);
            vol = (ULONG)dl->dol_misc.dol_volume.dol_DiskType;
        }
    }
    FreeVec(id);
    UnLock(lock);
    char a[16], b[16];
    disk_type_text(info, a);
    disk_type_text(vol, b);
    if ((vol & DT_FAT_MASK) == DT_FAT || (info & DT_FAT_MASK) == DT_FAT) {
        snprintf(why, n, "FAT volume (%s): fat-handler does not date writes, every file is hashed at each scan", b);
        return 0;
    }
    ULONG t = vol ? vol : info;
    if (t == DT_SFS || ((t & DT_FAT_MASK) == DT_DOS && (t & 0xFF) <= 7)) {
        snprintf(why, n, "volume type %s: size and date are trusted", vol ? b : a);
        return 1;
    }
    snprintf(why, n, "volume type %s/%s not known to date every write: every file is hashed at each scan", a, b);
    return 0;
}
#else
int scan_dates_reliable(const char *root, char *why, size_t n)
{
    (void)root;
    snprintf(why, n, "size and date are trusted (host filesystem)");
    return 1;
}
#endif

struct scan_run {
    char *root;
    scan_result *r;
    size_t next;  /* the next item to look at for a directory to read */
    int started, open, early;
    char *rel;    /* the directory being read */
#ifdef __AROS__
    BPTR lock;
    struct FileInfoBlock *fib;
#else
    DIR *d;
    char *path;
#endif
};

int scan_ignored_name(const char *name)
{
    if (strcmp(name, SCAN_MARKER) == 0)
        return 1;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    return prefix_ci(base, ".stpoc-") || (has_prefix(base, ".syncthing.") && ends_with(base, ".tmp"))
           || (has_prefix(base, "~syncthing~") && ends_with(base, ".tmp"));
}

static char *join(const char *root, const char *rel)
{
    size_t a = strlen(root), b = strlen(rel);
    char *p = malloc(a + b + 2);
    if (!p)
        return NULL;
    memcpy(p, root, a);
    size_t o = a;
    if (b) {
        if (a && root[a - 1] != '/' && root[a - 1] != ':')
            p[o++] = '/';
        memcpy(p + o, rel, b);
        o += b;
    }
    p[o] = 0;
    return p;
}

static char *child(const char *rel, const char *name)
{
    size_t a = strlen(rel), b = strlen(name);
    char *p = malloc(a + b + 2);
    if (!p)
        return NULL;
    if (a) {
        memcpy(p, rel, a);
        p[a] = '/';
        memcpy(p + a + 1, name, b + 1);
    } else {
        memcpy(p, name, b + 1);
    }
    return p;
}

/* Keeps name in r->leftovers when it is one of this client's temporary or
 * moved-aside files (".stpoc-<x>.tmp", ".stpoc-<x>.old"). 0 kept (name now
 * owned by r), -1 not kept (the caller frees it). */
static int keep_leftover(scan_result *r, char *name)
{
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    size_t n = strlen(base);
    if (!prefix_ci(base, ".stpoc-") || n < 11 || (strcmp(base + n - 4, ".tmp") != 0 && strcmp(base + n - 4, ".old") != 0))
        return -1;
    if (r->nl == r->capl) {
        size_t cap = r->capl ? r->capl * 2 : 8;
        char **nl = realloc(r->leftovers, cap * sizeof *nl);
        if (!nl)
            return -1;
        r->leftovers = nl;
        r->capl = cap;
    }
    r->leftovers[r->nl++] = name;
    return 0;
}

static int add_item(scan_result *r, char *name, enum scan_kind k, int64_t size, int64_t s, int32_t ns)
{
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 64;
        scan_item *ni = realloc(r->items, cap * sizeof *ni);
        if (!ni)
            return -1;
        r->items = ni;
        r->cap = cap;
    }
    scan_item *it = &r->items[r->n++];
    it->name = name;
    it->kind = k;
    it->size = size;
    it->mtime_s = s;
    it->mtime_ns = ns;
    return 0;
}

static int add_complete(scan_result *r, const char *rel)
{
    if (r->nc == r->capc) {
        size_t cap = r->capc ? r->capc * 2 : 16;
        char **nc = realloc(r->complete, cap * sizeof *nc);
        if (!nc)
            return -1;
        r->complete = nc;
        r->capc = cap;
    }
    if (!(r->complete[r->nc] = strdup(rel)))
        return -1;
    r->nc++;
    return 0;
}

#ifdef __AROS__
/* DateStamp as seconds since 1970 in the same (local) time the file
 * system keeps: only compared with itself, for change detection. */
static void ds_time(const struct DateStamp *d, int64_t *s, int32_t *ns)
{
    *s = ((int64_t)d->ds_Days + 2922) * 86400 + (int64_t)d->ds_Minute * 60 + d->ds_Tick / TICKS_PER_SECOND;
    *ns = (int32_t)(d->ds_Tick % TICKS_PER_SECOND) * (1000000000 / TICKS_PER_SECOND);
}

static enum scan_kind fib_kind(LONG t)
{
    if (t == ST_FILE)
        return SCAN_FILE;
    if (t == ST_USERDIR)
        return SCAN_DIR;
    return SCAN_OTHER; /* links (soft or hard) and anything else */
}

/* One directory being listed. open: 0, or -1 when it cannot be read at
 * all. entry: takes one entry; 1 when there may be more, 0 at the end
 * (s->early then tells whether the listing stopped early). Directories
 * found are appended to the item list and read later. */
static int dir_open(scan_run *s, const char *rel)
{
    char *path = join(s->root, rel);
    if (!path)
        return -1;
    s->lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    free(path);
    if (!s->lock)
        return -1;
    s->fib = AllocDosObject(DOS_FIB, NULL);
    if (!s->fib || !Examine(s->lock, s->fib) || s->fib->fib_DirEntryType <= 0) {
        if (s->fib)
            FreeDosObject(DOS_FIB, s->fib);
        UnLock(s->lock);
        s->fib = NULL;
        s->lock = 0;
        return -1;
    }
    s->early = 0;
    return 0;
}

static int dir_entry(scan_run *s, const char *rel)
{
    scan_result *r = s->r;
    struct FileInfoBlock *fib = s->fib;
    if (!ExNext(s->lock, fib)) {
        if (IoErr() != ERROR_NO_MORE_ENTRIES)
            s->early = 1;
        return 0;
    }
    char *name = child(rel, (const char *)fib->fib_FileName);
    if (!name) {
        s->early = 1;
        return 0;
    }
    if (scan_ignored_name(name)) {
        if (!*rel && strcmp(name, SCAN_MARKER) == 0)
            r->marker = 1;
        if (keep_leftover(r, name) != 0)
            free(name);
        return 1;
    }
    int64_t sec;
    int32_t ns;
    ds_time(&fib->fib_Date, &sec, &ns);
    enum scan_kind k = fib_kind(fib->fib_DirEntryType);
    if (add_item(r, name, k, k == SCAN_FILE ? (int64_t)(ULONG)fib->fib_Size : 0, sec, ns) != 0) {
        free(name);
        s->early = 1;
        return 0;
    }
    return 1;
}

static void dir_close(scan_run *s)
{
    if (s->fib)
        FreeDosObject(DOS_FIB, s->fib);
    if (s->lock)
        UnLock(s->lock);
    s->fib = NULL;
    s->lock = 0;
}

int scan_one(const char *root, const char *name, scan_item *it)
{
    char *path = join(root, name);
    if (!path)
        return -1;
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    free(path);
    if (!l)
        return IoErr() == ERROR_OBJECT_NOT_FOUND ? 1 : -1;
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    int rc = -1;
    if (fib && Examine(l, fib)) {
        it->kind = fib_kind(fib->fib_DirEntryType);
        it->size = it->kind == SCAN_FILE ? (int64_t)(ULONG)fib->fib_Size : 0;
        ds_time(&fib->fib_Date, &it->mtime_s, &it->mtime_ns);
        rc = 0;
    }
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    UnLock(l);
    return rc;
}
#else
static int stat_item(const char *path, scan_item *it)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    it->kind = S_ISREG(st.st_mode) ? SCAN_FILE : S_ISDIR(st.st_mode) ? SCAN_DIR : SCAN_OTHER;
    it->size = it->kind == SCAN_FILE ? (int64_t)st.st_size : 0;
    it->mtime_s = (int64_t)st.st_mtime;
    it->mtime_ns = (int32_t)MTIME_NS(st);
    return 0;
}

static int dir_open(scan_run *s, const char *rel)
{
    s->path = join(s->root, rel);
    if (!s->path)
        return -1;
    s->d = opendir(s->path);
    if (!s->d) {
        free(s->path);
        s->path = NULL;
        return -1;
    }
    s->early = 0;
    return 0;
}

static int dir_entry(scan_run *s, const char *rel)
{
    scan_result *r = s->r;
    struct dirent *de = readdir(s->d);
    if (!de)
        return 0;
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
        return 1;
    char *name = child(rel, de->d_name);
    if (!name) {
        s->early = 1;
        return 0;
    }
    if (scan_ignored_name(name)) {
        if (!*rel && strcmp(name, SCAN_MARKER) == 0)
            r->marker = 1;
        if (keep_leftover(r, name) != 0)
            free(name);
        return 1;
    }
    char *full = join(s->path, de->d_name);
    scan_item it;
    if (!full || stat_item(full, &it) != 0) {
        /* Vanished between the listing and the lstat, or unreadable:
         * the listing of this directory is not complete. */
        free(full);
        free(name);
        s->early = 1;
        return 1;
    }
    free(full);
    if (add_item(r, name, it.kind, it.size, it.mtime_s, it.mtime_ns) != 0) {
        free(name);
        s->early = 1;
        return 0;
    }
    return 1;
}

static void dir_close(scan_run *s)
{
    if (s->d)
        closedir(s->d);
    free(s->path);
    s->d = NULL;
    s->path = NULL;
}

int scan_one(const char *root, const char *name, scan_item *it)
{
    char *path = join(root, name);
    if (!path)
        return -1;
    struct stat st;
    int missing = lstat(path, &st) != 0;
    int rc = missing ? 1 : stat_item(path, it);
    free(path);
    return rc;
}
#endif

int scan_mtime_utc(const char *root, const char *name, int64_t *s, int32_t *ns)
{
    char *path = join(root, name);
    struct stat st;
    int rc = path && stat(path, &st) == 0 ? 0 : -1;
    free(path);
    if (rc == 0) {
        *s = (int64_t)st.st_mtime;
        *ns = (int32_t)MTIME_NS(st);
    }
    return rc;
}

static int cmp_item(const void *a, const void *b)
{
    return strcmp(((const scan_item *)a)->name, ((const scan_item *)b)->name);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

scan_run *scan_begin(const char *root, scan_result *r)
{
    memset(r, 0, sizeof *r);
    scan_run *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->r = r;
    if (!(s->root = strdup(root))) {
        free(s);
        return NULL;
    }
    return s;
}

void scan_end(scan_run *s)
{
    if (!s)
        return;
    if (s->open)
        dir_close(s);
    free(s->rel);
    free(s->root);
    free(s);
}

int scan_step(scan_run *s, int entries)
{
    scan_result *r = s->r;
    while (entries > 0) {
        if (!s->open) {
            /* The root first, then every directory found, in turn (the
             * list grows as we go). */
            if (!s->started) {
                s->started = 1;
                s->rel = strdup("");
            } else {
                while (s->next < r->n && r->items[s->next].kind != SCAN_DIR)
                    s->next++;
                if (s->next == r->n) {
                    qsort(r->items, r->n, sizeof *r->items, cmp_item);
                    qsort(r->complete, r->nc, sizeof *r->complete, cmp_str);
                    return 0;
                }
                s->rel = strdup(r->items[s->next++].name);
            }
            if (!s->rel)
                return -1;
            if (dir_open(s, s->rel) != 0) {
                if (!*s->rel)
                    return -1; /* the root itself */
                r->failed_dirs++;
                free(s->rel);
                s->rel = NULL;
                entries--;
                continue;
            }
            s->open = 1;
        }
        entries--;
        if (dir_entry(s, s->rel))
            continue;
        dir_close(s);
        s->open = 0;
        if (s->early)
            r->failed_dirs++;
        else if (add_complete(r, s->rel) != 0)
            return -1;
        free(s->rel);
        s->rel = NULL;
    }
    return 1;
}

int scan_folder(const char *root, scan_result *r)
{
    scan_run *s = scan_begin(root, r);
    if (!s)
        return -1;
    int rc;
    while ((rc = scan_step(s, 1024)) == 1)
        ;
    scan_end(s);
    return rc;
}

static int complete(const scan_result *r, const char *dir)
{
    return bsearch(&dir, r->complete, r->nc, sizeof *r->complete, cmp_str) != NULL;
}

static const scan_item *listed(const scan_result *r, const char *name)
{
    scan_item key;
    key.name = (char *)name;
    return bsearch(&key, r->items, r->n, sizeof *r->items, cmp_item);
}

const scan_item *scan_find(const scan_result *r, const char *name)
{
    return listed(r, name);
}

int scan_absence_is_deletion(const scan_result *r, const char *name)
{
    if (!complete(r, ""))
        return 0;
    char *p = strdup(name);
    if (!p)
        return 0;
    int ok = 0;
    for (;;) {
        char *slash = strrchr(p, '/');
        if (!slash) {
            ok = 1; /* reached the root, which was read completely */
            break;
        }
        *slash = 0;
        const scan_item *it = listed(r, p);
        if (it) {
            /* The nearest ancestor that is there decides. */
            ok = it->kind == SCAN_DIR && complete(r, p);
            break;
        }
    }
    free(p);
    return ok;
}

void scan_free(scan_result *r)
{
    for (size_t i = 0; i < r->n; i++)
        free(r->items[i].name);
    for (size_t i = 0; i < r->nc; i++)
        free(r->complete[i]);
    for (size_t i = 0; i < r->nl; i++)
        free(r->leftovers[i]);
    free(r->items);
    free(r->complete);
    free(r->leftovers);
    memset(r, 0, sizeof *r);
}
