/*
 * The thread of a master module.
 */
#include "hubusd_worker.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WORKER_STACK_BYTES (512u * 1024u)

typedef struct {
    hubusd_worker_t *worker;
    uint8_t mcid;
} inst_ctx_t;

typedef struct {
    hubus_module_instance_t *self;
    hubus_module_host_t host;
    inst_ctx_t ctx;
    bool created;
} instance_t;

struct hubusd_worker {
    const hubus_module_t *module;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake; /* a job, or stop */
    pthread_cond_t done; /* the thread has ended */

    hubusd_job_t jobs[HUBUSD_WORKER_JOBS];
    unsigned job_head, job_count;
    hubusd_out_t out[HUBUSD_WORKER_OUT];
    unsigned out_head, out_count;

    bool stop, failed, exited, in_call;
    uint64_t call_seq;                 /* counts the calls; changes with every one */
    uint64_t seen_seq, seen_at_ms;     /* the call the watchdog saw first, and when */

    instance_t inst[HUBUS_MCID_MAX + 1u];
    hubusd_worker_t *next_buried;
};

/* Workers that could not be stopped stay reachable here until the process ends. */
static hubusd_worker_t *graveyard;
static pthread_mutex_t graveyard_lock = PTHREAD_MUTEX_INITIALIZER;

static void bury(hubusd_worker_t *w)
{
    pthread_mutex_lock(&graveyard_lock);
    w->next_buried = graveyard;
    graveyard = w;
    pthread_mutex_unlock(&graveyard_lock);
}

bool hubusd_worker_abandoned(const hubus_module_t *module)
{
    const hubusd_worker_t *w;
    bool found = false;

    pthread_mutex_lock(&graveyard_lock);
    for (w = graveyard; w && !found; w = w->next_buried) {
        found = w->module == module;
    }
    pthread_mutex_unlock(&graveyard_lock);
    return found;
}

/* ---- output of the functions -------------------------------------------------------------------------------------- */

static bool push_out(hubusd_worker_t *w, const hubusd_out_t *o)
{
    bool ok;

    pthread_mutex_lock(&w->lock);
    ok = w->out_count < HUBUSD_WORKER_OUT;
    if (ok) {
        w->out[(w->out_head + w->out_count) % HUBUSD_WORKER_OUT] = *o;
        w->out_count++;
    }
    pthread_mutex_unlock(&w->lock);
    return ok;
}

static hubus_module_status_t host_send(const hubus_module_host_t *host, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    const inst_ctx_t *c = host->ctx;
    hubusd_out_t o;

    /* opcode and arguments are one payload; the update opcodes and the other reserved ones are not for modules */
    if ((len > 0u && args == NULL) || (unsigned)len + 1u > HUBUS_MAX_PAYLOAD ||
        (opcode != HUBUS_OP_STATUS_CAPTURE && opcode < HUBUS_OP_APP_MIN)) {
        return HUBUS_MODULE_REJECTED;
    }
    memset(&o, 0, sizeof o);
    o.kind = HUBUSD_OUT_SEND;
    o.mcid = c->mcid;
    o.opcode = opcode;
    o.n = len;
    if (len) {
        memcpy(o.args, args, len);
    }
    return push_out(c->worker, &o) ? HUBUS_MODULE_OK : HUBUS_MODULE_FAILED;
}

static hubus_module_status_t host_set_state(const hubus_module_host_t *host, const char *key, const char *text)
{
    const inst_ctx_t *c = host->ctx;
    hubusd_out_t o;
    int idx;

    if (key == NULL || text == NULL || strlen(text) >= HUBUSD_STATE_TEXT_LEN ||
        (idx = hubus_module_find_entity(c->worker->module, key)) < 0) {
        return HUBUS_MODULE_REJECTED;
    }
    memset(&o, 0, sizeof o);
    o.kind = HUBUSD_OUT_STATE;
    o.mcid = c->mcid;
    o.entity = (uint8_t)idx;
    memcpy(o.text, text, strlen(text) + 1u);
    return push_out(c->worker, &o) ? HUBUS_MODULE_OK : HUBUS_MODULE_FAILED;
}

