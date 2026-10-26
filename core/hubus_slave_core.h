/*
 * Slave protocol state machine as pure logic (docs/spec/07-slave.md).
 *
 * The core has no hardware access and no time base. The caller feeds it the
 * pin levels after every pin change (`hubus_core_pins`) and applies the
 * resulting DATA drive (`hubus_core_data_low`) to the pin. The same code runs
 * in the ATtiny ISR, in host-side unit tests, and in the bus simulation.
 *
 * Application callbacks run in the context that feeds the pin events (the
 * ISR on a real device) and must be short.
 */
#ifndef HUBUS_SLAVE_CORE_H
#define HUBUS_SLAVE_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "hubus_proto.h"

/* Compile-time buffer sizes (bytes, 1..31). The ATtiny85 reference build
 * uses the full 31 bytes. HUBUS_RX_BUF_SIZE is the largest accepted
 * SEND_COMMAND payload including the opcode and is announced in SLAVE_INFO. */
#ifndef HUBUS_TX_BUF_SIZE
#define HUBUS_TX_BUF_SIZE 31
#endif
#ifndef HUBUS_RX_BUF_SIZE
#define HUBUS_RX_BUF_SIZE 31
#endif

#if HUBUS_TX_BUF_SIZE < 1 || HUBUS_TX_BUF_SIZE > 31
#error "HUBUS_TX_BUF_SIZE must be 1..31"
#endif
#if HUBUS_RX_BUF_SIZE < 1 || HUBUS_RX_BUF_SIZE > 31
#error "HUBUS_RX_BUF_SIZE must be 1..31"
#endif

/*
 * Optional parts (docs/spec/07-slave.md section 7.4), chosen per device type; both are on by default and every
 * combination is valid:
 *
 *   HUBUS_ENABLE_EVENTS  event buffer: hubus_core_write(), idle event signaling, REQUEST_DATA, STATUS_CAPTURE.
 *                        Without it the slave never speaks on its own (for example a sensor that is only read).
 *   HUBUS_ENABLE_STATE   live state for REQUEST_STATE (docs/spec/04-frames.md section 4.10).
 *
 * Without both the slave only executes commands and answers REQUEST_INFO; the master keeps its state in a cache.
 */
#ifndef HUBUS_ENABLE_EVENTS
#define HUBUS_ENABLE_EVENTS 1
#endif
#ifndef HUBUS_ENABLE_STATE
#define HUBUS_ENABLE_STATE 1
#endif

/*
 * Firmware update (docs/spec/06-operation.md section 6.8), off by default:
 *
 *   HUBUS_ENABLE_UPDATE  application side: announces capability bit 3 and takes UPDATE_ENTER; the caller then hands
 *                        over to the updater (hubus_core_update_requested()). Only for devices with an updater.
 *   HUBUS_UPDATER        the updater itself: announces capability bit 4 and passes the update opcodes
 *                        (UPDATE_LOAD and higher) to on_command.
 */
#ifndef HUBUS_ENABLE_UPDATE
#define HUBUS_ENABLE_UPDATE 0
#endif
#ifndef HUBUS_UPDATER
#define HUBUS_UPDATER 0
#endif
#if HUBUS_ENABLE_UPDATE && HUBUS_UPDATER
#error "HUBUS_ENABLE_UPDATE is for the application, HUBUS_UPDATER for the updater: not both"
#endif

/* Application bytes of SLAVE_INFO follow 3 protocol bytes. */
#define HUBUS_INFO_APP_MAX (HUBUS_MAX_PAYLOAD - HUBUS_INFO_APP)

/* Read a byte of the static SLAVE_INFO data (flash on AVR). */
#ifdef __AVR__
#include <avr/pgmspace.h>
#define HUBUS_INFO_READ(p) pgm_read_byte(p)
#else
#define HUBUS_INFO_READ(p) (*(p))
#endif

typedef void (*hubus_status_capture_fn)(void *ctx);
typedef void (*hubus_command_fn)(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t args_len);

typedef struct {
    uint8_t mcid;                              /* 1..63 */
    const uint8_t *info;                       /* application bytes of SLAVE_INFO (flash on AVR), may be NULL */
    uint8_t info_len;                          /* 0..HUBUS_INFO_APP_MAX */
    const volatile uint8_t *state;             /* live state for REQUEST_STATE, read while transmitting; may be NULL;
                                                  ignored without HUBUS_ENABLE_STATE */
    uint8_t state_len;                         /* 0 (no state) .. HUBUS_MAX_PAYLOAD */
    hubus_status_capture_fn on_status_capture; /* NULL: STATUS_CAPTURE not supported; ignored without HUBUS_ENABLE_EVENTS */
    hubus_command_fn on_command;               /* application opcodes 0x10..0xFF, may be NULL */
    void *ctx;
    uint8_t speed_class;                       /* 0 (not declared) .. HUBUS_SPEED_CLASS_MAX, announced in SLAVE_INFO */
} hubus_core_config_t;

/* Result of hubus_core_write(). */
#define HUBUS_WRITE_OK 0u
#define HUBUS_WRITE_TOO_LONG 1u /* more bytes than HUBUS_TX_BUF_SIZE */
#define HUBUS_WRITE_BUSY 2u     /* SLAVE_DATA response in progress; retry later */
#define HUBUS_WRITE_DISABLED 3u /* core not initialised (invalid configuration) */

