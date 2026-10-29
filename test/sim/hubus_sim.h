/*
 * Bus simulation: master-driven CLK, open-drain wired-AND DATA, N slave cores.
 *
 * The master code runs unchanged on the simulated bus through its HAL. The
 * slaves are the real `hubus_slave_core_t`. After every line change the
 * simulation feeds the resulting pin levels to all slaves and re-resolves the
 * wired-AND until the line is stable.
 *
 * The simulation also checks protocol invariants (a slave must never change
 * DATA while CLK is high) and, optionally, master timing minimums.
 */
#ifndef HUBUS_SIM_H
#define HUBUS_SIM_H

#include <stdint.h>
#include <stdio.h>

#include "hubus_master.h"
#include "hubus_slave_core.h"

#define HUBUS_SIM_MAX_SLAVES 64u

typedef struct hubus_sim hubus_sim_t;

/*
 * Fault injection: called for every slave before pin levels are delivered.
 * The hook may change the levels the slave sees (`*clk`, `*data`) or return 0
 * to skip the delivery. Typical uses: present CLK low during one pulse (lost
 * pulse), flip `*data` at one rising edge (bit error).
 */
typedef int (*hubus_sim_filter_fn)(hubus_sim_t *sim, unsigned slave, uint8_t *clk, uint8_t *data, void *ctx);

/* Fault injection on the master side: may change the DATA level the master reads. */
typedef uint8_t (*hubus_sim_read_filter_fn)(hubus_sim_t *sim, uint8_t level, void *ctx);

struct hubus_sim {
    hubus_slave_core_t *slave[HUBUS_SIM_MAX_SLAVES];
    uint8_t slave_low[HUBUS_SIM_MAX_SLAVES]; /* last DATA drive of each slave */
    uint8_t unplugged[HUBUS_SIM_MAX_SLAVES]; /* slave disconnected: sees no edges, does not drive DATA */
    unsigned n_slaves;

    uint8_t clk;         /* CLK driven by the master */
    uint8_t master_low;  /* master pulls DATA low */
    uint8_t extra_low;   /* stuck line / foreign node pulling DATA low */
    uint8_t data;        /* resolved DATA level, 1 = high */

    uint64_t time_ns;

    /* event counters (useful for fault injection) */
    unsigned clk_rises;
    unsigned clk_falls;
    unsigned n_start;
    unsigned n_stop;
    unsigned n_reset;     /* START immediately followed by STOP */
    unsigned data_edges;

    /* invariant violations */
    unsigned violations;        /* a slave changed DATA while CLK was high */
    unsigned timing_violations; /* master timing below the configured minimums */

    /* timing minimums to check (0 = not checked) */
    uint32_t min_clk_low_ns;
    uint32_t min_clk_high_ns;
    uint32_t min_cond_ns;

    /* Pin handler per slave, NULL: hubus_core_pins. Lets a test run a core built with other options on the same
     * bus, as long as `data_low` is at the same place in its structure. */
    void (*pins[HUBUS_SIM_MAX_SLAVES])(hubus_slave_core_t *core, uint8_t clk, uint8_t data);

    hubus_sim_filter_fn filter;
    void *filter_ctx;

    hubus_sim_read_filter_fn read_filter;
    void *read_filter_ctx;

    /* Slaves that run in time (simulated microcontrollers, test/avr/): called by every master delay with its end
     * time. The hook runs those slaves up to `until_ns`, advances `time_ns` as it goes, and settles the line after
     * each of their DATA changes. NULL: slaves react instantly. */
    void (*advance)(hubus_sim_t *sim, uint64_t until_ns, void *ctx);
    void *advance_ctx;
    unsigned master_reads; /* number of DATA reads by the master so far */

    FILE *trace; /* optional text trace */

    /* internal */
    uint64_t t_rise;
    uint64_t t_fall;
    uint64_t t_cond;
    uint8_t cond_since_clk; /* a START/STOP happened since the last CLK edge */
    uint8_t last_was_start;
};

void hubus_sim_init(hubus_sim_t *sim);

/* Adds a slave and delivers the current line levels to it. Returns its index. */
unsigned hubus_sim_add(hubus_sim_t *sim, hubus_slave_core_t *slave);

/* The master HAL bound to this simulation. */
hubus_master_hal_t hubus_sim_master_hal(hubus_sim_t *sim);

/* Re-evaluates the line after an external change (slave write, fault). */
void hubus_sim_settle(hubus_sim_t *sim);

#if HUBUS_ENABLE_EVENTS
/* Application write on a slave, with the line update that may follow. */
uint8_t hubus_sim_slave_write(hubus_sim_t *sim, unsigned slave, const uint8_t *data, uint8_t len, uint8_t more);
#endif

/* Disconnects a slave from the bus (unplugged = 1) or connects it again with the state it had (0). */
void hubus_sim_unplug(hubus_sim_t *sim, unsigned slave, int unplugged);

/* A spurious CLK pulse seen only by one slave (extra clock edge). */
void hubus_sim_glitch_clk(hubus_sim_t *sim, unsigned slave);

/* Checks timing minimums on the master side. */
void hubus_sim_check_timing(hubus_sim_t *sim, uint32_t clk_low_ns, uint32_t clk_high_ns, uint32_t cond_ns);

#endif /* HUBUS_SIM_H */
