/*
 * hubusd: HUBUS master daemon with an MQTT interface for the Raspberry Pi.
 *
 *   hubusd [-c /etc/hubus/hubusd.yaml] [--check] [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hubusd_run.h"
#include "hubus_master_gpiod.h"

#define DEFAULT_CONFIG "/etc/hubus/hubusd.yaml"

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-c config.yaml] [--check] [-v] [--version]\n"
            "  -c FILE     configuration (default %s)\n"
            "  --check     validate the configuration and exit\n"
            "  -v          verbose logging\n",
            argv0, DEFAULT_CONFIG);
}

int main(int argc, char **argv)
{
    const char *path = DEFAULT_CONFIG;
    int check = 0;
    hubusd_run_opts_t opts;
    hubusd_config_t *cfg;
    hubus_gpiod_cfg_t gcfg;
    hubus_gpiod_t *gpio;
    hubus_master_hal_t hal;
    char err[300];
    unsigned i;
    int rc;
    int a;

    memset(&opts, 0, sizeof opts);
    for (a = 1; a < argc; a++) {
        if (strcmp(argv[a], "-c") == 0 && a + 1 < argc) {
            path = argv[++a];
        } else if (strcmp(argv[a], "--check") == 0) {
            check = 1;
        } else if (strcmp(argv[a], "-v") == 0) {
            opts.verbose = 1;
        } else if (strcmp(argv[a], "--version") == 0) {
            printf("hubusd %s\n", HUBUSD_VERSION);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    cfg = malloc(sizeof *cfg);
    if (!cfg) {
        return 1;
    }
    if (hubusd_config_load(path, cfg, err, sizeof err) != 0) {
        fprintf(stderr, "hubusd: invalid configuration: %s\n", err);
        free(cfg);
        return 1;
    }
    for (i = 0; i < cfg->n_warnings; i++) {
        fprintf(stderr, "hubusd: warning: %s\n", cfg->warnings[i]);
    }
    if (check) {
        printf("%s: ok (%u slaves, %u custom profiles)\n", path, cfg->n_slaves, cfg->n_profiles);
        free(cfg);
        return 0;
    }

    memset(&gcfg, 0, sizeof gcfg);
    gcfg.chip_label = cfg->gpio.chip[0] ? cfg->gpio.chip : NULL;
    gcfg.chip_path = cfg->gpio.path[0] ? cfg->gpio.path : NULL;
    gcfg.clk_line = cfg->gpio.clk_line;
    gcfg.data_line = cfg->gpio.data_line;
    gcfg.internal_pullup = cfg->gpio.internal_pullup;
    gpio = hubus_gpiod_open(&gcfg);
    if (!gpio) {
        perror("hubusd: cannot open the GPIO lines");
        free(cfg);
        return 1;
    }
    if (cfg->gpio.realtime && hubus_gpiod_set_realtime(cfg->gpio.realtime) != 0) {
        perror("hubusd: warning: cannot set real-time priority (needs CAP_SYS_NICE)");
    }
    hal = hubus_gpiod_hal(gpio);
    opts.config_path = path;
    rc = hubusd_run(cfg, &hal, &opts);
    hubus_gpiod_close(gpio);
    free(cfg);
    return rc;
}