/* Slave states (exposed for tests and debugging). */
#define HUBUS_ST_IDLE 0u   /* wait for START; pending flag may trigger event signaling */
#define HUBUS_ST_EVENT 1u  /* DATA held low until the first falling CLK edge */
#define HUBUS_ST_RX 2u     /* receiving the master frame */
#define HUBUS_ST_TURN 3u   /* frame complete; turnaround slot */
#define HUBUS_ST_ARB 4u    /* arbitration with handoff slots */
#define HUBUS_ST_TX 5u     /* transmitting SLAVE_INFO / SLAVE_DATA */
#define HUBUS_ST_IGNORE 6u /* passive until the next START, STOP, or RESET */

typedef struct hubus_slave_core {
    /* configuration */
    uint8_t mcid; /* 0: core disabled (not initialised or invalid configuration) */
    const uint8_t *info;
    uint8_t info_len;
#if HUBUS_ENABLE_STATE
    const volatile uint8_t *app_state;
    uint8_t app_state_len;
#endif
#if HUBUS_ENABLE_EVENTS
    hubus_status_capture_fn on_status_capture;
#endif
    hubus_command_fn on_command;
    void *ctx;
    uint8_t speed_caps; /* speed class, already in its SLAVE_INFO capability bits */

    /* line */
    uint8_t clk;         /* last seen CLK level */
    uint8_t data;        /* last seen DATA level (1 = high) */
    uint8_t data_low;    /* output: 1 = pull DATA low, 0 = release */
#if HUBUS_ENABLE_EVENTS
    uint8_t armed;       /* STOP/RESET seen: a pending slave announces itself at the next falling CLK edge */
#endif

    uint8_t state;

    /* frame shifter, shared by receive and transmit (never active at the same time) */
    uint8_t hdr[2]; /* received header, then the header of the response */
    uint8_t shift;  /* current byte */
    uint8_t nbits;  /* receive: bits shifted in; transmit: bits of the loaded byte still to send */
    uint8_t idx;    /* receive: bytes received; transmit: next byte to load (header, payload, CRC) */
    uint8_t crc;

    /* request */
    uint8_t rx_type;
    uint8_t rx_len;
    uint8_t cmd[HUBUS_RX_BUF_SIZE];

    /* turnaround and arbitration */
    uint8_t slot; /* turnaround: phase 0..2; arbitration: position in the 12-slot pattern */
    uint8_t lost; /* MCID bit at which this slave lost, 0xFF while it still contends */

    /* transmit */
    uint8_t tx_len;

#if HUBUS_ENABLE_EVENTS
    /* the single outgoing buffer (event buffer) */
    uint8_t tx_complete; /* complete SLAVE_DATA sent in this transaction */
    uint8_t buf[HUBUS_TX_BUF_SIZE];
    uint8_t buf_len;
    uint8_t more;
    uint8_t gen;
    uint8_t pending;
#endif
#if HUBUS_ENABLE_UPDATE
    uint8_t update_req; /* valid UPDATE_ENTER received */
#endif
} hubus_slave_core_t;

/* `clk` and `data` are the current pin levels. Returns 1 on success. With an invalid MCID (0, > 63), too much info data,
 * a state longer than HUBUS_MAX_PAYLOAD, or a speed class above HUBUS_SPEED_CLASS_MAX the core stays disabled (ignores
 * all traffic, never drives DATA) and 0 is returned. */
uint8_t hubus_core_init(hubus_slave_core_t *c, const hubus_core_config_t *cfg, uint8_t clk, uint8_t data);

/* Feed the current pin levels (1 = high). Call after every pin change. */
void hubus_core_pins(hubus_slave_core_t *c, uint8_t clk, uint8_t data);

/* Desired DATA drive: 1 = pull low, 0 = release. */
static inline uint8_t hubus_core_data_low(const hubus_slave_core_t *c)
{
    return c->data_low;
}

/* 1 between transactions (after STOP or RESET, also while signaling an event); polled from the main loop. */
static inline uint8_t hubus_core_is_idle(const hubus_slave_core_t *c)
{
    return *(const volatile uint8_t *)&c->state <= HUBUS_ST_EVENT;
}

#if HUBUS_ENABLE_UPDATE
/* 1 after a valid UPDATE_ENTER: the caller hands over to the updater at once. */
static inline uint8_t hubus_core_update_requested(const hubus_slave_core_t *c)
{
    return c->update_req;
}
#endif

#if HUBUS_ENABLE_EVENTS
/*
 * Writes the outgoing buffer: sets the pending flag, increments GEN and, if
 * the bus is idle with CLK low, starts idle event signaling. `more` marks a
 * fragment that is followed by further fragments. The caller must exclude
 * concurrent pin events (disable interrupts) while calling this.
 */
uint8_t hubus_core_write(hubus_slave_core_t *c, const uint8_t *data, uint8_t len, uint8_t more);

/* Polled from the main loop while the ISR clears the flag: read it fresh on every call. */
static inline uint8_t hubus_core_is_pending(const hubus_slave_core_t *c)
{
    return *(const volatile uint8_t *)&c->pending;
}
#endif

#endif /* HUBUS_SLAVE_CORE_H */
