/*
 * Entities of Home Assistant for master modules: a helper library that a module links in and calls from its own functions.
 * The master knows nothing of it; it only passes the messages and frames.
 *
 * A module describes its slave by a table of entities (hubus_ha_t, in hubus_module_t.data): each with a key, a name, a kind
 * of Home Assistant entity, a unit and device class, where its value lies in a frame, for a number its range, and for a
 * switch the commands for ON and OFF. The library turns the table into topics below the slave and discovery messages:
 *
 *   <key>/state       retained: the value, from frames (decode) or from the module (hubus_ha_publish_state())
 *   <key>/set         a switch, a light, a text, or a number takes values here
 *   <key>/rgb/set     the colour of a light, "r,g,b"
 *
 * A module that needs nothing else uses HUBUS_HA_FUNCTIONS as its functions. One with more to do calls hubus_ha_announce(),
 * hubus_ha_on_frame(), and hubus_ha_on_message() from its own functions, and gets the values that need code in on_set.
 */
#ifndef HUBUS_MODULE_HA_H
#define HUBUS_MODULE_HA_H

#include "hubus_module.h"

#define HUBUS_MODULE_CMD_MAX_ARGS 8u
#define HUBUS_MODULE_ENTITIES_MAX 8u /* per module */
#define HUBUS_ENTITY_KEY_MAX 23u     /* characters; letters, digits, '_' and '-': it is part of a topic */
#define HUBUS_MODULE_TEXT_MAX 95u    /* characters of a text value of an entity, in either direction */

/* The kinds of entity of Home Assistant that a table can have. */
typedef enum {
    HUBUS_COMPONENT_SENSOR = 0,
    HUBUS_COMPONENT_BINARY_SENSOR,
    HUBUS_COMPONENT_SWITCH,
    HUBUS_COMPONENT_LIGHT,
    HUBUS_COMPONENT_TEXT,   /* free text from the user, for on_set */
    HUBUS_COMPONENT_NUMBER, /* a number from the user within min..max, for on_set */
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
    HUBUS_DEVICE_CLASS_DISTANCE,
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
    HUBUS_UNIT_CENTIMETER,
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
    const char *key;  /* unique within the module; part of the topics */
    const char *name; /* shown to the user */
    hubus_component_t component;
    hubus_device_class_t device_class;
    hubus_unit_t unit;
    hubus_module_decode_t decode;
    hubus_module_command_t on; /* switch: without on_set */
    hubus_module_command_t off;
    /* number: the range the user may set, in the unit of the entity; step 0 means 1. The module publishes the state. */
    double min;
    double max;
    double step;
} hubus_entity_t;

typedef enum {
    HUBUS_VALUE_BOOL = 0,
    HUBUS_VALUE_NUMBER,
    HUBUS_VALUE_RGB,
    HUBUS_VALUE_TEXT
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
        const char *text; /* at most HUBUS_MODULE_TEXT_MAX characters; valid during the call */
    } as;
} hubus_value_t;

/* The table of a module, in hubus_module_t.data. */
typedef struct {
    const hubus_entity_t *entities;
    uint8_t n_entities;
    /*
     * Optional: a value for an entity that has no fixed command, parsed from the message: a bool for a switch or the on/off
     * of a light, RGB for the colour of a light, text for a text entity, a number within min..max for a number entity.
     * `self` is the instance of the module. HUBUS_MODULE_REJECTED for a value it does not take. A switch without fixed
     * commands, a light, a text, and a number need it.
     */
    hubus_module_status_t (*on_set)(hubus_module_instance_t *self, const hubus_module_host_t *host,
                                    const hubus_entity_t *entity, const hubus_value_t *value);
} hubus_ha_t;

/* The functions of a module that is all table. */
#define HUBUS_HA_FUNCTIONS \
    .create = hubus_ha_create, .destroy = hubus_ha_destroy, .on_frame = hubus_ha_on_frame, .on_message = hubus_ha_on_message

/* The table of a module; one without entities if its data is NULL. */
const hubus_ha_t *hubus_ha_of(const hubus_module_t *module);

/* Checks the table of a module: keys, kinds, values, commands within the command size of the module, and on_set where an
 * entity needs it. hubus_ha_announce() refuses a table that fails here. */
bool hubus_ha_valid(const hubus_module_t *module);

/* Subscribes to the topics of the entities that take values and publishes their discovery messages. HUBUS_MODULE_REJECTED
 * if the table is not valid (logged). */
hubus_module_status_t hubus_ha_announce(const hubus_module_host_t *host);

/* The value of the entity `key` as its state: "<key>/state", retained. For values that no frame reports. */
hubus_module_status_t hubus_ha_publish_state(const hubus_module_host_t *host, const char *key, const char *text);

/* create: hubus_ha_announce(), and an instance without a state of its own. destroy: nothing to do. */
hubus_module_instance_t *hubus_ha_create(const hubus_module_host_t *host);
void hubus_ha_destroy(hubus_module_instance_t *self);

/* Publishes the state of every entity whose value the frame holds. */
void hubus_ha_on_frame(hubus_module_instance_t *self, const hubus_module_host_t *host, hubus_frame_kind_t kind,
                       const uint8_t *data, uint8_t len, bool gap);

/* Takes a value from "<key>/set" or "<key>/rgb/set": sends the fixed command of a switch (and with readback cache publishes
 * the state it establishes), or passes the parsed value to on_set. Anything that does not fit is rejected through the host. */
void hubus_ha_on_message(hubus_module_instance_t *self, const hubus_module_host_t *host, const char *topic,
                         const char *payload, size_t len);

/* Index of the entity with this key, -1 if the table has none. */
int hubus_ha_find_entity(const hubus_ha_t *ha, const char *key);

/* The value of an entity in a frame of the slave, as text: a sensor gives a number with the decimals of the entity, the
 * other components "ON" or "OFF" (value not zero). Returns the length of the text, 0 if the frame is too short, the entity
 * has no value in a frame, or `out` is too small. */
size_t hubus_entity_decode(const hubus_entity_t *entity, const uint8_t *data, uint8_t len, char *out, size_t out_size);

/* The names Home Assistant knows for the enums. NULL where an enum says "none". */
const char *hubus_ha_component_name(hubus_component_t component);
const char *hubus_ha_device_class_name(hubus_device_class_t device_class);
const char *hubus_ha_unit_text(hubus_unit_t unit);

#endif /* HUBUS_MODULE_HA_H */