static hubus_module_status_t host_log(const hubus_module_host_t *host, const char *text)
{
    const inst_ctx_t *c = host->ctx;
    hubusd_out_t o;

    if (text == NULL) {
        return HUBUS_MODULE_REJECTED;
    }
    memset(&o, 0, sizeof o);
    o.kind = HUBUSD_OUT_LOG;
    o.mcid = c->mcid;
    strncpy(o.text, text, sizeof o.text - 1u);
    return push_out(c->worker, &o) ? HUBUS_MODULE_OK : HUBUS_MODULE_FAILED;
}

/* ---- calls --------------------------------------------------------------------------------------------------------- */

static void call_begin(hubusd_worker_t *w)
{
    pthread_mutex_lock(&w->lock);
    w->in_call = true;
    w->call_seq++;
    pthread_mutex_unlock(&w->lock);
}

static void call_end(hubusd_worker_t *w)
{
    pthread_mutex_lock(&w->lock);
    w->in_call = false;
    pthread_mutex_unlock(&w->lock);
}

static void do_destroy(hubusd_worker_t *w, instance_t *i)
{
    if (!i->created) {
        return;
    }
    if (w->module->destroy) {
        call_begin(w);
        w->module->destroy(i->self);
        call_end(w);
    }
    i->created = false;
    i->self = NULL;
}

static void execute(hubusd_worker_t *w, const hubusd_job_t *job)
{
    const hubus_module_t *m = w->module;
    instance_t *i = &w->inst[job->mcid];
    hubusd_out_t o;

    memset(&o, 0, sizeof o);
    o.mcid = job->mcid;
    switch (job->kind) {
    case HUBUSD_JOB_CREATE:
        if (i->created) {
            break;
        }
        i->ctx.worker = w;
        i->ctx.mcid = job->mcid;
        memset(&i->host, 0, sizeof i->host);
        i->host.ctx = &i->ctx;
        i->host.mcid = job->mcid;
        i->host.send = host_send;
        i->host.set_state = host_set_state;
        i->host.log = host_log;
        if (m->create) {
            call_begin(w);
            i->self = m->create(&i->host);
            call_end(w);
            if (!i->self) {
                o.kind = HUBUSD_OUT_CREATE_FAILED;
                (void)push_out(w, &o);
                break;
            }
        }
        i->created = true;
        break;
    case HUBUSD_JOB_DESTROY:
        do_destroy(w, i);
        break;
    case HUBUSD_JOB_FRAME:
        if (i->created && m->on_frame) {
            call_begin(w);
            m->on_frame(i->self, &i->host, job->frame_kind, job->data, job->len, job->gap);
            call_end(w);
        }
        break;
    case HUBUSD_JOB_SET:
        if (!i->created || !m->on_set) {
            o.kind = HUBUSD_OUT_NOT_RUNNING;
            o.entity = job->entity;
            (void)push_out(w, &o);
        } else {
            hubus_module_status_t st;
            hubus_value_t value = job->value;

            if (value.type == HUBUS_VALUE_TEXT) {
                value.as.text = job->text;
            }
            call_begin(w);
            st = m->on_set(i->self, &i->host, &m->entities[job->entity], &value);
            call_end(w);
            if (st != HUBUS_MODULE_OK) {
                o.kind = HUBUSD_OUT_SET_REFUSED;
                o.entity = job->entity;
                o.status = st;
                (void)push_out(w, &o);
            }
        }
        break;
    }
}

