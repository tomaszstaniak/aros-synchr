/* The engine's control interface: one command line in, a text answer
 * out. The same commands arrive through a public message port on AROS
 * (ARexx messages) and through a local socket elsewhere (ctlport.h), and
 * are what a GUI, a script or a test uses.
 *
 *   VERSION          the program and the version of this interface
 *   STATUS           one line for this device's reachability (reach.h),
 *                    then one per device and per folder:
 *                      network listen=PORT relay=URL announced=yes|no unknown=ID@IP
 *                      device NAME STATE sessions=N retry=SECONDS last=REASON address=IP:PORT
 *                             via=direct|incoming|relay addresses=N
 *                      folder ID STATE device=NAME why=REASON received=N
 *                             deleted=N conflicts=N held=N failed=N in=BYTES out=BYTES
 *                    (the words of link.h; "-" for an empty value, such
 *                    as the address of a device not found yet, whose
 *                    last is NO_ADDRESS; new
 *                    key=value pairs may be added at the end of a line)
 *   SCAN ID | ALL    read the folder(s) again now
 *   PAUSE ID         leave the folder out: no session carries it and it is
 *                    not read (state paused-here); the other folders go
 *                    on. For this run only: nothing is written to the
 *                    configuration file, the next start takes it up again.
 *   RESUME ID        take it up again (read at once)
 *                    Both answer "OK ..." (return 0) when the folder
 *                    already was in that state.
 *   STOP             end the program as Ctrl-C does
 *
 * Commands are not case sensitive. The answer of a command that only
 * acts starts with "OK". */
#ifndef ST_CTL_H
#define ST_CTL_H

#include "link.h"

#define CTL_VERSION 1
#define CTL_ANSWER_MAX 16384

struct reach;

typedef struct {
    peerlink **links;
    size_t nlinks;
    void (*stop)(void); /* asks the engine to end */
    const struct reach *net; /* NULL: no network line in STATUS */
} ctl_context;

/* Runs one command. Returns 0, or 5 for a command that names something
 * that is not there, or 10 for a line that is not understood; out
 * always holds text (the answer, or what was wrong). */
int ctl_command(const ctl_context *c, const char *line, char *out, size_t n);

#endif
