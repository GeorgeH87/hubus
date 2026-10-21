/*
 * HUBUS master (docs/PROTOCOL.md, docs/ARBITRATION.md).
 *
 * All hardware access goes through `hubus_master_hal_t`: set CLK, pull or
 * release DATA, read DATA, wait. The same logic runs on a Raspberry Pi
 * (libgpiod HAL) and against the simulated bus in the host tests.
 *
 * All timing values are minimums. The master owns every timeout, retry, and
 * recovery decision; slaves have none.
 */
#ifndef HUBUS_MASTER_H
#define HUBUS_MASTER_H

#include <stddef.h>
#include <stdint.h>

#include "hubus_arb.h"
#include "hubus_gen.h"
#include "hubus_proto.h"

/* ---- hardware abstraction ------------------------------------------- */

typedef struct {
    void *ctx;
    void (*set_clk)(void *ctx, uint8_t high);        /* CLK, push-pull */
    void (*set_data_low)(void *ctx, uint8_t low);    /* 1: pull DATA low, 0: release (never drive high) */
    uint8_t (*read_data)(void *ctx);                 /* 1: DATA high */
    void (*delay_ns)(void *ctx, uint32_t ns);        /* wait at least `ns` */
} hubus_master_hal_t;

/* ---- configuration -------------------------------------------------- */

typedef struct {
    uint32_t t_clk_low_ns;     /* minimum CLK low time per clock period */
    uint32_t t_clk_high_ns;    /* minimum CLK high time per clock period */
    uint32_t t_sample_ns;      /* delay from CLK rise to reading DATA (<= t_clk_high_ns) */
    uint32_t t_cond_ns;        /* minimum CLK high time around a START/STOP DATA change */
    uint32_t t_turnaround_ns;  /* minimum CLK low time after the turnaround slot */
    uint16_t line_fault_ticks; /* DATA stuck low for this many ticks is a line fault (>= one max frame) */
    uint8_t retry_budget;      /* retries of a failed REQUEST_DATA within a burst */
    uint8_t follow_more;       /* immediately request the next fragment after MORE = 1 */
    uint8_t max_fragments;     /* limit for consecutive fragments from one slave per burst */
    uint8_t dup_fail_threshold;/* consecutive CRC failures of one MCID that raise a suspicion */
    uint8_t line_problem_count;/* more suspected MCIDs than this point to a line problem */
} hubus_master_cfg_t;

/* Conservative defaults for a first bring-up. Frozen after measurements (H3). */
void hubus_master_cfg_default(hubus_master_cfg_t *cfg);

/* ---- results -------------------------------------------------------- */

typedef enum {
    HUBUS_OK = 0,
    HUBUS_ERR_ARG,          /* invalid argument */
    HUBUS_ERR_NO_RESPONSE,  /* silence after the turnaround slot */
    HUBUS_ERR_CRC,          /* response with invalid CRC8 */
    HUBUS_ERR_MISMATCH,     /* valid frame with unexpected type or source MCID */
    HUBUS_ERR_LINE,         /* DATA readback differs from what was transmitted; recovery was run */
    HUBUS_ERR_LINE_FAULT,   /* DATA stuck low: hardware line fault */
    HUBUS_ERR_STACK         /* pending mask stack overflow (corrupted arbitration) */
} hubus_status_t;

const char *hubus_status_str(hubus_status_t s);

/* A frame received from a slave. */
typedef struct {
    uint8_t type;   /* HUBUS_S_DATA or HUBUS_S_INFO */
    uint8_t mcid;
    uint8_t more;
    uint8_t gen;
    uint8_t len;
    uint8_t payload[HUBUS_MAX_PAYLOAD];
} hubus_slave_frame_t;

/* ---- master state --------------------------------------------------- */

typedef struct {
    hubus_master_hal_t hal;
    hubus_master_cfg_t cfg;
    uint8_t clk;   /* CLK level driven by the master */
    uint8_t hold;  /* keep CLK high after STOP (back-to-back transactions in a burst) */
    hubus_mask_stack_t stack;
    hubus_gen_tracker_t gen[HUBUS_MCID_MAX + 1u];
    uint8_t fail[HUBUS_MCID_MAX + 1u]; /* consecutive CRC/mismatch failures per MCID */
} hubus_master_t;

void hubus_master_init(hubus_master_t *m, const hubus_master_hal_t *hal, const hubus_master_cfg_t *cfg);

