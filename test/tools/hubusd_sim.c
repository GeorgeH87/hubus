/*
 * hubusd on a simulated bus, for trying out the MQTT interface without
 * hardware (and for the end-to-end script test/tools/e2e.sh).
 *
 *   hubusd_sim -c config.yaml
 *
 * Simulated devices: relay (MCID 3), button (MCID 5), temperature (MCID 7),
 * and an unconfigurable device (MCID 9). Commands on stdin:
 *
 *   press | release       button state change (announced like a real event)
 *   temp <raw adc value>  new sensor value (298 = 25 degC)
 *   unplug <mcid>         device stops answering
 *   plug <mcid>           device answers again
 *   quit
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hubus_sim.h"
#include "hubusd_run.h"

typedef struct {
    hubus_slave_core_t core;
    uint8_t type;
    uint8_t state;
    uint16_t raw_temp;
    volatile uint8_t temp_be[2]; /* live state of the temperature sensor */
    uint8_t hwid[HUBUS_HWID_SERIAL + 4u];
} device_t;

static hubus_sim_t sim;
static device_t devices[4];

static void capture(void *ctx)
{
    device_t *v = ctx;
    uint8_t b[2];

    if (v->type == 0x83u) {
        b[0] = (uint8_t)(v->raw_temp >> 8);
        b[1] = (uint8_t)v->raw_temp;
        (void)hubus_core_write(&v->core, b, 2, 0);
    } else {
        (void)hubus_core_write(&v->core, &v->state, 1, 0);
    }
}

static void command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    device_t *v = ctx;

    if (v->type == 0x81u && opcode == 0x10 && len >= 1) {
        v->state = args[0] ? 1 : 0;
        fprintf(stderr, "[sim] relay %u\n", v->state);
    }
}

/* The devices of this tool: the relay and the sensor have a state, the button does not. */
#define TYPE_RELAY 0x81u
#define TYPE_BUTTON 0x82u
#define TYPE_TEMPERATURE 0x83u
#define TYPE_UNKNOWN 0x97u

static const hubus_module_rule_t relay_rules[] = {{.pattern = {TYPE_RELAY}, .mask = {0xFF}, .len = 1}};
static const hubus_module_rule_t button_rules[] = {{.pattern = {TYPE_BUTTON}, .mask = {0xFF}, .len = 1}};
static const hubus_module_rule_t temperature_rules[] = {{.pattern = {TYPE_TEMPERATURE}, .mask = {0xFF}, .len = 1}};
static const hubus_entity_t relay_entity[] = {{
    .key = "relay",
    .name = "Relay",
    .component = HUBUS_COMPONENT_SWITCH,
    .decode = {.offset = 0, .len = 1, .scale = 1.0},
    .on = {.used = true, .opcode = 0x10, .n_args = 1, .args = {1}},
    .off = {.used = true, .opcode = 0x10, .n_args = 1, .args = {0}},
}};
static const hubus_entity_t button_entity[] = {
    {.key = "button", .name = "Button", .component = HUBUS_COMPONENT_BINARY_SENSOR, .decode = {.offset = 0, .len = 1, .scale = 1.0}},
};
static const hubus_entity_t temperature_entity[] = {{
    .key = "temperature",
    .name = "Temperature",
    .component = HUBUS_COMPONENT_SENSOR,
    .device_class = HUBUS_DEVICE_CLASS_TEMPERATURE,
    .unit = HUBUS_UNIT_CELSIUS,
    .decode = {.offset = 0, .len = 2, .big_endian = true, .scale = 1.0, .add = -273.0},
}};
static const hubus_module_t relay_module = {
    HUBUS_MODULE_INIT, .name = "relay", .rules = relay_rules, .n_rules = 1, .max_command = 31,
    .has_state = true, .has_events = true, .has_status_capture = true,
    .entities = relay_entity, .n_entities = 1,
};
static const hubus_module_t button_module = {
    HUBUS_MODULE_INIT, .name = "button", .rules = button_rules, .n_rules = 1, .max_command = 31,
    .has_events = true, .has_status_capture = true,
    .entities = button_entity, .n_entities = 1,
};
static const hubus_module_t temperature_module = {
    HUBUS_MODULE_INIT, .name = "temperature", .rules = temperature_rules, .n_rules = 1, .max_command = 31,
    .has_state = true, .has_events = true, .has_status_capture = true,
    .entities = temperature_entity, .n_entities = 1,
};
static const hubus_module_t *const sim_modules[] = {&relay_module, &button_module, &temperature_module};

