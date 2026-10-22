/*
 * hubusd core: operates the bus through the master library and talks MQTT
 * through a small interface (docs/DAEMON_REQUIREMENTS.md).
 *
 * Threading: `hubusd_step()` and everything else except the two functions
 * below run in the bus thread, the only thread that touches the bus.
 * `hubusd_handle_message()` and `hubusd_on_connect()` may be called from the
 * MQTT thread; they only validate and enqueue.
 */
#ifndef HUBUSD_CORE_H
#define HUBUSD_CORE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "hubus_master.h"
#include "hubusd_config.h"

#define HUBUSD_VERSION "0.1.0"
#define HUBUSD_QUEUE_LEN 32
#define HUBUSD_TEXT_LEN 96

typedef struct {
    void *ctx;
    /* Publish a message; returns 0 on success. Called from the bus thread only. */
    int (*publish)(void *ctx, const char *topic, const char *payload, size_t len, int retain);
} hubusd_mqtt_t;

typedef struct {
    void *ctx;
    uint64_t (*mono_ms)(void *ctx); /* monotonic milliseconds */
    uint64_t (*wall_s)(void *ctx);  /* seconds since the epoch (for timestamps in messages) */
} hubusd_clock_t;

typedef void (*hubusd_log_fn)(void *ctx, int level, const char *msg); /* level: 0 info, 1 warning, 2 error */

typedef enum {
    HUBUSD_BUS_STARTING,
    HUBUSD_BUS_OK,
    HUBUSD_BUS_LINE_FAULT
} hubusd_bus_state_t;

typedef enum {
    HUBUSD_REQ_NONE,
    HUBUSD_REQ_SET,      /* text from a `set` topic, interpreted with the slave profile */
    HUBUSD_REQ_RAW,      /* opcode and arguments */
    HUBUSD_REQ_CAPTURE,  /* STATUS_CAPTURE */
    HUBUSD_REQ_READ,     /* REQUEST_STATE: read the live state of one slave */
    HUBUSD_REQ_PROBE,    /* REQUEST_INFO to one MCID (prefix), configured or not */
    HUBUSD_REQ_SCAN,
    HUBUSD_REQ_STATS,
    HUBUSD_REQ_REJECT    /* message that was invalid in the MQTT thread; reported by the bus thread */
} hubusd_cmd_kind_t;

typedef struct {
    hubusd_cmd_kind_t kind;
    uint8_t target_len; /* 0..6 */
    uint8_t prefix;     /* MCID or group prefix */
    uint8_t opcode;
    uint8_t n_args;
    uint8_t args[HUBUS_MAX_PAYLOAD];
    char text[HUBUSD_TEXT_LEN];
} hubusd_request_t;

typedef struct {
    uint32_t bursts; /* group requests ("polls") are counted by the master library */
    uint32_t frames;
    uint32_t duplicates;
    uint32_t gaps;
    uint32_t retries;
    uint32_t skipped;
    uint32_t commands_executed;
    uint32_t commands_rejected;
    uint32_t queue_full;
    uint32_t publish_errors;
    uint32_t startups;
    uint32_t scans;
} hubusd_stats_t;

typedef struct {
    uint8_t configured;
    uint8_t cfg_index;
    const hubusd_profile_t *profile; /* resolved profile or NULL (raw topics only) */
    int online;                      /* -1 unknown, 0 offline, 1 online */
    unsigned ping_failures;
    uint8_t have_info;
    hubus_slave_frame_t info;
    uint64_t last_seen_ms;
    uint64_t next_ping_ms;
    uint64_t next_poll_ms;           /* next periodic state read (slave option poll_interval_s) */
    char last_state[32];
    uint8_t unknown_seen;            /* present on the bus but not configured */
    uint8_t unknown_logged;
    uint8_t needs_refresh;           /* came online: read its state (REQUEST_STATE, or STATUS_CAPTURE without a state) */
} hubusd_slave_t;

typedef struct hubusd {
    hubusd_config_t cfg;
    hubus_master_t master;
    hubusd_mqtt_t mqtt;
    hubusd_clock_t clock;
    hubusd_log_fn log_fn;
    void *log_ctx;

    hubusd_bus_state_t bus;
    char last_error[96];
    uint64_t start_ms;
    uint64_t next_startup_ms;
    uint64_t next_scan_ms;
    uint64_t next_stats_ms;
    char last_state_json[768];
    char last_diag_json[256];
    int started;          /* initial startup, scan, and probe done */

    hubusd_slave_t slave[HUBUS_MCID_MAX + 1u];
    hubusd_stats_t stats;

    pthread_mutex_t lock; /* protects the queue and the flags below */
    hubusd_request_t queue[HUBUSD_QUEUE_LEN];
    unsigned q_head, q_count;
    int republish;        /* set by hubusd_on_connect() */
    int reload_pending;
    hubusd_config_t *reload_cfg;
} hubusd_t;

/* The configuration is copied. Nothing is sent before hubusd_step(). */
void hubusd_init(hubusd_t *d, const hubusd_config_t *cfg, const hubus_master_hal_t *hal, hubusd_mqtt_t mqtt,
                 hubusd_clock_t clock);
void hubusd_destroy(hubusd_t *d);
void hubusd_set_logger(hubusd_t *d, hubusd_log_fn fn, void *ctx);

/* One iteration of the bus thread; call it in a loop. Returns 1 if the bus was busy (call again soon). */
int hubusd_step(hubusd_t *d);

/* Publishes offline states and generates a RESET. */
void hubusd_shutdown(hubusd_t *d);

/* MQTT thread: an incoming message. `topic` is nul-terminated, `payload` is not. */
void hubusd_handle_message(hubusd_t *d, const char *topic, const void *payload, size_t len);

/* MQTT thread: connection (re-)established; retained state is republished by the bus thread. */
void hubusd_on_connect(hubusd_t *d);

/* Any thread: replace the slave list and profiles with a new configuration (SIGHUP). Takes ownership. */
void hubusd_request_reload(hubusd_t *d, hubusd_config_t *new_cfg);

/* Topics the MQTT client has to subscribe to; calls `fn` for every topic filter. */
void hubusd_subscriptions(const hubusd_t *d, void (*fn)(void *ctx, const char *filter), void *ctx);

/* Topic of the master availability (last will). Returns the length. */
size_t hubusd_status_topic(const hubusd_t *d, char *out, size_t out_size);

#endif /* HUBUSD_CORE_H */
