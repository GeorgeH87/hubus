#define _GNU_SOURCE
#include "hubus_master_gpiod.h"

#include <dirent.h>
#include <errno.h>
#include <gpiod.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct hubus_gpiod {
    struct gpiod_chip *chip;
    struct gpiod_line_request *clk_req;
    struct gpiod_line_request *data_req;
    unsigned clk_line;
    unsigned data_line;
};

/* Chip labels of the supported Raspberry Pi models. */
static const char *const known_labels[] = {
    "pinctrl-rp1",     /* Pi 5 */
    "pinctrl-bcm2711", /* Pi 4, Pi 400, CM4 */
    "pinctrl-bcm2835", /* Pi 3 and older SoC naming */
    "pinctrl-bcm2837",
    "pinctrl-bcm2710",
};

static int label_known(const char *label)
{
    size_t i;

    for (i = 0; i < sizeof known_labels / sizeof known_labels[0]; i++) {
        if (strcmp(label, known_labels[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static struct gpiod_chip *open_chip(const hubus_gpiod_cfg_t *cfg)
{
    DIR *dir;
    struct dirent *ent;

    if (cfg->chip_path) {
        return gpiod_chip_open(cfg->chip_path);
    }
    dir = opendir("/dev");
    if (!dir) {
        return NULL;
    }
    while ((ent = readdir(dir)) != NULL) {
        char path[300];
        struct gpiod_chip *chip;
        struct gpiod_chip_info *info;
        const char *label;
        int match;

        if (strncmp(ent->d_name, "gpiochip", 8) != 0) {
            continue;
        }
        snprintf(path, sizeof path, "/dev/%s", ent->d_name);
        chip = gpiod_chip_open(path);
        if (!chip) {
            continue;
        }
        info = gpiod_chip_get_info(chip);
        label = info ? gpiod_chip_info_get_label(info) : NULL;
        match = label && (cfg->chip_label ? strcmp(label, cfg->chip_label) == 0 : label_known(label));
        if (info) {
            gpiod_chip_info_free(info);
        }
        if (match) {
            closedir(dir);
            return chip;
        }
        gpiod_chip_close(chip);
    }
    closedir(dir);
    errno = ENODEV;
    return NULL;
}

static struct gpiod_line_request *request_line(struct gpiod_chip *chip, unsigned offset, int open_drain, int pullup)
{
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    struct gpiod_line_request *req = NULL;

    if (!settings || !line_cfg || !req_cfg) {
        goto out;
    }
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    /* CLK idles low; an open-drain DATA starts released (ACTIVE = not pulled low). */
    gpiod_line_settings_set_output_value(settings, open_drain ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
    if (open_drain) {
        gpiod_line_settings_set_drive(settings, GPIOD_LINE_DRIVE_OPEN_DRAIN);
        if (pullup) {
            gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
        }
    }
    if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) < 0) {
        goto out;
    }
    gpiod_request_config_set_consumer(req_cfg, "hubus");
    req = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
out:
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    return req;
}

hubus_gpiod_t *hubus_gpiod_open(const hubus_gpiod_cfg_t *cfg)
{
    hubus_gpiod_t *h = calloc(1, sizeof *h);

    if (!h) {
        return NULL;
    }
    h->clk_line = cfg->clk_line;
    h->data_line = cfg->data_line;
    h->chip = open_chip(cfg);
    if (!h->chip) {
        goto fail;
    }
    h->clk_req = request_line(h->chip, cfg->clk_line, 0, 0);
    h->data_req = request_line(h->chip, cfg->data_line, 1, cfg->internal_pullup);
    if (!h->clk_req || !h->data_req) {
        goto fail;
    }
    return h;
fail: {
    int err = errno;
    hubus_gpiod_close(h);
    errno = err;
}
    return NULL;
}

void hubus_gpiod_close(hubus_gpiod_t *h)
{
    if (!h) {
        return;
    }
    if (h->data_req) {
        gpiod_line_request_set_value(h->data_req, h->data_line, GPIOD_LINE_VALUE_ACTIVE);
        gpiod_line_request_release(h->data_req);
    }
    if (h->clk_req) {
        gpiod_line_request_set_value(h->clk_req, h->clk_line, GPIOD_LINE_VALUE_INACTIVE);
        gpiod_line_request_release(h->clk_req);
    }
    if (h->chip) {
        gpiod_chip_close(h->chip);
    }
    free(h);
}

/* ---- HAL ----------------------------------------------------------------- */

static void hal_set_clk(void *ctx, uint8_t high)
{
    hubus_gpiod_t *h = ctx;

    gpiod_line_request_set_value(h->clk_req, h->clk_line, high ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

static void hal_set_data_low(void *ctx, uint8_t low)
{
    hubus_gpiod_t *h = ctx;

    /* Open-drain: INACTIVE pulls the line low, ACTIVE releases it. */
    gpiod_line_request_set_value(h->data_req, h->data_line, low ? GPIOD_LINE_VALUE_INACTIVE : GPIOD_LINE_VALUE_ACTIVE);
}

static uint8_t hal_read_data(void *ctx)
{
    hubus_gpiod_t *h = ctx;

    return gpiod_line_request_get_value(h->data_req, h->data_line) == GPIOD_LINE_VALUE_ACTIVE ? 1u : 0u;
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* All timing values are minimums, so oversleeping only slows the bus down. */
static void hal_delay_ns(void *ctx, uint32_t ns)
{
    uint64_t end = now_ns() + ns;

    (void)ctx;
    if (ns > 300000u) { /* sleep for the bulk, spin for the rest */
        struct timespec ts;
        uint32_t sleep_ns = ns - 150000u;

        ts.tv_sec = 0;
        ts.tv_nsec = sleep_ns;
        nanosleep(&ts, NULL);
    }
    while (now_ns() < end) {
    }
}

hubus_master_hal_t hubus_gpiod_hal(hubus_gpiod_t *h)
{
    hubus_master_hal_t hal;

    hal.ctx = h;
    hal.set_clk = hal_set_clk;
    hal.set_data_low = hal_set_data_low;
    hal.read_data = hal_read_data;
    hal.delay_ns = hal_delay_ns;
    return hal;
}

int hubus_gpiod_set_realtime(int priority)
{
    struct sched_param sp;

    memset(&sp, 0, sizeof sp);
    sp.sched_priority = priority;
    return sched_setscheduler(0, SCHED_FIFO, &sp);
}
