/* After a failed stat(), lstat() or open(): is the reason that nothing
 * has this name? errno says so where it can be relied on. On AROS it
 * cannot always: mainline nightlies of 2026-11-06 to 2026-11-14 left it
 * untouched after a failed open() (AROS PR 2250), and on the Raspberry Pi
 * runtime of 2026-11-05 a failed lstat() was seen to leave it at 0 (run
 * eng-soak-pis-20261118: no file of the peer could be taken). There the
 * answer comes from dos.library itself. */
#ifndef ST_ABSENT_H
#define ST_ABSENT_H

#include <errno.h>

#ifdef __AROS__
#include <dos/dos.h>
#include <proto/dos.h>
#endif

/* e: errno as the failed call left it. */
static inline int path_absent(const char *path, int e)
{
    if (e == ENOENT)
        return 1;
#ifdef __AROS__
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (l) {
        UnLock(l);
        return 0;
    }
    LONG io = IoErr();
    return io == ERROR_OBJECT_NOT_FOUND || io == ERROR_DIR_NOT_FOUND;
#else
    (void)path;
    return 0;
#endif
}

#endif
