/*
 * The thread of a master module (daemon/hubusd_worker.h), on its own: order of the calls, bounded queues, what the host
 * functions accept, the topics of an instance, the time limit, and stopping a thread that hangs.
 */
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "hubusd_worker.h"
#include "unity.h"

#define MAX_CALLS 64

typedef enum { CALL_CREATE, CALL_DESTROY, CALL_FRAME, CALL_MESSAGE } call_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static call_t calls[MAX_CALLS];
static uint8_t call_mcid[MAX_CALLS];
static uint8_t frame_byte[MAX_CALLS];
static bool frame_gap[MAX_CALLS];
static hubus_module_readback_t call_readback[MAX_CALLS];
static unsigned n_calls;
static bool block_calls; /* functions wait here until the test lets them go */
static bool create_fails;
static bool check_host; /* on_message tries what the host accepts */
static hubus_module_status_t results[16];
static char full_topic[128];
static size_t full_len, small_len;
static char message_topic[HUBUSD_SUBTOPIC_LEN];
static char message_payload[HUBUS_MODULE_MESSAGE_MAX + 1u];
static size_t message_len;
static const hubus_module_host_t *last_host;
static hubus_module_instance_t *self_seen;
static int the_instance;

static void record(call_t c, const hubus_module_host_t *host, uint8_t byte, bool gap)
{
    pthread_mutex_lock(&lock);
    if (n_calls < MAX_CALLS) {
        calls[n_calls] = c;
        call_mcid[n_calls] = host ? host->mcid : 0u;
        call_readback[n_calls] = host ? host->readback : HUBUS_READBACK_NONE;
        frame_byte[n_calls] = byte;
        frame_gap[n_calls] = gap;
        n_calls++;
    }
    pthread_cond_broadcast(&changed);
    while (block_calls) {
        pthread_cond_wait(&changed, &lock);
    }
    pthread_mutex_unlock(&lock);
}

static hubus_module_instance_t *m_create(const hubus_module_host_t *host)
{
    last_host = host;
    record(CALL_CREATE, host, 0, false);
    return create_fails ? NULL : (hubus_module_instance_t *)&the_instance;
}

static void m_destroy(hubus_module_instance_t *self)
{
    self_seen = self;
    record(CALL_DESTROY, NULL, 0, false);
}

static void m_frame(hubus_module_instance_t *self, const hubus_module_host_t *host, hubus_frame_kind_t kind,
                    const uint8_t *data, uint8_t len, bool gap)
{
    (void)kind;
    self_seen = self;
    record(CALL_FRAME, host, len ? data[0] : 0, gap);
}

/* Everything a function can ask of its host, and what the host has to refuse. */
static void try_the_host(const hubus_module_host_t *host)
{
    static const uint8_t arg[HUBUS_MAX_PAYLOAD] = {0x55};
    static char too_long[HUBUS_MODULE_PAYLOAD_MAX + 2u];
    char small[8];
    unsigned k = 0;

    memset(too_long, 'x', sizeof too_long - 1u);
    results[k++] = host->send(host, 0x10, arg, 1);
    results[k++] = host->send(host, 0x05, NULL, 0);                 /* reserved opcode */
    results[k++] = host->send(host, 0x10, arg, HUBUS_MAX_PAYLOAD);  /* one byte too much with the opcode */
    results[k++] = host->subscribe(host, "lamp/+");
    results[k++] = host->subscribe(host, "lamp/+");                 /* the second time changes nothing */
    results[k++] = host->subscribe(host, "cmd/x");                  /* the master's */
    results[k++] = host->publish(host, "lamp/state", "ON", 2, true);
    results[k++] = host->publish(host, "lamp/+", "ON", 2, false);   /* no wildcards */
    results[k++] = host->publish(host, "lamp/state", too_long, HUBUS_MODULE_PAYLOAD_MAX + 1u, false);
    results[k++] = host->discovery(host, "light", "lamp", "{\"name\":\"Lamp\"}");
    results[k++] = host->discovery(host, "Light", "lamp", "{}");   /* not a component */
    results[k++] = host->discovery(host, "light", "lamp/x", "{}"); /* one level only */
    results[k++] = host->discovery(host, "light", "lamp", "name"); /* no object */
    results[k++] = host->reject(host, "no");
    full_len = host->topic(host, "lamp/state", full_topic, sizeof full_topic);
    small_len = host->topic(host, "lamp/state", small, sizeof small);
}

