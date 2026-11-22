#include "ctlport.h"

#include "ctl.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __AROS__

#include <exec/ports.h>
#include <proto/exec.h>
#include <proto/rexxsyslib.h>
#include <rexx/errors.h>
#include <rexx/rxslib.h>
#include <rexx/storage.h>

/* Defined here, so that the library is opened by this file at run time
 * and not by the startup code, which would refuse to start without it. */
struct RxsLib *RexxSysBase;
static struct MsgPort *port;

static int rexx_open(void)
{
    if (!RexxSysBase)
        RexxSysBase = (struct RxsLib *)OpenLibrary((CONST_STRPTR)"rexxsyslib.library", 0);
    return RexxSysBase != NULL;
}

int ctlport_open(char *why, size_t n)
{
    if (!rexx_open()) {
        snprintf(why, n, "rexxsyslib.library cannot be opened");
        return -1;
    }
    if (!(port = CreateMsgPort())) {
        snprintf(why, n, "no message port");
        return -1;
    }
    port->mp_Node.ln_Name = (char *)CTLPORT_NAME;
    port->mp_Node.ln_Pri = 0;
    int taken = 0;
    Forbid();
    if (FindPort((CONST_STRPTR)CTLPORT_NAME))
        taken = 1;
    else
        AddPort(port);
    Permit();
    if (taken) {
        DeleteMsgPort(port);
        port = NULL;
        snprintf(why, n, "port %s exists: the program is already running", CTLPORT_NAME);
        return 1;
    }
    net_wait_signals(1UL << port->mp_SigBit);
    return 0;
}

void ctlport_poll(ctlport_handler fn, void *ctx)
{
    static char out[CTL_ANSWER_MAX];
    struct Message *m;
    if (!port)
        return;
    while ((m = GetMsg(port)) != NULL) {
        struct RexxMsg *rm = (struct RexxMsg *)m;
        if (IsRexxMsg(rm)) {
            const char *line = rm->rm_Args[0] ? (const char *)rm->rm_Args[0] : "";
            int rc = fn(ctx, line, out, sizeof out);
            rm->rm_Result1 = rc;
            rm->rm_Result2 = 0;
            if (rc == 0 && (rm->rm_Action & RXFF_RESULT))
                rm->rm_Result2 = (IPTR)CreateArgstring((STRPTR)out, strlen(out));
        }
        ReplyMsg(m);
    }
}

void ctlport_close(void)
{
    if (port) {
        struct Message *m;
        net_wait_signals(0);
        Forbid();
        RemPort(port);
        while ((m = GetMsg(port)) != NULL) {
            if (IsRexxMsg((struct RexxMsg *)m))
                ((struct RexxMsg *)m)->rm_Result1 = RC_FATAL;
            ReplyMsg(m);
        }
        Permit();
        DeleteMsgPort(port);
        port = NULL;
    }
    if (RexxSysBase) {
        CloseLibrary((struct Library *)RexxSysBase);
        RexxSysBase = NULL;
    }
}

int ctlport_send(const char *line, char *out, size_t n)
{
    int rc = 20;
    out[0] = 0;
    if (!rexx_open()) {
        snprintf(out, n, "rexxsyslib.library cannot be opened");
        return 20;
    }
    struct MsgPort *reply = CreateMsgPort();
    struct RexxMsg *rm = reply ? CreateRexxMsg(reply, NULL, NULL) : NULL;
    if (rm && (rm->rm_Args[0] = (IPTR)CreateArgstring((STRPTR)line, strlen(line))) != 0) {
        rm->rm_Action = RXCOMM | RXFF_RESULT;
        Forbid();
        struct MsgPort *to = FindPort((CONST_STRPTR)CTLPORT_NAME);
        if (to)
            PutMsg(to, (struct Message *)rm);
        Permit();
        if (to) {
            WaitPort(reply);
            GetMsg(reply);
            rc = (int)rm->rm_Result1;
            if (rc == 0 && rm->rm_Result2) {
                snprintf(out, n, "%s", (const char *)rm->rm_Result2);
                DeleteArgstring((STRPTR)rm->rm_Result2);
            }
        } else {
            snprintf(out, n, "no port %s: the engine is not running", CTLPORT_NAME);
        }
        DeleteArgstring((STRPTR)rm->rm_Args[0]);
    } else {
        snprintf(out, n, "out of memory");
    }
    if (rm)
        DeleteRexxMsg(rm);
    if (reply)
        DeleteMsgPort(reply);
    CloseLibrary((struct Library *)RexxSysBase);
    RexxSysBase = NULL;
    return rc;
}

