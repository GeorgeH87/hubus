/*
 * Raspberry Pi HAL for the HUBUS master: Linux GPIO character device via
 * libgpiod (v2 API).
 *
 * CLK is a push-pull output. DATA is requested as open-drain output: the
 * kernel emulates open-drain by switching the pin between input (released)
 * and output low, so the master never drives DATA high. DATA needs an
 * external pull-up to 3.3 V.
 *
 * The GPIO chip is selected by its label, not by number, because chip
 * numbering differs between models and kernel versions (Pi 5: the GPIOs are
 * on the RP1 chip, label "pinctrl-rp1").
 */
#ifndef HUBUS_MASTER_GPIOD_H
#define HUBUS_MASTER_GPIOD_H

#include "hubus_master.h"

typedef struct hubus_gpiod hubus_gpiod_t;

typedef struct {
    const char *chip_label; /* e.g. "pinctrl-rp1" (Pi 5), "pinctrl-bcm2711" (Pi 4); NULL: first chip with a known label */
    const char *chip_path;  /* alternative to chip_label, e.g. "/dev/gpiochip0" */
    unsigned clk_line;      /* GPIO offset of CLK */
    unsigned data_line;     /* GPIO offset of DATA */
    int internal_pullup;    /* 1: enable the SoC pull-up on DATA (bench tests only; use an external pull-up) */
} hubus_gpiod_cfg_t;

/* Returns NULL on error (errno is set). */
hubus_gpiod_t *hubus_gpiod_open(const hubus_gpiod_cfg_t *cfg);

/* Releases both lines (CLK low, DATA released) and frees the handle. */
void hubus_gpiod_close(hubus_gpiod_t *h);

/* The HAL for hubus_master_init(). The handle must outlive the master. */
hubus_master_hal_t hubus_gpiod_hal(hubus_gpiod_t *h);

/* Real-time scheduling (SCHED_FIFO) for the calling thread; needs privileges. Returns 0 on success. */
int hubus_gpiod_set_realtime(int priority);

#endif /* HUBUS_MASTER_GPIOD_H */