/* The settings: what create brought, what a save changes at once, and what the host refuses. */
static bool check_settings;
static hubus_module_status_t setting_results[8];
static char setting_seen[4][HUBUS_MODULE_SETTING_MAX + 1u];
static size_t setting_len[4];

static void try_the_settings(const hubus_module_host_t *host)
{
    static char too_long[HUBUS_MODULE_SETTING_MAX + 2u];
    char key[4];
    char tiny[2];
    unsigned k = 0, i;

    memset(too_long, 'x', sizeof too_long - 1u);
    setting_len[0] = host->setting(host, "level", setting_seen[0], sizeof setting_seen[0]); /* from create */
    setting_len[1] = host->setting(host, "level", tiny, sizeof tiny);                     /* does not fit */
    setting_results[k++] = host->save(host, "level", "9");
    setting_len[2] = host->setting(host, "level", setting_seen[2], sizeof setting_seen[2]); /* at once */
    setting_results[k++] = host->save(host, "level", "9");    /* no change: nothing to write */
    setting_results[k++] = host->save(host, "module", "x");   /* the master's */
    setting_results[k++] = host->save(host, "mode", too_long);
    setting_results[k++] = host->save(host, "colour", NULL);  /* not there: nothing to remove */
    setting_results[k++] = host->save(host, "level", NULL);
    setting_len[3] = host->setting(host, "level", setting_seen[3], sizeof setting_seen[3]);
    for (i = 0; i < HUBUS_MODULE_SETTINGS_MAX; i++) {
        snprintf(key, sizeof key, "k%u", i);
        (void)host->save(host, key, "v");
    }
    setting_results[k++] = host->save(host, "one-more", "v"); /* full */
}

static void m_message(hubus_module_instance_t *self, const hubus_module_host_t *host, const char *topic,
                      const char *payload, size_t len)
{
    self_seen = self;
    snprintf(message_topic, sizeof message_topic, "%s", topic);
    memcpy(message_payload, payload, len + 1u); /* with the terminator */
    message_len = len;
    if (check_host) {
        try_the_host(host);
    }
    if (check_settings) {
        try_the_settings(host);
    }
    record(CALL_MESSAGE, host, 0, false);
}

static const hubus_module_rule_t rules[] = {{.pattern = {0xF0}, .mask = {0xFF}, .len = 1}};
static const hubus_module_t module = {
    HUBUS_MODULE_INIT,
    .name = "worker_test",
    .rules = rules,
    .n_rules = 1,
    .max_command = HUBUS_MAX_PAYLOAD,
    .create = m_create,
    .destroy = m_destroy,
    .on_frame = m_frame,
    .on_message = m_message,
};

static hubusd_worker_t *worker;

static void wait_for_calls(unsigned n)
{
    struct timespec until;
    unsigned seen;

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 5;
    pthread_mutex_lock(&lock);
    while (n_calls < n && pthread_cond_timedwait(&changed, &lock, &until) == 0) {
    }
    seen = n_calls;
    pthread_mutex_unlock(&lock);
    TEST_ASSERT_TRUE_MESSAGE(seen >= n, "the worker did not make the calls in time");
}

/* Until the worker has nothing left to do but output to be taken. */
static void wait_until_idle(void)
{
    struct timespec pause = {0, 1000000};
    unsigned i;
    hubusd_out_t o;

    for (i = 0; i < 5000u; i++) {
        if (!hubusd_worker_busy(worker)) {
            return;
        }
        if (hubusd_worker_take(worker, &o)) {
            TEST_FAIL_MESSAGE("output where none was expected");
        }
        nanosleep(&pause, NULL);
    }
    TEST_FAIL_MESSAGE("the worker stayed busy");
}