static void *run(void *arg)
{
    hubusd_worker_t *w = arg;
    bool failed;
    unsigned k;

    pthread_mutex_lock(&w->lock);
    for (;;) {
        hubusd_job_t job;

        while (w->job_count == 0u && !w->stop) {
            pthread_cond_wait(&w->wake, &w->lock);
        }
        if (w->job_count == 0u || w->failed) {
            break;
        }
        job = w->jobs[w->job_head];
        w->job_head = (w->job_head + 1u) % HUBUSD_WORKER_JOBS;
        w->job_count--;
        pthread_mutex_unlock(&w->lock);
        execute(w, &job);
        pthread_mutex_lock(&w->lock);
    }
    failed = w->failed;
    pthread_mutex_unlock(&w->lock);
    if (!failed) { /* a module that failed gets no more calls, not even these */
        for (k = 0; k <= HUBUS_MCID_MAX; k++) {
            do_destroy(w, &w->inst[k]);
        }
    }
    pthread_mutex_lock(&w->lock);
    w->exited = true;
    pthread_cond_broadcast(&w->done);
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

/* ---- interface ----------------------------------------------------------------------------------------------------- */

hubusd_worker_t *hubusd_worker_start(const hubus_module_t *module)
{
    hubusd_worker_t *w = calloc(1, sizeof *w);
    pthread_attr_t attr;
    int rc;

    if (!w) {
        return NULL;
    }
    w->module = module;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->wake, NULL);
    pthread_cond_init(&w->done, NULL);
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WORKER_STACK_BYTES);
    rc = pthread_create(&w->thread, &attr, run, w);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_destroy(&w->lock);
        pthread_cond_destroy(&w->wake);
        pthread_cond_destroy(&w->done);
        free(w);
        return NULL;
    }
    return w;
}

bool hubusd_worker_post(hubusd_worker_t *w, const hubusd_job_t *job)
{
    bool ok;

    pthread_mutex_lock(&w->lock);
    ok = !w->failed && !w->stop && w->job_count < HUBUSD_WORKER_JOBS;
    if (ok) {
        w->jobs[(w->job_head + w->job_count) % HUBUSD_WORKER_JOBS] = *job;
        w->job_count++;
        pthread_cond_signal(&w->wake);
    }
    pthread_mutex_unlock(&w->lock);
    return ok;
}

bool hubusd_worker_take(hubusd_worker_t *w, hubusd_out_t *out)
{
    bool ok;

    pthread_mutex_lock(&w->lock);
    ok = w->out_count > 0u;
    if (ok) {
        *out = w->out[w->out_head];
        w->out_head = (w->out_head + 1u) % HUBUSD_WORKER_OUT;
        w->out_count--;
    }
    pthread_mutex_unlock(&w->lock);
    return ok;
}

bool hubusd_worker_check(hubusd_worker_t *w, uint64_t now_ms, uint64_t limit_ms)
{
    bool marked = false;

    pthread_mutex_lock(&w->lock);
    if (!w->failed && w->in_call) {
        if (w->call_seq != w->seen_seq) {
            w->seen_seq = w->call_seq;
            w->seen_at_ms = now_ms;
        } else if (now_ms - w->seen_at_ms >= limit_ms) {
            w->failed = true;
            marked = true;
        }
    }
    pthread_mutex_unlock(&w->lock);
    return marked;
}

bool hubusd_worker_failed(hubusd_worker_t *w)
{
    bool failed;

    pthread_mutex_lock(&w->lock);
    failed = w->failed;
    pthread_mutex_unlock(&w->lock);
    return failed;
}

const hubus_module_t *hubusd_worker_module(const hubusd_worker_t *w)
{
    return w->module; /* never changes */
}

bool hubusd_worker_stop(hubusd_worker_t *w, unsigned wait_ms)
{
    struct timespec deadline;
    bool ended;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += wait_ms / 1000u;
    deadline.tv_nsec += (long)(wait_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&w->lock);
    w->stop = true;
    pthread_cond_broadcast(&w->wake);
    while (!w->exited) {
        if (pthread_cond_timedwait(&w->done, &w->lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    ended = w->exited;
    pthread_mutex_unlock(&w->lock);
    if (!ended) {
        pthread_detach(w->thread);
        bury(w);
        return false;
    }
    pthread_join(w->thread, NULL);
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->wake);
    pthread_cond_destroy(&w->done);
    free(w);
    return true;
}
