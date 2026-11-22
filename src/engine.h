/* The engine: every configured folder kept synchronised with its device
 * for as long as the program runs (docs/backlog/engine.md). One loop
 * serves all devices (link.h): each has one connection that carries all
 * of its folders, is made again when it is lost, and is stepped without
 * waiting, so a device that does not answer holds up no other.
 *
 * It ends on a stop request (Ctrl-C; SIGINT or SIGTERM on other
 * systems): every session writes its state, sends Close and ends. */
#ifndef ST_ENGINE_H
#define ST_ENGINE_H

#include "link.h"
#include "reach.h"

typedef struct {
    const link_config *devices;
    size_t ndevices;
    tls_log_fn log, ui;
    /* Connections that come in, relays, global discovery and our own
     * announcements (reach.h); NULL: none of these. local_port is set
     * by the engine. */
    const reach_options *reach;
} engine_config;

/* Installs the stop request handling; call once before engine_run. */
void engine_stop_init(void);
/* Nonzero once a stop was requested; stays so. */
int engine_stop_requested(void);

/* Returns 0 after a stop request (*result "STOPPED"), nonzero when it
 * cannot go on or when a state file could not be written before the
 * stop (*result names the reason). */
int engine_run(const engine_config *cfg, const tls_identity *id, const char **result);

#endif