static void block(bool on)
{
    pthread_mutex_lock(&lock);
    block_calls = on;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

static bool post_kind(hubusd_job_kind_t kind, uint8_t mcid, uint8_t byte, bool gap)
{
    hubusd_job_t job;

    memset(&job, 0, sizeof job);
    job.kind = kind;
    job.mcid = mcid;
    job.gap = gap;
    job.len = 1;
    job.data[0] = byte;
    return hubusd_worker_post(worker, &job);
}

static bool post_message(uint8_t mcid, const char *topic, const char *payload, hubus_module_readback_t readback)
{
    hubusd_job_t job;

    memset(&job, 0, sizeof job);
    job.kind = HUBUSD_JOB_MESSAGE;
    job.mcid = mcid;
    job.readback = readback;
    snprintf(job.topic, sizeof job.topic, "%s", topic);
    job.payload_len = strlen(payload);
    memcpy(job.payload, payload, job.payload_len + 1u);
    return hubusd_worker_post(worker, &job);
}

static hubusd_out_t take(hubusd_out_kind_t kind)
{
    hubusd_out_t o;

    TEST_ASSERT_TRUE_MESSAGE(hubusd_worker_take(worker, &o), "no output");
    TEST_ASSERT_EQUAL(kind, o.kind);
    return o;
}

void setUp(void)
{
    n_calls = 0;
    block_calls = false;
    create_fails = false;
    check_host = false;
    self_seen = NULL;
    worker = hubusd_worker_start(&module, "hubus");
    TEST_ASSERT_NOT_NULL(worker);
}

void tearDown(void)
{
    block(false);
    if (worker) {
        TEST_ASSERT_TRUE(hubusd_worker_stop(worker, 5000));
    }
}

static void test_calls_run_in_the_order_of_the_jobs(void)
{
    unsigned i;

    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    for (i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_FRAME, 4, (uint8_t)i, false));
    }
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_DESTROY, 4, 0, false));
    wait_for_calls(12);
    TEST_ASSERT_EQUAL(CALL_CREATE, calls[0]);
    TEST_ASSERT_EQUAL_UINT8(4, call_mcid[0]);
    for (i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(CALL_FRAME, calls[1 + i]);
        TEST_ASSERT_EQUAL_UINT8(i, frame_byte[1 + i]);
    }
    TEST_ASSERT_EQUAL(CALL_DESTROY, calls[11]);
    TEST_ASSERT_EQUAL_PTR(&the_instance, self_seen);
    wait_until_idle();
}

static void test_a_gap_reaches_the_function(void)
{
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_FRAME, 4, 1, false));
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_FRAME, 4, 2, true));
    wait_for_calls(3);
    TEST_ASSERT_FALSE(frame_gap[1]);
    TEST_ASSERT_TRUE(frame_gap[2]);
}

static void test_a_message_arrives_with_its_topic_and_the_readback_of_the_slave(void)
{
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    TEST_ASSERT_TRUE(post_message(4, "lamp/set", "ON", HUBUS_READBACK_CACHE));
    TEST_ASSERT_TRUE(post_message(4, "lamp/set", "", HUBUS_READBACK_SLAVE));
    wait_for_calls(2);
    TEST_ASSERT_EQUAL(CALL_MESSAGE, calls[1]);
    TEST_ASSERT_EQUAL(HUBUS_READBACK_CACHE, call_readback[1]);
    wait_for_calls(3);
    TEST_ASSERT_EQUAL(HUBUS_READBACK_SLAVE, call_readback[2]);
    TEST_ASSERT_EQUAL_STRING("lamp/set", message_topic);
    TEST_ASSERT_EQUAL_UINT(0, message_len);
    TEST_ASSERT_EQUAL_STRING("", message_payload);
    TEST_ASSERT_EQUAL_PTR(&the_instance, self_seen);
}

static void test_the_job_queue_is_bounded_and_nothing_blocks(void)
{
    unsigned i;

    block(true);
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    wait_for_calls(1); /* the thread hangs in create: the queue only fills up from here */
    TEST_ASSERT_TRUE(hubusd_worker_busy(worker));
    for (i = 0; i < HUBUSD_WORKER_JOBS; i++) {
        TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_FRAME, 4, 0, false));
    }
    TEST_ASSERT_FALSE(post_kind(HUBUSD_JOB_FRAME, 4, 0, false));
}

static void test_frames_without_an_instance_are_not_passed_on(void)
{
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_FRAME, 4, 0, false));
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    wait_for_calls(1);
    TEST_ASSERT_EQUAL(CALL_CREATE, calls[0]);
}

