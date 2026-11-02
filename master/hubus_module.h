/*
 * Master modules (docs/spec/06-operation.md section 6.10): the typed interface between the master and the module that
 * is delivered with a slave firmware image, and the selection of a module by the hardware identifier
 * (docs/spec/04-frames.md section 4.7).
 *
 * A module is a constant hubus_module_t in C. Everything the bus thread needs is data, so that no module code runs in
 * it. A plugin exports hubus_module_entry(); a module can also be linked statically, as in the tests.
 */
#ifndef HUBUS_MODULE_H
#define HUBUS_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hubus_proto.h"

#define HUBUS_MODULE_API_VERSION 1u

#define HUBUS_RULE_MAX_LEN HUBUS_HWID_FULL_MIN_LEN /* rules never cover the serial number */
#define HUBUS_MODULE_CMD_MAX_ARGS 8u
#define HUBUS_MODULE_ENTITIES_MAX 8u /* per module */
#define HUBUS_ENTITY_KEY_MAX 23u     /* characters; letters, digits, '_' and '-': it is part of an MQTT topic */

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

/* The kinds of entity of Home Assistant that a module can offer. */
typedef enum {
    HUBUS_COMPONENT_SENSOR = 0,
    HUBUS_COMPONENT_BINARY_SENSOR,
    HUBUS_COMPONENT_SWITCH,
    HUBUS_COMPONENT_LIGHT,
    HUBUS_COMPONENT_COUNT
} hubus_component_t;

typedef enum {
    HUBUS_DEVICE_CLASS_NONE = 0,
    HUBUS_DEVICE_CLASS_TEMPERATURE,
    HUBUS_DEVICE_CLASS_HUMIDITY,
    HUBUS_DEVICE_CLASS_ILLUMINANCE,
    HUBUS_DEVICE_CLASS_PRESSURE,
    HUBUS_DEVICE_CLASS_POWER,
    HUBUS_DEVICE_CLASS_VOLTAGE,
    HUBUS_DEVICE_CLASS_CURRENT,
    HUBUS_DEVICE_CLASS_MOTION,
    HUBUS_DEVICE_CLASS_DOOR,
    HUBUS_DEVICE_CLASS_COUNT
} hubus_device_class_t;

typedef enum {
    HUBUS_UNIT_NONE = 0,
    HUBUS_UNIT_CELSIUS,
    HUBUS_UNIT_PERCENT,
    HUBUS_UNIT_LUX,
    HUBUS_UNIT_HECTOPASCAL,
    HUBUS_UNIT_WATT,
    HUBUS_UNIT_VOLT,
    HUBUS_UNIT_AMPERE,
    HUBUS_UNIT_COUNT
} hubus_unit_t;

/* Where a value lies in a frame of the slave: value = raw * scale + add. len 0: the entity has no such value. */
typedef struct {
    uint8_t offset;
    uint8_t len; /* 0, 1, or 2 */
    bool is_signed;
    bool big_endian;
    double scale;
    double add;
    uint8_t decimals;
} hubus_module_decode_t;

/* A command with fixed bytes. */
typedef struct {
    bool used;
    uint8_t opcode;
    uint8_t n_args;
    uint8_t args[HUBUS_MODULE_CMD_MAX_ARGS];
} hubus_module_command_t;

typedef struct {
    const char *key;  /* unique within the module; part of the MQTT topic */
    const char *name; /* shown to the user */
    hubus_component_t component;
    hubus_device_class_t device_class;
    hubus_unit_t unit;
    hubus_module_decode_t decode;
    hubus_module_command_t on;  /* switch: without on_set */
    hubus_module_command_t off;
} hubus_entity_t;

typedef enum {
    HUBUS_VALUE_BOOL = 0,
    HUBUS_VALUE_NUMBER,
    HUBUS_VALUE_RGB
} hubus_value_type_t;

/* A value for an entity, from the user. */
typedef struct {
    hubus_value_type_t type;
    union {
        bool on;
        double number;
        struct {
            uint8_t r, g, b;
        } rgb;
    } as;
} hubus_value_t;

typedef enum {
    HUBUS_FRAME_EVENT = 0,
    HUBUS_FRAME_STATE
} hubus_frame_kind_t;

typedef enum {
    HUBUS_MODULE_OK = 0,
    HUBUS_MODULE_REJECTED, /* the value does not fit, or the module cannot do this */
    HUBUS_MODULE_FAILED    /* the command could not be queued */
} hubus_module_status_t;

/* What the master offers a module instance. */
typedef struct hubus_module_host hubus_module_host_t;
struct hubus_module_host {
    void *ctx; /* of the master; modules do not touch it */
    uint8_t mcid;
    /* Queues a command to this slave; the bus thread sends it. args may be NULL with len 0. */
    hubus_module_status_t (*send)(const hubus_module_host_t *host, uint8_t opcode, const uint8_t *args, uint8_t len);
};

/* The state of a module for one slave. Each module defines this structure for itself; the master never looks inside. */
typedef struct hubus_module_instance hubus_module_instance_t;

typedef struct {
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

    const hubus_entity_t *entities;
    uint8_t n_entities;

    /* Optional functions, never called in the bus thread. create and destroy come together. */
    hubus_module_instance_t *(*create)(const hubus_module_host_t *host);
    void (*destroy)(hubus_module_instance_t *self);
    void (*on_frame)(hubus_module_instance_t *self, const hubus_module_host_t *host, hubus_frame_kind_t kind,
                     const uint8_t *data, uint8_t len);
    hubus_module_status_t (*on_set)(hubus_module_instance_t *self, const hubus_module_host_t *host,
                                    const hubus_entity_t *entity, const hubus_value_t *value);
} hubus_module_t;

typedef enum {
    HUBUS_MODULE_VALID = 0,
    HUBUS_MODULE_ERR_API,       /* other API version, or a smaller structure than this master knows */
    HUBUS_MODULE_ERR_NAME,
    HUBUS_MODULE_ERR_RULE,
    HUBUS_MODULE_ERR_PROPERTIES, /* command size, or STATUS_CAPTURE without an event buffer */
    HUBUS_MODULE_ERR_BLIND,
    HUBUS_MODULE_ERR_ENTITY,
    HUBUS_MODULE_ERR_FUNCTIONS
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

/* Index of the entity with this key, -1 if the module has none. */
int hubus_module_find_entity(const hubus_module_t *module, const char *key);

/* The value of an entity in a frame of the slave, as text: a sensor gives a number with the decimals of the entity, the
 * other components "ON" or "OFF" (value not zero). Returns the length of the text, 0 if the frame is too short, the entity
 * has no value in a frame, or `out` is too small. */
size_t hubus_entity_decode(const hubus_entity_t *entity, const uint8_t *data, uint8_t len, char *out, size_t out_size);

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

#endif /* HUBUS_MODULE_H */
