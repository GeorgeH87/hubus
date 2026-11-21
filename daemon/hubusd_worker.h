/*
 * The functions of a master module run on a thread of their own, one thread per module (docs/project/open-issues.md,
 * Master Modules). The bus thread only posts jobs and takes what the functions produce:
 *
 *   bus thread --- jobs (create, destroy, frame, message) ---> worker thread --- module functions
 *   bus thread <--- output (commands, messages, topics) ------ worker thread <--- host->send(), host->publish(), ...
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
#include "hubusd_config.h"

#define HUBUSD_WORKER_JOBS 32u
#define HUBUSD_WORKER_OUT 64u
#define HUBUSD_SUBTOPIC_LEN (HUBUS_MODULE_TOPIC_MAX + 1u) /* a topic below the slave, including the terminator */
#define HUBUSD_COMPONENT_LEN 32u

typedef enum {
    HUBUSD_JOB_CREATE, /* an instance for the slave `mcid` */
    HUBUSD_JOB_DESTROY,
    HUBUSD_JOB_FRAME,  /* a frame of the slave */
    HUBUSD_JOB_MESSAGE /* a message on a topic the instance subscribed to */
} hubusd_job_kind_t;

typedef struct {
    hubusd_job_kind_t kind;
    uint8_t mcid;
    hubus_module_readback_t readback; /* of the slave, for host->readback */
    hubus_frame_kind_t frame_kind;    /* FRAME */
    bool gap;                         /* FRAME: frames were lost before this one */
    uint8_t len;
    uint8_t data[HUBUS_MAX_PAYLOAD];
    char topic[HUBUSD_SUBTOPIC_LEN];                   /* MESSAGE */
    size_t payload_len;                             /* MESSAGE */
    char payload[HUBUS_MODULE_MESSAGE_MAX + 1u];    /* MESSAGE, with a terminator */
    hubusd_settings_t settings;                     /* CREATE: what the master keeps for the instance (host->setting()) */
} hubusd_job_t;

typedef enum {
    HUBUSD_OUT_SEND,          /* a command to the slave: opcode, args */
    HUBUSD_OUT_PUBLISH,       /* a message on `topic` below the slave */
    HUBUSD_OUT_SUBSCRIBE,     /* the instance takes `topic` */
    HUBUSD_OUT_UNSUBSCRIBE,
    HUBUSD_OUT_DISCOVERY,     /* a discovery message: `component`, the object in `topic`, the JSON in `payload` */
    HUBUSD_OUT_LOG,           /* a line for the log, in `payload` */
    HUBUSD_OUT_REJECT,        /* why a message was not taken, in `payload` */
    HUBUSD_OUT_NOT_RUNNING,   /* a message on `topic` for a slave without an instance */
    HUBUSD_OUT_CREATE_FAILED, /* create returned NULL */
    HUBUSD_OUT_SAVE           /* a setting: the key in `topic`, the value in `payload`; `n` 1: the key is removed */
} hubusd_out_kind_t;

typedef struct {
    hubusd_out_kind_t kind;
    uint8_t mcid;
    uint8_t opcode;
    uint8_t n;
    uint8_t args[HUBUS_MAX_PAYLOAD];
    bool retain;
    char topic[HUBUSD_SUBTOPIC_LEN];
    char component[HUBUSD_COMPONENT_LEN];
    size_t len;
    char payload[HUBUS_MODULE_PAYLOAD_MAX + 1u]; /* with a terminator */
} hubusd_out_t;

typedef struct hubusd_worker hubusd_worker_t;

/* Starts the thread of a module. `base` is the base topic, for host->topic(). NULL if it cannot be started. */
hubusd_worker_t *hubusd_worker_start(const hubus_module_t *module, const char *base);

/* Queues a job. False if the queue is full, the module has failed, or the worker is stopping. */
bool hubusd_worker_post(hubusd_worker_t *worker, const hubusd_job_t *job);

/* Takes the oldest output. False if there is none. */
bool hubusd_worker_take(hubusd_worker_t *worker, hubusd_out_t *out);

/* True while jobs wait, a call runs, or output waits to be taken. The jobs of a failed module do not count. */
bool hubusd_worker_busy(hubusd_worker_t *worker);

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
