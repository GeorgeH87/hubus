/*
 * The thread of a master module.
 */
#include "hubusd_worker.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WORKER_STACK_BYTES (512u * 1024u)
#define BASE_LEN 96u

typedef struct {
    hubusd_worker_t *worker;
    uint8_t mcid;
} inst_ctx_t;

typedef struct {
    hubus_module_instance_t *self;
    hubus_module_host_t host;
    inst_ctx_t ctx;
    bool created;
    char topics[HUBUS_MODULE_TOPICS_MAX][HUBUSD_SUBTOPIC_LEN]; /* what it subscribed to; the bus thread keeps the same list */
    unsigned n_topics;
    hubusd_settings_t settings; /* what it saved last, or what the master gave it with create */
} instance_t;

struct hubusd_worker {
    const hubus_module_t *module;
    char base[BASE_LEN];
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake; /* a job, or stop */
    pthread_cond_t done; /* the thread has ended */

    hubusd_job_t jobs[HUBUSD_WORKER_JOBS];
    unsigned job_head, job_count;
    hubusd_job_t current; /* the job being executed */
    hubusd_out_t out[HUBUSD_WORKER_OUT];
    unsigned out_head, out_count;

    bool stop, failed, exited, in_call;
    bool executing;                    /* a job was taken and is not done yet */
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

static void out_init(hubusd_out_t *o, hubusd_out_kind_t kind, uint8_t mcid)
{
    o->kind = kind;
    o->mcid = mcid;
    o->opcode = 0;
    o->n = 0;
    o->retain = false;
    o->topic[0] = '\0';
    o->component[0] = '\0';
    o->len = 0;
    o->payload[0] = '\0';
}

static hubus_module_status_t pushed(hubusd_worker_t *w, const hubusd_out_t *o)
{
    return push_out(w, o) ? HUBUS_MODULE_OK : HUBUS_MODULE_FAILED;
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
    out_init(&o, HUBUSD_OUT_SEND, c->mcid);
    o.opcode = opcode;
    o.n = len;
    if (len) {
        memcpy(o.args, args, len);
    }
    return pushed(c->worker, &o);
}

static int find_topic(const instance_t *i, const char *topic)
{
    unsigned k;

    for (k = 0; k < i->n_topics; k++) {
        if (strcmp(i->topics[k], topic) == 0) {
            return (int)k;
        }
    }
    return -1;
}

static hubus_module_status_t host_subscribe(const hubus_module_host_t *host, const char *topic)
{
    const inst_ctx_t *c = host->ctx;
    instance_t *i = &c->worker->inst[c->mcid];
    hubusd_out_t o;

    if (!hubus_module_topic_valid(topic, true)) {
        return HUBUS_MODULE_REJECTED;
    }
    if (find_topic(i, topic) >= 0) {
        return HUBUS_MODULE_OK;
    }
    if (i->n_topics >= HUBUS_MODULE_TOPICS_MAX) {
        return HUBUS_MODULE_REJECTED;
    }
    out_init(&o, HUBUSD_OUT_SUBSCRIBE, c->mcid);
    memcpy(o.topic, topic, strlen(topic) + 1u);
    if (!push_out(c->worker, &o)) {
        return HUBUS_MODULE_FAILED;
    }
    memcpy(i->topics[i->n_topics++], topic, strlen(topic) + 1u);
    return HUBUS_MODULE_OK;
}

static hubus_module_status_t host_unsubscribe(const hubus_module_host_t *host, const char *topic)
{
    const inst_ctx_t *c = host->ctx;
    instance_t *i = &c->worker->inst[c->mcid];
    hubusd_out_t o;
    int k;

    if (!hubus_module_topic_valid(topic, true)) {
        return HUBUS_MODULE_REJECTED;
    }
    if ((k = find_topic(i, topic)) < 0) {
        return HUBUS_MODULE_OK;
    }
    out_init(&o, HUBUSD_OUT_UNSUBSCRIBE, c->mcid);
    memcpy(o.topic, topic, strlen(topic) + 1u);
    if (!push_out(c->worker, &o)) {
        return HUBUS_MODULE_FAILED;
    }
    i->n_topics--;
    memmove(i->topics[k], i->topics[k + 1], (i->n_topics - (unsigned)k) * sizeof i->topics[0]);
    return HUBUS_MODULE_OK;
}

static hubus_module_status_t host_publish(const hubus_module_host_t *host, const char *topic, const void *payload,
                                          size_t len, bool retain)
{
    const inst_ctx_t *c = host->ctx;
    hubusd_out_t o;

    if (!hubus_module_topic_valid(topic, false) || len > HUBUS_MODULE_PAYLOAD_MAX || (len > 0u && payload == NULL)) {
        return HUBUS_MODULE_REJECTED;
    }
    out_init(&o, HUBUSD_OUT_PUBLISH, c->mcid);
    memcpy(o.topic, topic, strlen(topic) + 1u);
    o.retain = retain;
    o.len = len;
    if (len) {
        memcpy(o.payload, payload, len);
    }
    o.payload[len] = '\0';
    return pushed(c->worker, &o);
}

/* A component of Home Assistant: lower-case letters and '_'. */
static bool component_valid(const char *component)
{
    size_t i;

    if (component == NULL || component[0] == '\0' || strlen(component) >= HUBUSD_COMPONENT_LEN) {
        return false;
    }
    for (i = 0; component[i] != '\0'; i++) {
        if (!((component[i] >= 'a' && component[i] <= 'z') || component[i] == '_')) {
            return false;
        }
    }
    return true;
}

static hubus_module_status_t host_discovery(const hubus_module_host_t *host, const char *component, const char *object,
                                            const char *json)
{
    const inst_ctx_t *c = host->ctx;
    hubusd_out_t o;
    size_t len = json ? strlen(json) : 0u;

    if (!component_valid(component) || !hubus_module_topic_valid(object, false) || strchr(object, '/') != NULL ||
        len < 2u || len > HUBUS_MODULE_PAYLOAD_MAX || json[0] != '{' || json[len - 1u] != '}') {
        return HUBUS_MODULE_REJECTED;
    }
    out_init(&o, HUBUSD_OUT_DISCOVERY, c->mcid);
    memcpy(o.component, component, strlen(component) + 1u);
    memcpy(o.topic, object, strlen(object) + 1u);
    o.len = len;
    memcpy(o.payload, json, len + 1u);
    return pushed(c->worker, &o);
}

static size_t host_topic(const hubus_module_host_t *host, const char *topic, char *out, size_t out_size)
{
    const inst_ctx_t *c = host->ctx;
    int n;

    if (topic == NULL || out == NULL || out_size == 0u) {
        return 0;
    }
    n = snprintf(out, out_size, "%s/slave/%u/%s", c->worker->base, (unsigned)c->mcid, topic);
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

static hubus_module_status_t host_text(const hubus_module_host_t *host, hubusd_out_kind_t kind, const char *text)
{
    const inst_ctx_t *c = host->ctx;
    hubusd_out_t o;

    if (text == NULL) {
        return HUBUS_MODULE_REJECTED;
    }
    out_init(&o, kind, c->mcid);
    snprintf(o.payload, HUBUS_MODULE_LOG_MAX + 1u, "%s", text);
    o.len = strlen(o.payload);
    return pushed(c->worker, &o);
}

static hubus_module_status_t host_log(const hubus_module_host_t *host, const char *text)
{
    return host_text(host, HUBUSD_OUT_LOG, text);
}

static hubus_module_status_t host_reject(const hubus_module_host_t *host, const char *text)
{
    return host_text(host, HUBUSD_OUT_REJECT, text);
}

static int find_setting(const hubusd_settings_t *st, const char *key)
{
    unsigned k;

    for (k = 0; k < st->n; k++) {
        if (strcmp(st->item[k].key, key) == 0) {
            return (int)k;
        }
    }
    return -1;
}

/* Answered from the copy of the instance: the bus thread keeps the same values. */
static size_t host_setting(const hubus_module_host_t *host, const char *key, char *out, size_t out_size)
{
    const inst_ctx_t *c = host->ctx;
    const hubusd_settings_t *st = &c->worker->inst[c->mcid].settings;
    int k;
    size_t len;

    if (out == NULL || out_size == 0u) {
        return 0;
    }
    out[0] = '\0';
    if (key == NULL || (k = find_setting(st, key)) < 0 || (len = strlen(st->item[k].value)) >= out_size) {
        return 0;
    }
    memcpy(out, st->item[k].value, len + 1u);
    return len;
}

static hubus_module_status_t host_save(const hubus_module_host_t *host, const char *key, const char *value)
{
    const inst_ctx_t *c = host->ctx;
    hubusd_settings_t *st = &c->worker->inst[c->mcid].settings;
    int k;
    hubusd_out_t o;

    if (!hubus_module_setting_valid(key, value)) {
        return HUBUS_MODULE_REJECTED;
    }
    k = find_setting(st, key);
    if (value ? (k >= 0 && strcmp(st->item[k].value, value) == 0) : k < 0) {
        return HUBUS_MODULE_OK; /* no change, nothing to write */
    }
    if (value && k < 0 && st->n >= HUBUS_MODULE_SETTINGS_MAX) {
        return HUBUS_MODULE_REJECTED;
    }
    out_init(&o, HUBUSD_OUT_SAVE, c->mcid);
    memcpy(o.topic, key, strlen(key) + 1u);
    if (value) {
        o.len = strlen(value);
        memcpy(o.payload, value, o.len + 1u);
    } else {
        o.n = 1;
    }
    if (!push_out(c->worker, &o)) {
        return HUBUS_MODULE_FAILED;
    }
    if (!value) {
        st->n--;
        memmove(&st->item[k], &st->item[k + 1], (st->n - (unsigned)k) * sizeof st->item[0]);
    } else {
        if (k < 0) {
            k = (int)st->n++;
            memcpy(st->item[k].key, key, strlen(key) + 1u);
        }
        memcpy(st->item[k].value, value, strlen(value) + 1u);
    }
    return HUBUS_MODULE_OK;
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
    call_begin(w);
    w->module->destroy(i->self);
    call_end(w);
    i->created = false;
    i->self = NULL;
    i->n_topics = 0; /* the bus thread drops them as it posts the job */
}

static void execute(hubusd_worker_t *w, const hubusd_job_t *job)
{
    const hubus_module_t *m = w->module;
    instance_t *i = &w->inst[job->mcid];
    hubusd_out_t o;

    i->host.readback = job->readback;
    switch (job->kind) {
    case HUBUSD_JOB_CREATE:
        if (i->created) {
            break;
        }
        i->ctx.worker = w;
        i->ctx.mcid = job->mcid;
        memset(&i->host, 0, sizeof i->host);
        i->host.ctx = &i->ctx;
        i->host.module = m;
        i->host.mcid = job->mcid;
        i->host.readback = job->readback;
        i->host.send = host_send;
        i->host.subscribe = host_subscribe;
        i->host.unsubscribe = host_unsubscribe;
        i->host.publish = host_publish;
        i->host.discovery = host_discovery;
        i->host.topic = host_topic;
        i->host.log = host_log;
        i->host.reject = host_reject;
        i->host.setting = host_setting;
        i->host.save = host_save;
        i->settings = job->settings;
        i->n_topics = 0;
        call_begin(w);
        i->self = m->create(&i->host);
        call_end(w);
        if (!i->self) {
            i->n_topics = 0;
            out_init(&o, HUBUSD_OUT_CREATE_FAILED, job->mcid);
            (void)push_out(w, &o);
            break;
        }
        i->created = true;
        break;
    case HUBUSD_JOB_DESTROY:
        do_destroy(w, i);
        break;
    case HUBUSD_JOB_FRAME:
        if (i->created) {
            call_begin(w);
            m->on_frame(i->self, &i->host, job->frame_kind, job->data, job->len, job->gap);
            call_end(w);
        }
        break;
    case HUBUSD_JOB_MESSAGE:
        if (!i->created) {
            out_init(&o, HUBUSD_OUT_NOT_RUNNING, job->mcid);
            memcpy(o.topic, job->topic, sizeof o.topic);
            (void)push_out(w, &o);
        } else {
            call_begin(w);
            m->on_message(i->self, &i->host, job->topic, job->payload, job->payload_len);
            call_end(w);
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
        hubusd_job_t *job;

        while (w->job_count == 0u && !w->stop) {
            pthread_cond_wait(&w->wake, &w->lock);
        }
        if (w->job_count == 0u || w->failed) {
            break;
        }
        job = &w->current;
        *job = w->jobs[w->job_head];
        w->job_head = (w->job_head + 1u) % HUBUSD_WORKER_JOBS;
        w->job_count--;
        w->executing = true;
        pthread_mutex_unlock(&w->lock);
        execute(w, job);
        pthread_mutex_lock(&w->lock);
        w->executing = false;
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

hubusd_worker_t *hubusd_worker_start(const hubus_module_t *module, const char *base)
{
    hubusd_worker_t *w = calloc(1, sizeof *w);
    pthread_attr_t attr;
    int rc;

    if (!w) {
        return NULL;
    }
    w->module = module;
    snprintf(w->base, sizeof w->base, "%s", base);
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

bool hubusd_worker_busy(hubusd_worker_t *w)
{
    bool busy;

    pthread_mutex_lock(&w->lock);
    busy = (!w->failed && (w->job_count > 0u || w->executing)) || w->out_count > 0u;
    pthread_mutex_unlock(&w->lock);
    return busy;
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
