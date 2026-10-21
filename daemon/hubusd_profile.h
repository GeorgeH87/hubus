/*
 * Device profiles: how the daemon interprets the buffer bytes of a slave and
 * how it turns `set` commands into protocol commands (docs/DAEMON_REQUIREMENTS.md
 * section 7). Built-in profiles cover the three example firmwares; further
 * profiles can be defined in the configuration file.
 */
#ifndef HUBUSD_PROFILE_H
#define HUBUSD_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#define HUBUSD_PROFILE_NAME_LEN 24
#define HUBUSD_CMD_MAX_ARGS 8

typedef enum {
    HUBUSD_COMP_SWITCH,        /* Home Assistant switch: state ON/OFF, commands ON/OFF */
    HUBUSD_COMP_BINARY_SENSOR, /* binary_sensor: state ON/OFF */
    HUBUSD_COMP_SENSOR         /* sensor: numeric state */
} hubusd_component_t;

typedef struct {
    uint8_t valid;
    uint8_t opcode;
    uint8_t n_args;
    uint8_t args[HUBUSD_CMD_MAX_ARGS];
} hubusd_cmd_t;

typedef struct {
    char name[HUBUSD_PROFILE_NAME_LEN];
    int device_type; /* first application byte of SLAVE_INFO; -1: not recognised automatically */
    hubusd_component_t component;
    uint8_t byte_offset; /* position of the value in the SLAVE_DATA payload */
    uint8_t byte_len;    /* 1 or 2 */
    uint8_t is_signed;
    uint8_t big_endian;
    double scale;        /* value = raw * scale + add */
    double add;
    uint8_t decimals;
    char unit[8];
    char device_class[24];
    hubusd_cmd_t on;     /* switch: command for ON */
    hubusd_cmd_t off;    /* switch: command for OFF */
} hubusd_profile_t;

/* Built-in profiles: "relay" (0x01), "switch" (0x02), "temperature" (0x03). NULL if unknown. */
const hubusd_profile_t *hubusd_builtin_profile(const char *name);
const hubusd_profile_t *hubusd_builtin_profile_by_type(int device_type);

/* Fills `p` with neutral defaults (sensor, one byte, scale 1). */
void hubusd_profile_init(hubusd_profile_t *p);

/*
 * Interprets a SLAVE_DATA payload. Switches and binary sensors give "ON" or
 * "OFF" (value != 0), sensors a decimal number. Returns the text length, or 0
 * if the payload is too short for the profile.
 */
size_t hubusd_profile_decode(const hubusd_profile_t *p, const uint8_t *data, uint8_t len, char *out, size_t out_size);

/* Maps the text of a `set` message ("ON", "OFF", case-insensitive) to a command. Returns 1 on success. */
int hubusd_profile_command(const hubusd_profile_t *p, const char *text, hubusd_cmd_t *cmd);

#endif /* HUBUSD_PROFILE_H */