static device_t *add(uint8_t mcid, uint8_t type)
{
    static unsigned n;
    device_t *v = &devices[n++];
    hubus_core_config_t c;

    v->type = type;
    v->raw_temp = 298;
    v->temp_be[0] = (uint8_t)(298 >> 8);
    v->temp_be[1] = (uint8_t)298;
    v->hwid[HUBUS_HWID_TYPE] = type;
    v->hwid[HUBUS_HWID_VARIANT] = 1;
    v->hwid[HUBUS_HWID_VERSION] = 1;
    v->hwid[HUBUS_HWID_SERIAL + 3u] = mcid;
    memset(&c, 0, sizeof c);
    c.mcid = mcid;
    c.hwid = v->hwid;
    c.hwid_len = sizeof v->hwid;
    c.on_status_capture = capture;
    c.on_command = command;
    if (type == TYPE_RELAY) { /* the relay and the sensor expose their state (REQUEST_STATE), the button does not */
        c.state = &v->state;
        c.state_len = 1;
    } else if (type == TYPE_TEMPERATURE) {
        c.state = v->temp_be;
        c.state_len = 2;
    }
    c.ctx = v;
    hubus_core_init(&v->core, &c, sim.clk, sim.data);
    hubus_sim_add(&sim, &v->core);
    return v;
}

static device_t *by_mcid(unsigned mcid)
{
    unsigned i;

    for (i = 0; i < 4; i++) {
        if (devices[i].core.mcid == mcid) {
            return &devices[i];
        }
    }
    return NULL;
}

static void hook(void *ctx, hubusd_t *d)
{
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    char line[128];
    unsigned mcid, raw;

    (void)ctx;
    (void)d;
    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) {
        return;
    }
    if (!fgets(line, sizeof line, stdin)) {
        hubusd_run_stop();
        return;
    }
    if (strncmp(line, "press", 5) == 0 || strncmp(line, "release", 7) == 0) {
        uint8_t pressed = line[0] == 'p';
        device_t *b = by_mcid(5);

        b->state = pressed;
        hubus_sim_slave_write(&sim, (unsigned)(b - devices), &pressed, 1, 0);
    } else if (sscanf(line, "temp %u", &raw) == 1) {
        by_mcid(7)->raw_temp = (uint16_t)raw;
        by_mcid(7)->temp_be[0] = (uint8_t)(raw >> 8);
        by_mcid(7)->temp_be[1] = (uint8_t)raw;
    } else if (sscanf(line, "unplug %u", &mcid) == 1 && by_mcid(mcid)) {
        hubus_sim_unplug(&sim, (unsigned)(by_mcid(mcid) - devices), 1);
    } else if (sscanf(line, "plug %u", &mcid) == 1 && by_mcid(mcid)) {
        hubus_sim_unplug(&sim, (unsigned)(by_mcid(mcid) - devices), 0);
    } else if (strncmp(line, "quit", 4) == 0) {
        hubusd_run_stop();
    }
}

int main(int argc, char **argv)
{
    hubusd_config_t *cfg = malloc(sizeof *cfg);
    hubusd_run_opts_t opts;
    hubus_master_hal_t hal;
    char err[300];

    if (argc != 3 || strcmp(argv[1], "-c") != 0) {
        fprintf(stderr, "usage: %s -c config.yaml\n", argv[0]);
        return 2;
    }
    if (hubusd_config_load(argv[2], cfg, err, sizeof err) != 0) {
        fprintf(stderr, "hubusd_sim: %s\n", err);
        return 1;
    }
    hubus_sim_init(&sim);
    add(3, TYPE_RELAY);
    add(5, TYPE_BUTTON);
    add(7, TYPE_TEMPERATURE);
    add(9, TYPE_UNKNOWN);
    hal = hubus_sim_master_hal(&sim);
    memset(&opts, 0, sizeof opts);
    opts.config_path = argv[2];
    opts.poll_hook = hook;
    opts.verbose = 1;
    opts.modules = sim_modules;
    opts.n_modules = sizeof sim_modules / sizeof sim_modules[0];
    setvbuf(stdout, NULL, _IOLBF, 0);
    return hubusd_run(cfg, &hal, &opts);
}
