/*
 * hubusd configuration (YAML). See docs/daemon.md (Configuration), daemon/hubusd.example.yaml, and
 * daemon/hubusd.runtime.example.yaml for the format.
 *
 * Two files: the main file holds what is fixed while hubusd runs (GPIO, timing, the broker, the modules); the runtime file
 * holds what can change over MQTT (the slaves, the bus section, some MQTT options, the settings of the modules). hubusd
 * writes the runtime file itself after every change; the main file may hold the runtime part as the start values for as
 * long as there is no runtime file.
 */
#ifndef HUBUSD_CONFIG_H
#define HUBUSD_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "hubus_master.h"
#include "hubus_module.h"

#define HUBUSD_MAX_SLAVES 63
#define HUBUSD_MAX_WARNINGS 16
#define HUBUSD_NAME_LEN 48
#define HUBUSD_MODULE_NAME_LEN 48
#define HUBUSD_RUNTIME_FILE_DEFAULT "/var/lib/hubus/runtime.yaml"

/* What a module keeps for one slave across restarts (host->save(), host->setting()). */
typedef struct {
    char key[HUBUS_MODULE_SETTING_KEY_MAX + 1u];
    char value[HUBUS_MODULE_SETTING_MAX + 1u];
} hubusd_setting_t;

typedef struct {
    char module[HUBUSD_MODULE_NAME_LEN]; /* the module they belong to; another module of the slave does not get them */
    hubusd_setting_t item[HUBUS_MODULE_SETTINGS_MAX];
    uint8_t n;
} hubusd_settings_t;

/* State reported after a command (config key `readback`). */
#define HUBUSD_READBACK_NONE 0  /* nothing: the next event or capture shows the state */
#define HUBUSD_READBACK_SLAVE 1 /* ask the slave (default): REQUEST_STATE, or STATUS_CAPTURE without a state */
#define HUBUSD_READBACK_CACHE 2 /* report the state the command establishes, without contacting the slave */
#define HUBUSD_TOPIC_LEN 64

typedef struct {
    uint8_t mcid;
    char name[HUBUSD_NAME_LEN];
    int readback;                          /* HUBUSD_READBACK_*, -1: use bus.readback */
    unsigned poll_interval_s;              /* read the live state (REQUEST_STATE) this often, 0: off; for slaves
                                              that only answer and never announce changes (passive sensors) */
    hubusd_settings_t settings;            /* of its module; runtime file only */
} hubusd_slave_cfg_t;

/*
 * The runtime part: everything that can change while hubusd runs, over MQTT or in the runtime file. It is a type of its own,
 * so that the functions for changes over MQTT and for the runtime file get only this part: GPIO, timing, the broker, and the
 * modules are not in it, and nothing on that path can reach them.
 */
typedef struct {
    struct {
        int readback;               /* HUBUSD_READBACK_*: what is reported after a state changing command */
        unsigned ping_interval_s;   /* REQUEST_INFO per configured slave to track availability, 0: off */
        unsigned ping_failures;     /* consecutive misses before a slave is offline */
        unsigned scan_interval_s;   /* commissioning scan for unconfigured slaves, 0: only at start */
        unsigned startup_retry_s;   /* retry interval after a line fault */
    } bus;

    struct {
        int discovery;         /* publish Home Assistant discovery messages */
        int discovery_cleanup; /* on every connection, remove discovery entries of unconfigured MCIDs and of early versions */
        unsigned stats_interval_s;
    } mqtt;

    hubusd_slave_cfg_t slaves[HUBUSD_MAX_SLAVES];
    unsigned n_slaves;
} hubusd_runtime_cfg_t;

/* The whole configuration: the fixed part, only from the main file, and the runtime part. */
typedef struct {
    char runtime_file[200]; /* empty: changes over MQTT are not kept */

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
    } mqtt;

    struct {
        char dir[200]; /* plugins (*.so) are loaded from here at the start; empty: only the built-in modules */
        unsigned call_timeout_ms; /* a module function that runs longer stops the module */
    } modules;

    hubusd_runtime_cfg_t rt;

    char warnings[HUBUSD_MAX_WARNINGS][256];
    unsigned n_warnings;
} hubusd_config_t;

void hubusd_config_defaults(hubusd_config_t *cfg);

/*
 * All return 0 on success and -1 on error with "line N: message" (or "file: message") in `err`; on an error the configuration
 * may be changed in part, so callers work on a copy.
 *
 * parse: the main file, from the defaults. load: the main file `path`, then the runtime file it names, if that exists: what
 * the runtime file has replaces the same part of the main file (the list of slaves as a whole). load_runtime: the text of a
 * runtime file, applied to the runtime part of `cfg`; unknown keys are warnings in `cfg`.
 */
int hubusd_config_parse(const char *text, size_t len, hubusd_config_t *cfg, char *err, size_t errsz);
int hubusd_config_load(const char *path, hubusd_config_t *cfg, char *err, size_t errsz);
int hubusd_config_load_runtime(const char *text, size_t len, hubusd_config_t *cfg, char *err, size_t errsz);

/*
 * A change over MQTT: applies a mapping with the runtime part (bus, mqtt.stats_interval_s, mqtt.discovery,
 * mqtt.discovery_cleanup, slaves) to `rt`; JSON is YAML too. Keys it does not name stay as they are; a list of slaves replaces
 * the list, and the settings of a slave that stays are kept (they belong to its module). Strict: an unknown key is an error,
 * and so is a section of the main file.
 */
int hubusd_config_merge(const char *text, size_t len, hubusd_runtime_cfg_t *rt, char *err, size_t errsz);

/* Applies a mapping of the options of one slave (name, readback, poll_interval_s) to slave `mcid`, which is added if it is not
 * configured. Strict like hubusd_config_merge(). */
int hubusd_config_merge_slave(const char *text, size_t len, hubusd_runtime_cfg_t *rt, unsigned mcid, char *err, size_t errsz);

/* Removes slave `mcid`; -1 if it is not configured. */
int hubusd_config_remove_slave(hubusd_runtime_cfg_t *rt, unsigned mcid);

/* The slave `mcid`, NULL if it is not configured. */
hubusd_slave_cfg_t *hubusd_config_slave(hubusd_runtime_cfg_t *rt, unsigned mcid);

/* The runtime part as YAML, in a buffer of the caller. Returns the length, 0 if it does not fit. */
size_t hubusd_config_format_runtime(const hubusd_runtime_cfg_t *rt, char *out, size_t size);

/* Writes the runtime part to `path`: to a new file next to it that then replaces the old one, so that a crash leaves either
 * the old or the new file. */
int hubusd_config_write_runtime(const hubusd_runtime_cfg_t *rt, const char *path, char *err, size_t errsz);

#endif /* HUBUSD_CONFIG_H */
