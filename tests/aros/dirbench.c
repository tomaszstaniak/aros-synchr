/* Name lookup cost against directory size, with dos.library only (no
 * stpoc, no C library file calls).
 *
 *   dirbench DIR COUNT [REPEAT]
 *
 * Creates DIR/dbN (must not exist) with COUNT small files f00000...,
 * then times, each REPEAT times (default 20):
 *   lock-first   Lock()+UnLock() of the first file created
 *   lock-last    the same for the last file created
 *   lock-miss    Lock() of a name that does not exist
 *   open-last    Open(MODE_OLDFILE)+Close() of the last file
 * and once over all files:
 *   lock-all     Lock()+UnLock() of every file, in creation order
 * Prints the mean per call in microseconds. If a lookup reads the
 * directory from its start, lock-last and lock-miss grow with COUNT while
 * lock-first does not; if it reads the whole directory every time, all
 * three grow. Everything created is deleted by exact name at the end. */
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static long long now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void name_of(char *out, size_t n, const char *dir, long i)
{
    snprintf(out, n, "%s/f%05ld", dir, i);
}

static long long time_lock(const char *path, int repeat, int expect)
{
    long long t0 = now_us();
    for (int r = 0; r < repeat; r++) {
        BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
        if (l)
            UnLock(l);
        if ((l != 0) != expect) {
            printf("unexpected lock result for %s\n", path);
            return -1;
        }
    }
    return (now_us() - t0) / repeat;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: dirbench DIR COUNT [REPEAT]\n");
        return 10;
    }
    long count = atol(argv[2]);
    int repeat = argc > 3 ? atoi(argv[3]) : 20;
    if (count < 1 || count > 99999 || repeat < 1) {
        printf("bad COUNT or REPEAT\n");
        return 10;
    }
    char dir[256], path[300];
    snprintf(dir, sizeof dir, "%s/db%ld", argv[1], count);
    BPTR l = Lock((CONST_STRPTR)dir, SHARED_LOCK);
    if (l) {
        UnLock(l);
        printf("%s exists; nothing done\n", dir);
        return 10;
    }
    if (!(l = CreateDir((CONST_STRPTR)dir))) {
        printf("cannot create %s (%ld)\n", dir, (long)IoErr());
        return 10;
    }
    UnLock(l);
    int rc = 0;
    long made = 0;
    long long t0 = now_us();
    for (; made < count; made++) {
        name_of(path, sizeof path, dir, made);
        BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
        if (!fh) {
            printf("cannot create %s (%ld)\n", path, (long)IoErr());
            rc = 10;
            break;
        }
        Write(fh, "x\n", 2);
        Close(fh);
    }
    long long create = made ? (now_us() - t0) / made : 0;
    if (rc == 0) {
        char first[300], last[300], miss[300];
        name_of(first, sizeof first, dir, 0);
        name_of(last, sizeof last, dir, count - 1);
        snprintf(miss, sizeof miss, "%s/none", dir);
        long long lf = time_lock(first, repeat, 1);
        long long ll = time_lock(last, repeat, 1);
        long long lm = time_lock(miss, repeat, 0);
        t0 = now_us();
        for (int r = 0; r < repeat; r++) {
            BPTR fh = Open((CONST_STRPTR)last, MODE_OLDFILE);
            if (fh)
                Close(fh);
        }
        long long ol = (now_us() - t0) / repeat;
        t0 = now_us();
        for (long i = 0; i < count; i++) {
            name_of(path, sizeof path, dir, i);
            BPTR k = Lock((CONST_STRPTR)path, SHARED_LOCK);
            if (k)
                UnLock(k);
        }
        long long la = (now_us() - t0) / count;
        printf("dirbench %s count=%ld repeat=%d us/call: create=%lld lock-first=%lld lock-last=%lld "
               "lock-miss=%lld open-last=%lld lock-all=%lld\n",
               dir, count, repeat, create, lf, ll, lm, ol, la);
        if (lf < 0 || ll < 0 || lm < 0)
            rc = 10;
    }
    long left = 0;
    for (long i = 0; i < made; i++) {
        name_of(path, sizeof path, dir, i);
        if (!DeleteFile((CONST_STRPTR)path))
            left++;
    }
    if (left || !DeleteFile((CONST_STRPTR)dir)) {
        printf("cleanup incomplete: %ld file(s) left in %s\n", left, dir);
        rc = 10;
    }
    return rc;
}