static void test_create_that_fails_is_reported_and_gets_no_calls(void)
{
    hubusd_out_t o;

    create_fails = true;
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    TEST_ASSERT_TRUE(post_message(4, "lamp/set", "ON", HUBUS_READBACK_SLAVE));
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 5, 0, false)); /* passes when the message is done */
    wait_for_calls(2);
    TEST_ASSERT_EQUAL_UINT8(4, take(HUBUSD_OUT_CREATE_FAILED).mcid);
    o = take(HUBUSD_OUT_NOT_RUNNING);
    TEST_ASSERT_EQUAL_UINT8(4, o.mcid);
    TEST_ASSERT_EQUAL_STRING("lamp/set", o.topic);
    TEST_ASSERT_EQUAL(CALL_CREATE, calls[1]);
    TEST_ASSERT_EQUAL_UINT8(5, call_mcid[1]);
}

static void test_the_host_checks_what_a_function_produces(void)
{
    static const hubus_module_status_t expected[] = {
        HUBUS_MODULE_OK,       HUBUS_MODULE_REJECTED, HUBUS_MODULE_REJECTED,            /* send */
        HUBUS_MODULE_OK,       HUBUS_MODULE_OK,       HUBUS_MODULE_REJECTED,            /* subscribe */
        HUBUS_MODULE_OK,       HUBUS_MODULE_REJECTED, HUBUS_MODULE_REJECTED,            /* publish */
        HUBUS_MODULE_OK,       HUBUS_MODULE_REJECTED, HUBUS_MODULE_REJECTED, HUBUS_MODULE_REJECTED, /* discovery */
        HUBUS_MODULE_OK,                                                                /* reject */
    };
    hubusd_out_t o;
    unsigned i;

    check_host = true;
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    TEST_ASSERT_TRUE(post_message(4, "lamp/set", "ON", HUBUS_READBACK_SLAVE));
    wait_for_calls(2);
    for (i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(expected[i], results[i], "result of a host function");
    }
    TEST_ASSERT_EQUAL_STRING("hubus/slave/4/lamp/state", full_topic);
    TEST_ASSERT_EQUAL_UINT(strlen(full_topic), full_len);
    TEST_ASSERT_EQUAL_UINT(0, small_len);

    o = take(HUBUSD_OUT_SEND);
    TEST_ASSERT_EQUAL_UINT8(4, o.mcid);
    TEST_ASSERT_EQUAL_HEX8(0x10, o.opcode);
    TEST_ASSERT_EQUAL_UINT8(1, o.n);
    TEST_ASSERT_EQUAL_HEX8(0x55, o.args[0]);
    TEST_ASSERT_EQUAL_STRING("lamp/+", take(HUBUSD_OUT_SUBSCRIBE).topic); /* once */
    o = take(HUBUSD_OUT_PUBLISH);
    TEST_ASSERT_EQUAL_STRING("lamp/state", o.topic);
    TEST_ASSERT_EQUAL_UINT(2, o.len);
    TEST_ASSERT_EQUAL_STRING("ON", o.payload);
    TEST_ASSERT_TRUE(o.retain);
    o = take(HUBUSD_OUT_DISCOVERY);
    TEST_ASSERT_EQUAL_STRING("light", o.component);
    TEST_ASSERT_EQUAL_STRING("lamp", o.topic);
    TEST_ASSERT_EQUAL_STRING("{\"name\":\"Lamp\"}", o.payload);
    TEST_ASSERT_EQUAL_STRING("no", take(HUBUSD_OUT_REJECT).payload);
    TEST_ASSERT_FALSE(hubusd_worker_take(worker, &o));
}

