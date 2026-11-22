#include "prof.h"

#include <stdio.h>
#include <time.h>

static uint64_t total_us[PROF_N];
static unsigned long calls[PROF_N];
static const char *names[PROF_N] = { "index", "lookup", "hash", "accept", "state-mem", "state-write",
                                     "announce", "log-file", "log-console" };

uint64_t prof_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void prof_add(enum prof_cat c, uint64_t start_us)
{
    total_us[c] += prof_now_us() - start_us;
    calls[c]++;
}

void prof_format(char *buf, unsigned long n)
{
    unsigned long o = 0;
    buf[0] = 0;
    for (int i = 0; i < PROF_N && o + 1 < n; i++) {
        int w = snprintf(buf + o, n - o, "%s%s %llu ms/%lu", i ? ", " : "", names[i],
                         (unsigned long long)(total_us[i] / 1000u), calls[i]);
        if (w < 0)
            break;
        o += (unsigned long)w;
    }
}
