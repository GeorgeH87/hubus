#include "hubus_sim.h"

#include <stdlib.h>
#include <string.h>

static void timing_violation(hubus_sim_t *sim, int line)
{
    sim->timing_violations++;
    if (sim->trace) {
        fprintf(sim->trace, "%10llu ns  TIMING VIOLATION (hubus_sim.c:%d)\n", (unsigned long long)sim->time_ns, line);
    }
}

static uint8_t resolve(const hubus_sim_t *sim)
{
    unsigned i;

    if (sim->master_low || sim->extra_low) {
        return 0;
    }
    for (i = 0; i < sim->n_slaves; i++) {
        if (hubus_core_data_low(sim->slave[i])) {
            return 0;
        }
    }
    return 1;
}

static void deliver_all(hubus_sim_t *sim)
{
    unsigned i;

    for (i = 0; i < sim->n_slaves; i++) {
        uint8_t clk = sim->clk;
        uint8_t data = sim->data;

        if (sim->filter && !sim->filter(sim, i, &clk, &data, sim->filter_ctx)) {
            continue;
        }
        hubus_core_pins(sim->slave[i], clk, data);
    }
}

static void note_data_change(hubus_sim_t *sim, uint8_t new_data)
{
    sim->data_edges++;
    if (sim->trace) {
        fprintf(sim->trace, "%10llu ns  DATA=%u\n", (unsigned long long)sim->time_ns, new_data);
    }
    if (!sim->clk) {
        return;
    }
    /* A resolved DATA change while CLK is high is a line condition. */
    if (new_data) {
        sim->n_stop++;
        if (sim->last_was_start) {
            sim->n_reset++;
        }
        sim->last_was_start = 0;
    } else {
        sim->n_start++;
        sim->last_was_start = 1;
    }
    if (sim->min_cond_ns) {
        uint64_t ref = sim->cond_since_clk ? sim->t_cond : sim->t_rise;
        if (sim->time_ns - ref < sim->min_cond_ns) {
            timing_violation(sim, __LINE__);
        }
    }
    sim->t_cond = sim->time_ns;
    sim->cond_since_clk = 1;
}

void hubus_sim_settle(hubus_sim_t *sim)
{
    int iter;

    for (iter = 0; iter < 16; iter++) {
        uint8_t prev = sim->data;
        unsigned i;
        int slave_changed = 0;

        sim->data = resolve(sim);
        if (sim->data != prev) {
            note_data_change(sim, sim->data);
        }
        deliver_all(sim);

        for (i = 0; i < sim->n_slaves; i++) {
            uint8_t low = hubus_core_data_low(sim->slave[i]);
            if (low != sim->slave_low[i]) {
                sim->slave_low[i] = low;
                slave_changed = 1;
                if (sim->clk) {
                    sim->violations++; /* slaves may only change DATA while CLK is low */
                    if (sim->trace) {
                        fprintf(sim->trace, "VIOLATION: slave %u changed DATA while CLK high\n", i);
                    }
                }
            }
        }
        if (!slave_changed && resolve(sim) == sim->data) {
            return;
        }
    }
    fprintf(stderr, "hubus_sim: line did not settle\n");
    abort();
}

void hubus_sim_init(hubus_sim_t *sim)
{
    memset(sim, 0, sizeof *sim);
    sim->data = 1;
    if (getenv("HUBUS_SIM_TRACE")) {
        sim->trace = stderr;
    }
}

unsigned hubus_sim_add(hubus_sim_t *sim, hubus_slave_core_t *slave)
{
    unsigned idx = sim->n_slaves;

    if (idx >= HUBUS_SIM_MAX_SLAVES) {
        fprintf(stderr, "hubus_sim: too many slaves\n");
        abort();
    }
    sim->slave[idx] = slave;
    sim->slave_low[idx] = hubus_core_data_low(slave);
    sim->n_slaves++;
    hubus_sim_settle(sim);
    return idx;
}

/* ---- master HAL ----------------------------------------------------- */

static void hal_set_clk(void *ctx, uint8_t high)
{
    hubus_sim_t *sim = ctx;

    high = high ? 1u : 0u;
    if (high == sim->clk) {
        return;
    }
    if (high) {
        if (sim->min_clk_low_ns && sim->time_ns - sim->t_fall < sim->min_clk_low_ns && sim->clk_falls > 0) {
            timing_violation(sim, __LINE__);
        }
        sim->t_rise = sim->time_ns;
        sim->clk_rises++;
    } else {
        if (sim->min_clk_high_ns && sim->time_ns - sim->t_rise < sim->min_clk_high_ns) {
            timing_violation(sim, __LINE__);
        }
        if (sim->min_cond_ns && sim->cond_since_clk && sim->time_ns - sim->t_cond < sim->min_cond_ns) {
            timing_violation(sim, __LINE__);
        }
        sim->t_fall = sim->time_ns;
        sim->clk_falls++;
    }
    sim->cond_since_clk = 0;
    sim->last_was_start = 0;
    sim->clk = high;
    if (sim->trace) {
        fprintf(sim->trace, "%10llu ns  CLK=%u\n", (unsigned long long)sim->time_ns, high);
    }
    hubus_sim_settle(sim);
}

static void hal_set_data_low(void *ctx, uint8_t low)
{
    hubus_sim_t *sim = ctx;

    low = low ? 1u : 0u;
    if (low == sim->master_low) {
        return;
    }
    sim->master_low = low;
    hubus_sim_settle(sim);
}

static uint8_t hal_read_data(void *ctx)
{
    hubus_sim_t *sim = ctx;
    uint8_t level = sim->data;

    sim->master_reads++;
    if (sim->read_filter) {
        level = sim->read_filter(sim, level, sim->read_filter_ctx) ? 1u : 0u;
    }
    return level;
}

static void hal_delay(void *ctx, uint32_t ns)
{
    hubus_sim_t *sim = ctx;

    sim->time_ns += ns;
}

hubus_master_hal_t hubus_sim_master_hal(hubus_sim_t *sim)
{
    hubus_master_hal_t hal;

    hal.ctx = sim;
    hal.set_clk = hal_set_clk;
    hal.set_data_low = hal_set_data_low;
    hal.read_data = hal_read_data;
    hal.delay_ns = hal_delay;
    return hal;
}

uint8_t hubus_sim_slave_write(hubus_sim_t *sim, unsigned slave, const uint8_t *data, uint8_t len, uint8_t more)
{
    uint8_t r = hubus_core_write(sim->slave[slave], data, len, more);

    hubus_sim_settle(sim);
    return r;
}

void hubus_sim_glitch_clk(hubus_sim_t *sim, unsigned slave)
{
    hubus_core_pins(sim->slave[slave], 1, sim->data);
    hubus_core_pins(sim->slave[slave], 0, sim->data);
    hubus_sim_settle(sim);
}

void hubus_sim_check_timing(hubus_sim_t *sim, uint32_t clk_low_ns, uint32_t clk_high_ns, uint32_t cond_ns)
{
    sim->min_clk_low_ns = clk_low_ns;
    sim->min_clk_high_ns = clk_high_ns;
    sim->min_cond_ns = cond_ns;
}