static void test_settings_come_with_create_and_saves_go_to_the_master(void)
{
    static const hubus_module_status_t expected[] = {
        HUBUS_MODULE_OK, HUBUS_MODULE_OK, HUBUS_MODULE_REJECTED, HUBUS_MODULE_REJECTED, HUBUS_MODULE_OK, HUBUS_MODULE_OK,
        HUBUS_MODULE_REJECTED,
    };
    hubusd_job_t job;
    hubusd_out_t o;
    unsigned i;

    memset(&job, 0, sizeof job);
    job.kind = HUBUSD_JOB_CREATE;
    job.mcid = 4;
    job.settings.n = 1;
    snprintf(job.settings.item[0].key, sizeof job.settings.item[0].key, "level");
    snprintf(job.settings.item[0].value, sizeof job.settings.item[0].value, "42");
    TEST_ASSERT_TRUE(hubusd_worker_post(worker, &job));
    check_settings = true;
    TEST_ASSERT_TRUE(post_message(4, "lamp/set", "ON", HUBUS_READBACK_SLAVE));
    wait_for_calls(2);
    check_settings = false;
    for (i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(expected[i], setting_results[i], "result of save");
    }
    TEST_ASSERT_EQUAL_UINT(2, setting_len[0]);
    TEST_ASSERT_EQUAL_STRING("42", setting_seen[0]);
    TEST_ASSERT_EQUAL_UINT(0, setting_len[1]);
    TEST_ASSERT_EQUAL_STRING("9", setting_seen[2]);
    TEST_ASSERT_EQUAL_UINT(0, setting_len[3]);
    TEST_ASSERT_EQUAL_STRING("", setting_seen[3]);

    o = take(HUBUSD_OUT_SAVE);
    TEST_ASSERT_EQUAL_UINT8(4, o.mcid);
    TEST_ASSERT_EQUAL_STRING("level", o.topic);
    TEST_ASSERT_EQUAL_STRING("9", o.payload);
    TEST_ASSERT_EQUAL_UINT8(0, o.n);
    o = take(HUBUSD_OUT_SAVE); /* the removal; the same value twice was written once */
    TEST_ASSERT_EQUAL_STRING("level", o.topic);
    TEST_ASSERT_EQUAL_UINT8(1, o.n);
    for (i = 0; i < HUBUS_MODULE_SETTINGS_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, take(HUBUSD_OUT_SAVE).n);
    }
    TEST_ASSERT_FALSE(hubusd_worker_take(worker, &o));
}

static void test_an_instance_takes_a_limited_number_of_topics_and_loses_them_with_destroy(void)
{
    char t[16];
    hubusd_out_t o;
    unsigned i;

    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    wait_for_calls(1);
    wait_until_idle(); /* the test calls the host itself from here on, while the thread waits */
    for (i = 0; i < HUBUS_MODULE_TOPICS_MAX; i++) {
        snprintf(t, sizeof t, "t%u/set", i);
        TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, last_host->subscribe(last_host, t));
        TEST_ASSERT_EQUAL_STRING(t, take(HUBUSD_OUT_SUBSCRIBE).topic);
    }
    TEST_ASSERT_EQUAL(HUBUS_MODULE_REJECTED, last_host->subscribe(last_host, "one/more"));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, last_host->unsubscribe(last_host, "t3/set"));
    TEST_ASSERT_EQUAL_STRING("t3/set", take(HUBUSD_OUT_UNSUBSCRIBE).topic);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, last_host->unsubscribe(last_host, "t3/set")); /* not taken: nothing to do */
    TEST_ASSERT_FALSE(hubusd_worker_take(worker, &o));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, last_host->subscribe(last_host, "one/more"));
    take(HUBUSD_OUT_SUBSCRIBE);

    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_DESTROY, 4, 0, false));
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    wait_for_calls(3);
    wait_until_idle();
    TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, last_host->subscribe(last_host, "t0/set")); /* a new instance starts empty */
    TEST_ASSERT_EQUAL_STRING("t0/set", take(HUBUSD_OUT_SUBSCRIBE).topic);
}

static void test_a_full_output_queue_tells_the_function(void)
{
    static const uint8_t arg[1] = {1};
    hubusd_out_t o;
    unsigned i;

    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    wait_for_calls(1);
    wait_until_idle();
    for (i = 0; i < HUBUSD_WORKER_OUT; i++) {
        TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, last_host->send(last_host, 0x10, arg, 1));
    }
    TEST_ASSERT_EQUAL(HUBUS_MODULE_FAILED, last_host->send(last_host, 0x10, arg, 1));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_FAILED, last_host->subscribe(last_host, "a/set"));
    TEST_ASSERT_TRUE(hubusd_worker_busy(worker)); /* output waits */
    TEST_ASSERT_TRUE(hubusd_worker_take(worker, &o));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, last_host->subscribe(last_host, "a/set")); /* it was not taken before */
}

