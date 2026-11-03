/*
 * The thread of a master module (daemon/hubusd_worker.h), on its own: order of the calls, bounded queues, what the host
 * functions accept, the time limit, and stopping a thread that hangs.
 */
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "hubusd_worker.h"
#include "unity.h"

#define MAX_CALLS 64

typedef enum { CALL_CREATE, CALL_DESTROY, CALL_FRAME, CALL_SET } call_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static call_t calls[MAX_CALLS];
static uint8_t call_mcid[MAX_CALLS];
static uint8_t frame_byte[MAX_CALLS];
static bool frame_gap[MAX_CALLS];
static unsigned n_calls;
static bool block_calls; /* functions wait here until the test lets them go */
static bool create_fails;
static hubus_module_status_t set_result;
static hubus_module_status_t send_result[3];
static hubus_module_status_t state_result[3];
static const hubus_module_host_t *last_host;
static hubus_module_instance_t *self_seen;
static int the_instance;

static void record(call_t c, uint8_t mcid, uint8_t byte, bool gap)
{
    pthread_mutex_lock(&lock);
    if (n_calls < MAX_CALLS) {
        calls[n_calls] = c;
        call_mcid[n_calls] = mcid;
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
    record(CALL_CREATE, host->mcid, 0, false);
    return create_fails ? NULL : (hubus_module_instance_t *)&the_instance;
}

static void m_destroy(hubus_module_instance_t *self)
{
    self_seen = self;
    record(CALL_DESTROY, 0, 0, false);
}

static void m_frame(hubus_module_instance_t *self, const hubus_module_host_t *host, hubus_frame_kind_t kind,
                    const uint8_t *data, uint8_t len, bool gap)
{
    (void)kind;
    self_seen = self;
    record(CALL_FRAME, host->mcid, len ? data[0] : 0, gap);
}

static hubus_module_status_t m_set(hubus_module_instance_t *self, const hubus_module_host_t *host,
                                   const hubus_entity_t *entity, const hubus_value_t *value)
{
    static const uint8_t arg[HUBUS_MAX_PAYLOAD] = {0x55};
    char long_text[HUBUSD_STATE_TEXT_LEN + 1u];

    (void)entity;
    (void)value;
    self_seen = self;
    memset(long_text, 'x', sizeof long_text - 1u);
    long_text[sizeof long_text - 1u] = '\0';
    send_result[0] = host->send(host, 0x10, arg, 1);
    send_result[1] = host->send(host, 0x05, NULL, 0); /* reserved opcode */
    send_result[2] = host->send(host, 0x10, arg, HUBUS_MAX_PAYLOAD); /* one byte too much with the opcode */
    state_result[0] = host->set_state(host, "lamp", "ON");
    state_result[1] = host->set_state(host, "nothing", "ON");
    state_result[2] = host->set_state(host, "lamp", long_text);
    record(CALL_SET, host->mcid, 0, false);
    return set_result;
}

static const hubus_module_rule_t rules[] = {{.pattern = {0xF0}, .mask = {0xFF}, .len = 1}};
static const hubus_entity_t entities[] = {{.key = "lamp", .name = "Lamp", .component = HUBUS_COMPONENT_LIGHT}};
static const hubus_module_t module = {
    HUBUS_MODULE_INIT,
    .name = "worker_test",
    .rules = rules,
    .n_rules = 1,
    .max_command = HUBUS_MAX_PAYLOAD,
    .entities = entities,
    .n_entities = 1,
    .create = m_create,
    .destroy = m_destroy,
    .on_frame = m_frame,
    .on_set = m_set,
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
    job.value.type = HUBUS_VALUE_BOOL;
    return hubusd_worker_post(worker, &job);
}

void setUp(void)
{
    n_calls = 0;
    block_calls = false;
    create_fails = false;
    set_result = HUBUS_MODULE_OK;
    self_seen = NULL;
    worker = hubusd_worker_start(&module);
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

static void test_the_job_queue_is_bounded_and_nothing_blocks(void)
{
    unsigned i;

    block(true);
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    wait_for_calls(1); /* the thread hangs in create: the queue only fills up from here */
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
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_SET, 4, 0, false));
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 5, 0, false)); /* passes when the set is done */
    wait_for_calls(2);
    TEST_ASSERT_TRUE(hubusd_worker_take(worker, &o));
    TEST_ASSERT_EQUAL(HUBUSD_OUT_CREATE_FAILED, o.kind);
    TEST_ASSERT_EQUAL_UINT8(4, o.mcid);
    TEST_ASSERT_TRUE(hubusd_worker_take(worker, &o));
    TEST_ASSERT_EQUAL(HUBUSD_OUT_NOT_RUNNING, o.kind);
    TEST_ASSERT_EQUAL(CALL_CREATE, calls[1]);
    TEST_ASSERT_EQUAL_UINT8(5, call_mcid[1]);
}