#else /* not AROS */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int lfd = -1;
static char lpath[100];

static int address(struct sockaddr_un *sa, char *why, size_t n)
{
    const char *p = getenv("STPOC_CONTROL");
    if (!p || !*p) {
        snprintf(why, n, "STPOC_CONTROL is not set");
        return -1;
    }
    if (strlen(p) >= sizeof sa->sun_path) {
        snprintf(why, n, "the path in STPOC_CONTROL is too long");
        return -1;
    }
    memset(sa, 0, sizeof *sa);
    sa->sun_family = AF_UNIX;
    strcpy(sa->sun_path, p);
    return 0;
}

/* Waits up to a second for the descriptor. */
static int ready(int fd, int for_write)
{
    fd_set s;
    struct timeval tv = { 1, 0 };
    FD_ZERO(&s);
    FD_SET(fd, &s);
    return select(fd + 1, for_write ? NULL : &s, for_write ? &s : NULL, NULL, &tv) > 0;
}

int ctlport_open(char *why, size_t n)
{
    struct sockaddr_un sa;
    if (address(&sa, why, n) != 0)
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(why, n, "no socket (errno %d)", errno);
        return -1;
    }
    /* A path left by an engine that is gone is taken over; one that
     * answers belongs to a running engine. */
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        close(fd);
        snprintf(why, n, "%s is answered: the program is already running", sa.sun_path);
        return 1;
    }
    unlink(sa.sun_path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 8) != 0
        || fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != 0) {
        snprintf(why, n, "cannot listen at %s (errno %d)", sa.sun_path, errno);
        close(fd);
        return -1;
    }
    lfd = fd;
    snprintf(lpath, sizeof lpath, "%s", sa.sun_path);
    net_wait_extra(lfd);
    return 0;
}

void ctlport_poll(ctlport_handler fn, void *ctx)
{
    static char out[CTL_ANSWER_MAX];
    if (lfd < 0)
        return;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0)
            return;
        char line[512];
        size_t len = 0;
        while (len < sizeof line - 1 && !memchr(line, '\n', len) && ready(fd, 0)) {
            long got = read(fd, line + len, sizeof line - 1 - len);
            if (got <= 0)
                break;
            len += (size_t)got;
        }
        line[len] = 0;
        line[strcspn(line, "\r\n")] = 0;
        if (len == 0) {
            close(fd); /* only a look whether somebody answers (ctlport_open) */
            continue;
        }
        int rc = fn(ctx, line, out, sizeof out - 16);
        char head[16];
        int hl = snprintf(head, sizeof head, "%d\n", rc);
        const char *parts[2] = { head, out };
        size_t lens[2] = { (size_t)hl, strlen(out) };
        for (int i = 0; i < 2; i++)
            for (size_t done = 0; done < lens[i] && ready(fd, 1);) {
                long w = write(fd, parts[i] + done, lens[i] - done);
                if (w <= 0)
                    break;
                done += (size_t)w;
            }
        close(fd);
    }
}

void ctlport_close(void)
{
    if (lfd >= 0) {
        net_wait_extra(-1);
        close(lfd);
        unlink(lpath);
        lfd = -1;
    }
}

int ctlport_send(const char *line, char *out, size_t n)
{
    struct sockaddr_un sa;
    char buf[CTL_ANSWER_MAX];
    out[0] = 0;
    if (address(&sa, out, n) != 0)
        return 20;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        snprintf(out, n, "nothing answers at %s: the engine is not running", sa.sun_path);
        if (fd >= 0)
            close(fd);
        return 20;
    }
    signal(SIGPIPE, SIG_IGN);
    size_t len = strlen(line), got = 0;
    if (write(fd, line, len) != (long)len || write(fd, "\n", 1) != 1) {
        snprintf(out, n, "the command could not be sent");
        close(fd);
        return 20;
    }
    for (;;) {
        long r = got < sizeof buf - 1 ? read(fd, buf + got, sizeof buf - 1 - got) : 0;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = 0;
    char *nl = strchr(buf, '\n');
    if (!nl) {
        snprintf(out, n, "no answer");
        return 20;
    }
    snprintf(out, n, "%s", nl + 1);
    return atoi(buf);
}

#endif
