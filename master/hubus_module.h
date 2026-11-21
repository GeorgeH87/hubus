/*
 * Master modules (docs/spec/06-operation.md section 6.10): the typed interface between the master and the module that
 * is delivered with a slave firmware image, and the selection of a module by the hardware identifier
 * (docs/spec/04-frames.md section 4.7).
 *
 * A module is a constant hubus_module_t in C. What the bus thread needs is data, so that no module code runs in it.
 * Everything else is up to the functions of the module: every slave that goes into operation gets an instance, and the
 * instance decides which topics below the slave it takes and what it publishes there. The master only passes messages,
 * frames and commands between the instance, the broker and the bus. hubus_module_ha.h offers ready-made functions for
 * modules that describe their slave by a table of entities. A plugin exports hubus_module_entry(); a module can also be
 * linked statically, as in the tests.
 */
#ifndef HUBUS_MODULE_H
#define HUBUS_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hubus_proto.h"

#define HUBUS_MODULE_API_VERSION 3u

#define HUBUS_RULE_MAX_LEN HUBUS_HWID_FULL_MIN_LEN /* rules never cover the serial number */
#define HUBUS_MODULE_TOPIC_MAX 63u    /* characters of a topic below the slave */
#define HUBUS_MODULE_TOPICS_MAX 16u   /* topics one instance can take at a time */
#define HUBUS_MODULE_PAYLOAD_MAX 1023u /* bytes of a message an instance publishes, a discovery message included */
#define HUBUS_MODULE_MESSAGE_MAX 255u /* bytes of a message to an instance */
#define HUBUS_MODULE_LOG_MAX 95u      /* characters of a line for the log */
#define HUBUS_MODULE_SETTING_KEY_MAX 23u /* characters of the key of a setting */
#define HUBUS_MODULE_SETTING_MAX 95u     /* characters of the value of a setting */
#define HUBUS_MODULE_SETTINGS_MAX 8u     /* settings of one slave */

/* Number of elements of an array, for the counts in the structures below. */
#define HUBUS_MODULE_LEN(array) ((uint8_t)(sizeof(array) / sizeof((array)[0])))

/* First fields of every hubus_module_t. */
#define HUBUS_MODULE_INIT .api_version = HUBUS_MODULE_API_VERSION, .struct_size = sizeof(hubus_module_t)

typedef enum {
    HUBUS_HWID_INVALID = 0,
    HUBUS_HWID_SHORT,  /* 0x01..0x7F: the type byte alone */
    HUBUS_HWID_FULL,   /* 0x80..0xFE: type, variant, version, hardware revision, optional serial number */
    HUBUS_HWID_UPDATER /* 0xFF: followed by the updater information */
} hubus_hwid_form_t;

/* A byte pattern and a mask of the same length. Unused bytes stay 0. */
typedef struct {
    uint8_t pattern[HUBUS_RULE_MAX_LEN];
    uint8_t mask[HUBUS_RULE_MAX_LEN];
    uint8_t len; /* 1..4 */
} hubus_module_rule_t;

/* Blind time after one opcode; overrides blind_default_ms. 0: this opcode does not make the slave blind. */
typedef struct {
    uint8_t opcode;
    uint16_t ms;
} hubus_module_blind_t;

typedef enum {
    HUBUS_FRAME_EVENT = 0,
    HUBUS_FRAME_STATE
} hubus_frame_kind_t;

typedef enum {
    HUBUS_MODULE_OK = 0,
    HUBUS_MODULE_REJECTED, /* the value does not fit, or the module cannot do this */
    HUBUS_MODULE_FAILED    /* the command could not be queued */
} hubus_module_status_t;

/* What the master does after a command to the slave, to learn its state (hubusd: readback). */
typedef enum {
    HUBUS_READBACK_NONE = 0, /* nothing: the state changes only through events and reads */
    HUBUS_READBACK_CACHE,    /* nothing either: the module publishes the state its command establishes */
    HUBUS_READBACK_SLAVE     /* the master reads the state of the slave, and on_frame gets it */
} hubus_module_readback_t;

typedef struct hubus_module hubus_module_t;

/*
 * What the master offers a module instance. It stays valid until destroy() has returned. Its functions only queue their
 * result; the master checks and executes it later on its bus thread, so they return at once and a refusal that depends on
 * the bus (a command too long for the slave, a slave that went away) shows only in the log. They return
 * HUBUS_MODULE_REJECTED for an argument that is not allowed and HUBUS_MODULE_FAILED if the queue of the module is full.
 *
 * A topic is always one below the slave, "<base>/slave/<mcid>/<topic>": levels of letters, digits, '_' and '-',
 * separated by '/', at most HUBUS_MODULE_TOPIC_MAX characters. Its first level is not one the master has for itself
 * (availability, capture, cmd, config, data, firmware, info, mcid, read, stats, update; hubus_module_topic_valid()).
 */
