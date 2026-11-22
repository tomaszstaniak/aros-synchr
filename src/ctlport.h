/* How control commands (ctl.h) reach the engine.
 *
 * AROS: a public message port that takes ARexx messages; the command is
 * the message's first argument, the answer its result string and the
 * return code its primary result. Any ARexx-speaking program can send
 * them; `stpoc ctl COMMAND` does so without a Rexx interpreter. The port
 * also makes a second engine notice the first. rexxsyslib.library is
 * opened at run time: without it the engine runs with no control port.
 *
 * Other systems: a local stream socket at a path; one command line per
 * connection, answered with "RC\n" and the text.
 *
 * The provisional name of the port is CTLPORT_NAME until the program has
 * its own; elsewhere the path is the value of the environment variable
 * STPOC_CONTROL, and there is no control interface without it. */
#ifndef ST_CTLPORT_H
#define ST_CTLPORT_H

#include <stddef.h>

#define CTLPORT_NAME "SYNCHR"

typedef int (*ctlport_handler)(void *ctx, const char *line, char *out, size_t n);

/* Opens the engine's side. 0; 1 when another engine already has it;
 * -1 when it cannot be had (why says so) and the engine goes on without. */
int ctlport_open(char *why, size_t n);
/* Answers every command that has arrived; never waits for one. */
void ctlport_poll(ctlport_handler fn, void *ctx);
void ctlport_close(void);

/* The other side: sends one command and gets the answer. Returns the
 * command's return code, or 20 when no engine could be reached (out
 * says why). */
int ctlport_send(const char *line, char *out, size_t n);

#endif
