/*
 * The functions of a master module run on a thread of their own, one thread per module (docs/project/open-issues.md,
 * Master Modules). The bus thread only posts jobs and takes what the functions produce:
 *
 *   bus thread --- jobs (create, destroy, frame, set) ---> worker thread --- module functions
 *   bus thread <--- output (commands, states, refusals) --- worker thread <--- host->send(), host->set_state()
 *
 * Both queues are bounded. The calls of one module run one after the other in the order of the jobs; different modules do
 * not wait for each other. A call that does not return in time is noticed by hubusd_worker_check(): the module is marked
 * failed and gets no more jobs. The thread of a failed module cannot be stopped safely, so it is left alone.
 */
#ifndef HUBUSD_WORKER_H
#define HUBUSD_WORKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hubus_module.h"

#define HUBUSD_WORKER_JOBS 32u
#define HUBUSD_WORKER_OUT 64u
#define HUBUSD_STATE_TEXT_LEN (HUBUS_MODULE_TEXT_MAX + 1u) /* including the terminator */

typedef enum {
    HUBUSD_JOB_CREATE, /* an instance for the slave `mcid` */
    HUBUSD_JOB_DESTROY,
    HUBUSD_JOB_FRAME, /* a frame of the slave */
    HUBUSD_JOB_SET    /* a value for the entity `entity` of the slave */
} hubusd_job_kind_t;

typedef struct {
    hubusd_job_kind_t kind;
    uint8_t mcid;
    hubus_frame_kind_t frame_kind; /* FRAME */
    bool gap;                      /* FRAME: frames were lost before this one */
    uint8_t len;
    uint8_t data[HUBUS_MAX_PAYLOAD];
    uint8_t entity;      /* SET: index in the entities of the module */
    hubus_value_t value; /* SET; for a text value the worker points it to `text` */
    char text[HUBUSD_STATE_TEXT_LEN];
} hubusd_job_t;

typedef enum {
    HUBUSD_OUT_SEND,          /* a command to the slave: opcode, args */
    HUBUSD_OUT_STATE,         /* the value of an entity, as text */
    HUBUSD_OUT_LOG,           /* a line for the log, as text */
    HUBUSD_OUT_SET_REFUSED,   /* on_set did not take the value: `status` says how */
    HUBUSD_OUT_NOT_RUNNING,   /* a SET for a slave without an instance */
    HUBUSD_OUT_CREATE_FAILED  /* create returned NULL */
} hubusd_out_kind_t;

typedef struct {
    hubusd_out_kind_t kind;
    uint8_t mcid;
    uint8_t opcode;
    uint8_t n;
    uint8_t args[HUBUS_MAX_PAYLOAD];
    uint8_t entity;
    hubus_module_status_t status;
    char text[HUBUSD_STATE_TEXT_LEN];
} hubusd_out_t;

typedef struct hubusd_worker hubusd_worker_t;

/* Starts the thread of a module. NULL if it cannot be started. */
hubusd_worker_t *hubusd_worker_start(const hubus_module_t *module);

/* Queues a job. False if the queue is full, the module has failed, or the worker is stopping. */
bool hubusd_worker_post(hubusd_worker_t *worker, const hubusd_job_t *job);

/* Takes the oldest output. False if there is none. */
bool hubusd_worker_take(hubusd_worker_t *worker, hubusd_out_t *out);

/* Call it regularly from the bus thread with its clock. A call that has been running for `limit_ms` since this function
 * first saw it marks the module failed. True the moment that happens. */
bool hubusd_worker_check(hubusd_worker_t *worker, uint64_t now_ms, uint64_t limit_ms);

bool hubusd_worker_failed(hubusd_worker_t *worker);

/* The module of this worker. */
const hubus_module_t *hubusd_worker_module(const hubusd_worker_t *worker);

/* Destroys the instances that are left, ends the thread, and waits up to `wait_ms` for it. True if the thread ended and the
 * worker is freed. False: a call did not return; the worker stays allocated until the process ends. */
bool hubusd_worker_stop(hubusd_worker_t *worker, unsigned wait_ms);

/* True if a thread that could not be stopped may still run in the code of this module: its plugin must stay loaded. */
bool hubusd_worker_abandoned(const hubus_module_t *module);

#endif /* HUBUSD_WORKER_H */