typedef struct hubus_module_host hubus_module_host_t;
struct hubus_module_host {
    void *ctx; /* of the master; modules do not touch it */
    const hubus_module_t *module;
    uint8_t mcid;
    hubus_module_readback_t readback; /* for this slave now; valid during the call */

    /* Queues a command to this slave. The opcode is 0 (STATUS_CAPTURE) or 0x10..0xFF, opcode and arguments are at most 31
     * bytes together; args may be NULL with len 0. */
    hubus_module_status_t (*send)(const hubus_module_host_t *host, uint8_t opcode, const uint8_t *args, uint8_t len);
    /* Takes the messages on a topic: on_message gets them from now on. The topic may contain '+' for one level and '#' as
     * its last level, as in MQTT, but not in its first level. At most HUBUS_MODULE_TOPICS_MAX topics at a time; taking a
     * topic twice changes nothing. */
    hubus_module_status_t (*subscribe)(const hubus_module_host_t *host, const char *topic);
    hubus_module_status_t (*unsubscribe)(const hubus_module_host_t *host, const char *topic);
    /* Publishes `len` bytes (at most HUBUS_MODULE_PAYLOAD_MAX) on a topic, without wildcards. A retained message is
     * published again when the master reconnects, and removed when the slave leaves the configuration or gets another
     * module. An empty retained message removes the topic. */
    hubus_module_status_t (*publish)(const hubus_module_host_t *host, const char *topic, const void *payload, size_t len,
                                     bool retain);
    /*
     * Publishes the discovery message of an entity of Home Assistant, retained, under
     * "<discovery prefix>/<component>/hubus_<mcid>_<object>/config", where `object` follows the rules of a topic level.
     * `json` is one JSON object without "unique_id", "availability", "availability_mode", and "device": the master adds
     * them, so that every entity of the slave belongs to its device and is available while master and slave are online.
     * Nothing is published while discovery is switched off. Like a retained message, it is published again and removed.
     */
    hubus_module_status_t (*discovery)(const hubus_module_host_t *host, const char *component, const char *object,
                                       const char *json);
    /* The full name of a topic of this slave, for a discovery message. Returns its length; 0 if `out` is too small. */
    size_t (*topic)(const hubus_module_host_t *host, const char *topic, char *out, size_t out_size);
    /* A line for the log of the master; cut off after HUBUS_MODULE_LOG_MAX characters. */
    hubus_module_status_t (*log)(const hubus_module_host_t *host, const char *text);
    /* Says why the module did not take a message: logged like log(), and counted as a rejected command. */
    hubus_module_status_t (*reject)(const hubus_module_host_t *host, const char *text);

    /*
     * Settings: short texts the master keeps for this slave and this module across its restarts (hubusd: in its runtime
     * file), such as a value the user chose that the slave forgets at a power-up. A key has letters, digits, '_' and '-', at
     * most HUBUS_MODULE_SETTING_KEY_MAX characters, and is not "module"; a value has at most HUBUS_MODULE_SETTING_MAX
     * printable characters; at most HUBUS_MODULE_SETTINGS_MAX keys per slave. The settings go when the slave leaves the
     * configuration; another module of the slave does not get them.
     *
     * setting copies the value of `key` to `out` as a string and returns its length; 0 if there is none or `out` is too small,
     * with "" in `out`. It answers at once with what the instance saved last, or what the master had when it created the
     * instance. save keeps `value` for `key`; NULL removes the key. Only a change is written.
     */
    size_t (*setting)(const hubus_module_host_t *host, const char *key, char *out, size_t out_size);
    hubus_module_status_t (*save)(const hubus_module_host_t *host, const char *key, const char *value);
};

/* The state of a module for one slave. Each module defines this structure for itself; the master never looks inside. */
typedef struct hubus_module_instance hubus_module_instance_t;

struct hubus_module {
    uint32_t api_version;
    uint32_t struct_size;
    const char *name;

    /* Data; the bus thread reads it. */
    const hubus_module_rule_t *rules;
    uint8_t n_rules;
    uint8_t max_command; /* largest accepted command payload including the opcode, 1..31 */
    bool has_state;
    bool has_events;
    bool has_status_capture; /* requires has_events */
    bool has_update;
    uint16_t blind_default_ms; /* 0 and no blind_ops: never blind */
    const hubus_module_blind_t *blind_ops;
    uint8_t n_blind_ops;

    /*
     * Optional: the application image this module comes with, from the start of the application area. A module with an
     * image needs has_update. firmware_version is the version byte of the HWID that the image reports once it runs;
     * firmware_signature the device signature it is built for, all zero: not checked.
     */
    const uint8_t *firmware;
    uint32_t firmware_len;
    uint8_t firmware_version;
    uint8_t firmware_signature[3];