static void test_the_host_checks_what_a_function_produces(void)
{
    hubusd_out_t o;

    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_SET, 4, 0, false));
    wait_for_calls(2);
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_DESTROY, 4, 0, false));
    wait_for_calls(3);

    TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, send_result[0]);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_REJECTED, send_result[1]);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_REJECTED, send_result[2]);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, state_result[0]);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_REJECTED, state_result[1]);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_REJECTED, state_result[2]);

    TEST_ASSERT_TRUE(hubusd_worker_take(worker, &o));
    TEST_ASSERT_EQUAL(HUBUSD_OUT_SEND, o.kind);
    TEST_ASSERT_EQUAL_UINT8(4, o.mcid);
    TEST_ASSERT_EQUAL_HEX8(0x10, o.opcode);
    TEST_ASSERT_EQUAL_UINT8(1, o.n);
    TEST_ASSERT_EQUAL_HEX8(0x55, o.args[0]);
    TEST_ASSERT_TRUE(hubusd_worker_take(worker, &o));
    TEST_ASSERT_EQUAL(HUBUSD_OUT_STATE, o.kind);
    TEST_ASSERT_EQUAL_UINT8(0, o.entity);
    TEST_ASSERT_EQUAL_STRING("ON", o.text);
    TEST_ASSERT_FALSE(hubusd_worker_take(worker, &o));
}

static void test_a_refused_value_is_reported(void)
{
    hubusd_out_t o;

    set_result = HUBUS_MODULE_REJECTED;
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_SET, 4, 0, false));
    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_DESTROY, 4, 0, false));
    wait_for_calls(3);
    TEST_ASSERT_TRUE(hubusd_worker_take(worker, &o)); /* the command of the function */
    TEST_ASSERT_TRUE(hubusd_worker_take(worker, &o)); /* the state of the function */
    TEST_ASSERT_TRUE(hubusd_worker_take(worker, &o));
    TEST_ASSERT_EQUAL(HUBUSD_OUT_SET_REFUSED, o.kind);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_REJECTED, o.status);
}

static void test_a_full_output_queue_tells_the_function(void)
{
    static const uint8_t arg[1] = {1};
    hubusd_out_t o;
    unsigned i;

    TEST_ASSERT_TRUE(post_kind(HUBUSD_JOB_CREATE, 4, 0, false));
    wait_for_calls(1);
    for (i = 0; i < HUBUSD_WORKER_OUT; i++) {
        TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, last_host->send(last_host, 0x10, arg, 1));
    }
    TEST_ASSERT_EQUAL(HUBUS_MODULE_FAILED, last_host->send(last_host, 0x10, arg, 1));
    TEST_ASSERT_TRUE(hubusd_worker_take(worker, &o));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, last_host->send(last_host, 0x10, arg, 1));
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
    RUN_TEST(test_the_job_queue_is_bounded_and_nothing_blocks);
    RUN_TEST(test_frames_without_an_instance_are_not_passed_on);
    RUN_TEST(test_create_that_fails_is_reported_and_gets_no_calls);
    RUN_TEST(test_the_host_checks_what_a_function_produces);
    RUN_TEST(test_a_refused_value_is_reported);
    RUN_TEST(test_a_full_output_queue_tells_the_function);
    RUN_TEST(test_a_call_over_the_limit_fails_the_module_once);
    RUN_TEST(test_a_new_call_starts_a_new_time);
    RUN_TEST(test_stop_destroys_the_instances_that_are_left);
    RUN_TEST(test_stop_gives_up_on_a_thread_that_hangs);
    return UNITY_END();
}
