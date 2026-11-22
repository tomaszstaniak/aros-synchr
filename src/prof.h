/* Where the time of a session goes: accumulated microseconds and call
 * counts per category, reported in one line at the end of the session.
 * Measures what is wrapped, nothing else; nested categories overlap. */
#ifndef ST_PROF_H
#define ST_PROF_H

#include <stdint.h>

enum prof_cat {
    PROF_INDEX,       /* decoding an index message, including everything done per file */
    PROF_LOOKUP,      /* path checks on the file system (lstat chains, moved-aside copies) */
    PROF_HASH,        /* opening, reading and hashing local files */
    PROF_ACCEPT,      /* name checks against the registered names (in memory) */
    PROF_STATE_MEM,   /* finding and recording entries in the state list (in memory) */
    PROF_STATE_WRITE, /* serialising and writing the state file */
    PROF_ANNOUNCE,    /* building and sending IndexUpdates */
    PROF_LOG_FILE,    /* writing log lines to the log file (each flushed) */
    PROF_LOG_CONSOLE, /* writing log lines to the console */
    PROF_N
};

uint64_t prof_now_us(void);
void prof_add(enum prof_cat c, uint64_t start_us);
/* "index 1234 ms/5, lookup ..." into buf. */
void prof_format(char *buf, unsigned long n);

#define PROF_BEGIN(var) uint64_t var = prof_now_us()
#define PROF_END(cat, var) prof_add((cat), (var))

#endif
