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
    uint8_t info[6];
} device_t;

static hubus_sim_t sim;
static device_t devices[4];

static void capture(void *ctx)
{
    device_t *v = ctx;
    uint8_t b[2];

    if (v->type == 3) {
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

    if (v->type == 1 && opcode == 0x10 && len >= 1) {
        v->state = args[0] ? 1 : 0;
        fprintf(stderr, "[sim] relay %u\n", v->state);
    }
}

static device_t *add(uint8_t mcid, uint8_t type)
{
    static unsigned n;
    device_t *v = &devices[n++];
    hubus_core_config_t c;

    v->type = type;
    v->raw_temp = 298;
    v->temp_be[0] = (uint8_t)(298 >> 8);
    v->temp_be[1] = (uint8_t)298;
    v->info[0] = type;
    v->info[1] = 1;
    v->info[5] = mcid;
    memset(&c, 0, sizeof c);
    c.mcid = mcid;
    c.info = v->info;
    c.info_len = sizeof v->info;
    c.on_status_capture = capture;
    c.on_command = command;
    if (type == 1) { /* the relay and the sensor expose their state (REQUEST_STATE), the button does not */
        c.state = &v->state;
        c.state_len = 1;
    } else if (type == 3) {
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
        by_mcid(mcid)->core.enabled = 0;
    } else if (sscanf(line, "plug %u", &mcid) == 1 && by_mcid(mcid)) {
        by_mcid(mcid)->core.enabled = 1;
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
    add(3, 1);
    add(5, 2);
    add(7, 3);
    add(9, 0x77);
    hal = hubus_sim_master_hal(&sim);
    memset(&opts, 0, sizeof opts);
    opts.config_path = argv[2];
    opts.poll_hook = hook;
    opts.verbose = 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    return hubusd_run(cfg, &hal, &opts);
}