static void test_a_call_over_the_limit_fails_the_module_once(void)
{
    block(true);
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    wait_for_calls(1);

    TEST_ASSERT_FALSE(hubusd_worker_check(worker, 1000, 100)); /* the first look starts the time */
    TEST_ASSERT_FALSE(hubusd_worker_check(worker, 1099, 100));
    TEST_ASSERT_FALSE(hubusd_worker_failed(worker));
    TEST_ASSERT_TRUE(hubusd_worker_check(worker, 1100, 100));
    TEST_ASSERT_TRUE(hubusd_worker_failed(worker));
    TEST_ASSERT_FALSE(hubusd_worker_check(worker, 2000, 100));
    TEST_ASSERT_FALSE(post_kind(HUBUSD_JOB_FRAME, 4, 0, false));
    TEST_ASSERT_FALSE(hubusd_worker_busy(worker)); /* nothing that a failed module does counts */

    block(false); /* comes back late: it gets no more calls, not even destroy */
    TEST_ASSERT_TRUE(hubusd_worker_stop(worker, 5000));
    worker = NULL;
    TEST_ASSERT_EQUAL_UINT(1, n_calls);
    TEST_ASSERT_FALSE(hubusd_worker_abandoned(&module));
}

static void test_a_new_call_starts_a_new_time(void)
{
    TEST_ASSERT_FALSE(hubusd_worker_check(worker, 500, 100)); /* no call running */
    block(true);
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    wait_for_calls(1);
    TEST_ASSERT_FALSE(hubusd_worker_check(worker, 1000, 100));
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_FRAME, 4, 0, false));
    block(false);
    wait_for_calls(2);
    block(true);
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_FRAME, 4, 0, false));
    wait_for_calls(3);
    TEST_ASSERT_FALSE(hubusd_worker_check(worker, 1500, 100));
    TEST_ASSERT_FALSE(hubusd_worker_failed(worker));
}

static void test_stop_destroys_the_instances_that_are_left(void)
{
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 9, 0, false));
    wait_for_calls(2);
    TEST_ASSERT_TRUE(hubusd_worker_stop(worker, 5000));
    worker = NULL;
    TEST_ASSERT_EQUAL_UINT(4, n_calls);
    TEST_ASSERT_EQUAL(CALL_DESTROY, calls[2]);
    TEST_ASSERT_EQUAL(CALL_DESTROY, calls[3]);
}

/* Last: the thread that hangs stays until the end of the process, and the module counts as abandoned from here on. */
static void test_stop_gives_up_on_a_thread_that_hangs(void)
{
    block(true);
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    wait_for_calls(1);
    TEST_ASSERT_FALSE(hubusd_worker_stop(worker, 20));
    worker = NULL;
    TEST_ASSERT_TRUE(hubusd_worker_abandoned(&module));

    block(false); /* it may end now; its memory is still there */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_calls_run_in_the_order_of_the_jobs);
    RUN_TEST(test_a_gap_reaches_the_function);
    RUN_TEST(test_a_message_arrives_with_its_topic_and_the_readback_of_the_slave);
    RUN_TEST(test_the_job_queue_is_bounded_and_nothing_blocks);
    RUN_TEST(test_frames_without_an_instance_are_not_passed_on);
    RUN_TEST(test_create_that_fails_is_reported_and_gets_no_calls);
    RUN_TEST(test_the_host_checks_what_a_function_produces);
    RUN_TEST(test_settings_come_with_create_and_saves_go_to_the_master);
    RUN_TEST(test_an_instance_takes_a_limited_number_of_topics_and_loses_them_with_destroy);
    RUN_TEST(test_a_full_output_queue_tells_the_function);
    RUN_TEST(test_a_call_over_the_limit_fails_the_module_once);
    RUN_TEST(test_a_new_call_starts_a_new_time);
    RUN_TEST(test_stop_destroys_the_instances_that_are_left);
    RUN_TEST(test_stop_gives_up_on_a_thread_that_hangs);
    return UNITY_END();
}
