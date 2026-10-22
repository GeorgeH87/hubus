/*
 * hubusd configuration (YAML). See docs/DAEMON_REQUIREMENTS.md section 8 and
 * daemon/hubusd.example.yaml for the format.
 */
#ifndef HUBUSD_CONFIG_H
#define HUBUSD_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "hubus_master.h"
#include "hubusd_profile.h"

#define HUBUSD_MAX_SLAVES 63
#define HUBUSD_MAX_PROFILES 16
#define HUBUSD_MAX_WARNINGS 16
#define HUBUSD_NAME_LEN 48

/* State reported after a command (config key `readback`). */
#define HUBUSD_READBACK_NONE 0  /* nothing: the next event or capture shows the state */
#define HUBUSD_READBACK_SLAVE 1 /* ask the slave (default): REQUEST_STATE, or STATUS_CAPTURE without a state */
#define HUBUSD_READBACK_CACHE 2 /* report the state the command establishes, without contacting the slave */
#define HUBUSD_TOPIC_LEN 64

typedef struct {
    uint8_t mcid;
    char name[HUBUSD_NAME_LEN];
    char profile[HUBUSD_PROFILE_NAME_LEN]; /* empty: recognised from SLAVE_INFO, otherwise raw topics only */
    int readback;                          /* HUBUSD_READBACK_*, -1: use bus.readback */
    unsigned poll_interval_s;              /* read the live state (REQUEST_STATE) this often, 0: off; for slaves
                                              that only answer and never announce changes (passive sensors) */
} hubusd_slave_cfg_t;

typedef struct {
    struct {
        char chip[64]; /* GPIO chip label; empty: auto-detect */
        char path[64]; /* alternative: device path */
        unsigned clk_line;
        unsigned data_line;
        int realtime;  /* SCHED_FIFO priority, 0: off */
        int internal_pullup;
    } gpio;

    hubus_master_cfg_t master; /* timing minimums, retry budget, ... */

    struct {
        char host[128];
        int port;
        char username[64];
        char password[128];
        int tls;
        char cafile[200];
        char base_topic[HUBUSD_TOPIC_LEN];
        char discovery_prefix[HUBUSD_TOPIC_LEN];
        char client_id[64];
        int discovery;         /* publish Home Assistant discovery messages */
        int discovery_cleanup; /* at start, remove discovery entries of unconfigured MCIDs */
        unsigned stats_interval_s;
    } mqtt;

    struct {
        int readback;               /* HUBUSD_READBACK_*: what is reported after a state changing command */
        unsigned ping_interval_s;   /* REQUEST_INFO per configured slave to track availability, 0: off */
        unsigned ping_failures;     /* consecutive misses before a slave is offline */
        unsigned scan_interval_s;   /* commissioning scan for unconfigured slaves, 0: only at start */
        unsigned startup_retry_s;   /* retry interval after a line fault */
    } bus;

    hubusd_slave_cfg_t slaves[HUBUSD_MAX_SLAVES];
    unsigned n_slaves;

    hubusd_profile_t profiles[HUBUSD_MAX_PROFILES]; /* custom profiles */
    unsigned n_profiles;

    char warnings[HUBUSD_MAX_WARNINGS][160];
    unsigned n_warnings;
} hubusd_config_t;

void hubusd_config_defaults(hubusd_config_t *cfg);

/* Both return 0 on success and -1 on error with "line N: message" (or "file: message") in `err`. */
int hubusd_config_parse(const char *text, size_t len, hubusd_config_t *cfg, char *err, size_t errsz);
int hubusd_config_load(const char *path, hubusd_config_t *cfg, char *err, size_t errsz);

/* Profile of a configured slave: custom profile first, then built-in. NULL if the slave has none. */
const hubusd_profile_t *hubusd_config_profile(const hubusd_config_t *cfg, const char *name);

#endif /* HUBUSD_CONFIG_H */
