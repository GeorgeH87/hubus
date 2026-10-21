/*
 * Test fixture: the real master on the simulated bus with N real slave cores.
 */
#ifndef HUBUS_FIXTURE_H
#define HUBUS_FIXTURE_H

#include <string.h>

#include "hubus_master.h"
#include "hubus_sim.h"
#include "unity.h"

#define FX_MAX_SLAVES HUBUS_SIM_MAX_SLAVES
#define FX_LOG_MAX 512

typedef struct {
    hubus_slave_core_t *core;
    unsigned captures;
    unsigned commands;
    uint8_t last_op;
    uint8_t last_args[HUBUS_MAX_PAYLOAD];
    uint8_t last_len;
    uint8_t status;
    uint8_t info[4];
} fx_app_t;

typedef struct {
    uint8_t mcid;
    uint8_t gen;
    uint8_t more;
    uint8_t len;
    uint8_t first; /* first payload byte */
    hubus_gen_result_t result;
    uint8_t lost;
} fx_rec_t;

typedef struct {
    hubus_sim_t sim;
    hubus_master_t master;
    hubus_master_cfg_t cfg;
    hubus_slave_core_t core[FX_MAX_SLAVES];
    fx_app_t app[FX_MAX_SLAVES];
    unsigned n;
    fx_rec_t log[FX_LOG_MAX];
    unsigned n_log;
    void (*on_record)(void *ctx, const hubus_slave_frame_t *f);
    void *on_record_ctx;
} fx_t;

static inline void fx_on_capture(void *ctx)
{
    fx_app_t *a = ctx;

    a->captures++;
    (void)hubus_core_write(a->core, &a->status, 1, 0);
}

static inline void fx_on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    fx_app_t *a = ctx;

    a->commands++;
    a->last_op = opcode;
    a->last_len = len;
    if (len) {
        memcpy(a->last_args, args, len);
    }
}

/* Creates the bus and the master (timing checks enabled). Call fx_startup() to run the startup sequence. */
static inline void fx_init(fx_t *fx)
{
    hubus_master_hal_t hal;

    memset(fx, 0, sizeof *fx);
    hubus_sim_init(&fx->sim);
    hubus_master_cfg_default(&fx->cfg);
    hal = hubus_sim_master_hal(&fx->sim);
    hubus_master_init(&fx->master, &hal, &fx->cfg);
    hubus_sim_check_timing(&fx->sim, fx->cfg.t_clk_low_ns, fx->cfg.t_clk_high_ns, fx->cfg.t_cond_ns);
}

/* Adds a slave with a device-unique serial in SLAVE_INFO. Returns its index. */
static inline unsigned fx_add_ex(fx_t *fx, uint8_t mcid, int capture, uint8_t serial)
{
    unsigned idx = fx->n++;
    hubus_core_config_t cfg;

    TEST_ASSERT_TRUE(idx < FX_MAX_SLAVES);
    fx->app[idx].core = &fx->core[idx];
    fx->app[idx].status = (uint8_t)(0x40 + mcid);
    fx->app[idx].info[0] = 0xD0;
    fx->app[idx].info[1] = 0x01;
    fx->app[idx].info[2] = 0x00;
    fx->app[idx].info[3] = serial;
    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = mcid;
    cfg.info = fx->app[idx].info;
    cfg.info_len = sizeof fx->app[idx].info;
    cfg.on_status_capture = capture ? fx_on_capture : NULL;
    cfg.on_command = fx_on_command;
    cfg.ctx = &fx->app[idx];
    TEST_ASSERT_TRUE(hubus_core_init(&fx->core[idx], &cfg, fx->sim.clk, fx->sim.data));
    TEST_ASSERT_EQUAL_UINT(idx, hubus_sim_add(&fx->sim, &fx->core[idx]));
    return idx;
}

static inline unsigned fx_add(fx_t *fx, uint8_t mcid)
{
    return fx_add_ex(fx, mcid, 1, mcid);
}

/* Application write of one byte on slave `idx`. */
static inline void fx_write(fx_t *fx, unsigned idx, uint8_t value)
{
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&fx->sim, idx, &value, 1, 0));
}

static inline void fx_startup(fx_t *fx)
{
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_startup(&fx->master));
}

static inline void fx_collect(void *ctx, const hubus_slave_frame_t *f, hubus_gen_result_t res, uint8_t lost)
{
    fx_t *fx = ctx;

    TEST_ASSERT_TRUE(fx->n_log < FX_LOG_MAX);
    fx->log[fx->n_log].mcid = f->mcid;
    fx->log[fx->n_log].gen = f->gen;
    fx->log[fx->n_log].more = f->more;
    fx->log[fx->n_log].len = f->len;
    fx->log[fx->n_log].first = f->len ? f->payload[0] : 0;
    fx->log[fx->n_log].result = res;
    fx->log[fx->n_log].lost = lost;
    fx->n_log++;
    if (fx->on_record) {
        fx->on_record(fx->on_record_ctx, f);
    }
}

static inline hubus_status_t fx_burst(fx_t *fx, hubus_burst_stats_t *st)
{
    fx->n_log = 0;
    return hubus_master_burst(&fx->master, fx_collect, fx, st);
}

/* Invariants that must hold after every test. */
static inline void fx_check(const fx_t *fx)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, fx->sim.violations, "a slave changed DATA while CLK was high");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, fx->sim.timing_violations, "master timing minimums violated");
}

#endif /* HUBUS_FIXTURE_H */