/* ---- line control --------------------------------------------------- */

/*
 * Startup sequence steps 1-3 (docs/PROTOCOL.md section 3.3): clock until DATA
 * is high, then generate RESET. Also used for error recovery. Does not run
 * the initial burst; call hubus_master_burst() afterwards.
 */
hubus_status_t hubus_master_startup(hubus_master_t *m);

/* Optional shutdown: RESET so that all slaves are idle with DATA released. */
hubus_status_t hubus_master_shutdown(hubus_master_t *m);

/*
 * Idle event handshake (docs/PROTOCOL.md section 3.1): if a slave holds DATA
 * low while the bus is idle, clock until it is released. `*event` is set to 1
 * if an event was confirmed. Call it periodically while idle.
 */
hubus_status_t hubus_master_check_event(hubus_master_t *m, uint8_t *event);

/* ---- transactions --------------------------------------------------- */

/* SEND_COMMAND to a group or a single slave (target_len 0..6). No response. */
hubus_status_t hubus_master_send_command(hubus_master_t *m, uint8_t target_len, uint8_t prefix, uint8_t opcode,
                                         const uint8_t *args, uint8_t args_len);

/* STATUS_CAPTURE (opcode 0x00) to a group or a single slave. */
hubus_status_t hubus_master_status_capture(hubus_master_t *m, uint8_t target_len, uint8_t prefix);

/* REQUEST_INFO / REQUEST_DATA to exactly one slave. */
hubus_status_t hubus_master_request_info(hubus_master_t *m, uint8_t mcid, hubus_slave_frame_t *out);
hubus_status_t hubus_master_request_data(hubus_master_t *m, uint8_t mcid, hubus_slave_frame_t *out);

/* POLL_PENDING with a prefix mask; fills the arbitration result. */
hubus_status_t hubus_master_poll(hubus_master_t *m, uint8_t prefix, uint8_t target_len, hubus_arb_result_t *res);

/* ---- burst ---------------------------------------------------------- */

typedef struct {
    uint16_t polls;
    uint16_t served;      /* frames delivered to the application */
    uint16_t duplicates;  /* frames discarded as duplicates (equal GEN and payload) */
    uint16_t gaps;        /* frames whose GEN skipped buffer contents */
    uint16_t retries;
    uint16_t skipped;     /* slaves given up on after the retry budget */
} hubus_burst_stats_t;

/*
 * Called for every SLAVE_DATA frame that is not a duplicate. `lost` is the
 * number of overwritten buffer contents for `HUBUS_GEN_GAP`, otherwise 0.
 * `f->more` tells the application that a further fragment follows.
 */
typedef void (*hubus_data_fn)(void *ctx, const hubus_slave_frame_t *f, hubus_gen_result_t gen, uint8_t lost);

/* Collects data from every slave pending at the start, in descending MCID order. */
hubus_status_t hubus_master_burst(hubus_master_t *m, hubus_data_fn on_data, void *ctx, hubus_burst_stats_t *stats);

/* check_event + burst: one iteration of a service loop. */
hubus_status_t hubus_master_service(hubus_master_t *m, hubus_data_fn on_data, void *ctx, hubus_burst_stats_t *stats);

/* ---- commissioning -------------------------------------------------- */

#define HUBUS_SCAN_ABSENT 0u
#define HUBUS_SCAN_PRESENT 1u
#define HUBUS_SCAN_SUSPECT 2u /* repeated CRC failures: duplicate MCID or line problem */

typedef struct {
    uint8_t state[HUBUS_MCID_MAX + 1u];
    hubus_slave_frame_t info[HUBUS_MCID_MAX + 1u];
} hubus_scan_t;

/* REQUEST_INFO to every MCID 1..63. */
hubus_status_t hubus_master_scan(hubus_master_t *m, hubus_scan_t *scan);

#define HUBUS_DIAG_OK 0u
#define HUBUS_DIAG_DUPLICATE 1u    /* a few MCIDs fail repeatedly: suspected duplicate MCID */
#define HUBUS_DIAG_LINE_PROBLEM 2u /* many MCIDs fail: line problem */

/* Runtime heuristic over the failure counters; `suspects` gets one bit per MCID. */
uint8_t hubus_master_diagnose(const hubus_master_t *m, uint64_t *suspects);

#endif /* HUBUS_MASTER_H */
