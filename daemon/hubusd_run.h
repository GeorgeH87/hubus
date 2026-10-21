/*
 * Runs the daemon: libmosquitto client in its own thread, the bus loop in the
 * calling thread, signal handling (SIGTERM/SIGINT stop, SIGHUP reloads the
 * slave list and the profiles). Shared by hubusd and the simulated bus tool.
 */
#ifndef HUBUSD_RUN_H
#define HUBUSD_RUN_H

#include "hubusd_core.h"

typedef struct {
    const char *config_path;                 /* re-read on SIGHUP */
    void (*poll_hook)(void *ctx, hubusd_t *d); /* called once per loop iteration (may be NULL) */
    void *poll_ctx;
    int verbose;                             /* also print info messages */
} hubusd_run_opts_t;

/* Returns the process exit code. `cfg` must already be validated. */
int hubusd_run(const hubusd_config_t *cfg, const hubus_master_hal_t *hal, const hubusd_run_opts_t *opts);

/* Asks a running hubusd_run() to return (async-signal-safe). */
void hubusd_run_stop(void);

#endif /* HUBUSD_RUN_H */