    /* For the functions of the module, such as the table of entities of hubus_module_ha.h; the master never looks at it. */
    const void *data;

    /*
     * The functions; every module has all four. A master calls them on a thread of the module, never on its bus thread:
     * the calls of one module run one after the other, in the order of the events, and those of different modules run at
     * the same time. A function has to return within the time limit of the master (hubusd: modules.call_timeout_ms);
     * otherwise the module is stopped.
     *
     * create makes the instance for one slave, NULL if it cannot; it usually subscribes to its topics and publishes its
     * discovery messages. destroy ends it; its topics go with it. on_frame gets a frame of the slave (`gap`: frames were
     * lost before it). on_message gets a message on a topic the instance subscribed to: the topic below the slave, and
     * `len` bytes of payload, followed by a NUL that is not counted.
     */
    hubus_module_instance_t *(*create)(const hubus_module_host_t *host);
    void (*destroy)(hubus_module_instance_t *self);
    void (*on_frame)(hubus_module_instance_t *self, const hubus_module_host_t *host, hubus_frame_kind_t kind,
                     const uint8_t *data, uint8_t len, bool gap);
    void (*on_message)(hubus_module_instance_t *self, const hubus_module_host_t *host, const char *topic,
                       const char *payload, size_t len);
};

typedef enum {
    HUBUS_MODULE_VALID = 0,
    HUBUS_MODULE_ERR_API,       /* other API version, or a smaller structure than this master knows */
    HUBUS_MODULE_ERR_NAME,
    HUBUS_MODULE_ERR_RULE,
    HUBUS_MODULE_ERR_PROPERTIES, /* command size, or STATUS_CAPTURE without an event buffer */
    HUBUS_MODULE_ERR_BLIND,
    HUBUS_MODULE_ERR_FUNCTIONS,  /* one of the four functions is missing */
    HUBUS_MODULE_ERR_FIRMWARE    /* an image without a length or the other way round, or without has_update */
} hubus_module_error_t;

typedef enum {
    HUBUS_MODULE_FOUND = 0,
    HUBUS_MODULE_UNKNOWN,  /* valid HWID, no rule matches */
    HUBUS_MODULE_INVALID,  /* invalid HWID */
    HUBUS_MODULE_CONFLICT  /* rules of different modules match with the same number of mask bits */
} hubus_module_match_t;

/* Exported by a module built as a plugin. */
const hubus_module_t *hubus_module_entry(void);

/* The form of a HWID, including its length. */
hubus_hwid_form_t hubus_hwid_form(const uint8_t *hwid, uint8_t len);

/* Checks a module before its first use. A module that fails here must not be selected. */
hubus_module_error_t hubus_module_validate(const hubus_module_t *module);

/* Selects the module for a HWID from validated modules: the rule with the most mask bits wins. *out is NULL unless the
 * result is HUBUS_MODULE_FOUND. */
hubus_module_match_t hubus_module_select(const hubus_module_t *const *modules, size_t n_modules, const uint8_t *hwid,
                                         uint8_t len, const hubus_module_t **out);

/* True if a rule of `a` and a rule of `b` have the same number of mask bits and can match the same HWID: select would
 * find no winner for it. Modules whose rules differ in the number of mask bits do not conflict, the more specific wins. */
bool hubus_module_conflicts(const hubus_module_t *a, const hubus_module_t *b);

/* The same from a SLAVE_INFO payload. A protocol version this master does not know counts as an invalid HWID: it may lay
 * out everything after byte 0 differently. */
hubus_module_match_t hubus_module_select_info(const hubus_module_t *const *modules, size_t n_modules,
                                              const uint8_t *payload, uint8_t len, const hubus_module_t **out);

/* 1 if the slave can be blind after any command: it then keeps quiet until the master ends that phase. */
bool hubus_module_has_blind_time(const hubus_module_t *module);

/* The blind time after a command with this opcode, in milliseconds; 0: the slave is not blind. */
uint16_t hubus_module_blind_ms(const hubus_module_t *module, uint8_t opcode);

/* True for a topic below the slave that a module may use (see hubus_module_host_t); `filter`: with wildcards, for subscribe. */
bool hubus_module_topic_valid(const char *topic, bool filter);

/* True if the first level of `topic` is one the master keeps for itself. */
bool hubus_module_topic_reserved(const char *topic);

/* True if `topic` matches the topic filter `filter`, both valid (MQTT rules for '+' and '#'). */
bool hubus_module_topic_matches(const char *filter, const char *topic);

/* True for the key of a setting and, unless it is NULL, its value (see hubus_module_host_t, setting and save). */
bool hubus_module_setting_valid(const char *key, const char *value);

#endif /* HUBUS_MODULE_H */
