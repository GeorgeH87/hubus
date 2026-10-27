/*
 * HUBUS master example for the Raspberry Pi.
 *
 *   hubus_master_example [options] <command> [args]
 *
 * Options:
 *   --chip LABEL   GPIO chip label (default: auto-detect Raspberry Pi chip)
 *   --clk N        GPIO offset of CLK  (default 17)
 *   --data N       GPIO offset of DATA (default 27)
 *   --rt PRIO      real-time priority (SCHED_FIFO, needs root)
 *   --pullup       enable the SoC pull-up on DATA (bench tests only)
 *
 * Commands:
 *   scan                       group REQUEST_INFO scan (one transaction per slave)
 *   burst                      startup sequence, then collect all pending data
 *   monitor                    startup, then serve idle events forever
 *   capture <mcid>             STATUS_CAPTURE to one slave (0 = all), then burst
 *   state <mcid>               REQUEST_STATE: the live state of one slave (0 = every slave that has one)
 *   info <mcid>                REQUEST_INFO to a single MCID
 *   send <mcid> <opcode> [b..] SEND_COMMAND to one slave (0 = all), numbers in C syntax
 *   update <mcid> <file.hex>   firmware update through the updater (docs/spec/06-operation.md section 6.8)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hubus_master_gpiod.h"
#include "hubus_update.h"

static void print_state(void *ctx, const hubus_slave_frame_t *f)
{
    unsigned i;

    (void)ctx;
    printf("mcid %2u state:", f->mcid);
    for (i = 0; i < f->len; i++) {
        printf(" %02X", f->payload[i]);
    }
    putchar('\n');
}

static void print_frame(void *ctx, const hubus_slave_frame_t *f, hubus_gen_result_t gen, uint8_t lost)
{
    static const char *const gen_names[] = {"baseline", "normal", "duplicate", "wrapped", "gap"};
    unsigned i;

    (void)ctx;
    printf("mcid %2u gen %u%s [%s", f->mcid, f->gen, f->more ? " more" : "", gen_names[gen]);
    if (gen == HUBUS_GEN_GAP) {
        printf(", %u lost", lost);
    }
    printf("] data:");
    for (i = 0; i < f->len; i++) {
        printf(" %02X", f->payload[i]);
    }
    printf("\n");
}

static void print_progress(void *ctx, uint16_t done, uint16_t total)
{
    (void)ctx;
    printf("\rpage %u of %u", done, total);
    fflush(stdout);
}

static int fail(const char *what, hubus_status_t s)
{
    fprintf(stderr, "%s: %s\n", what, hubus_status_str(s));
    return 1;
}

static int run_burst(hubus_master_t *m)
{
    hubus_burst_stats_t st;
    hubus_status_t s = hubus_master_burst(m, print_frame, NULL, &st);

    if (s != HUBUS_OK) {
        return fail("burst", s);
    }
    printf("burst: %u polls, %u served, %u duplicates, %u gaps, %u retries, %u skipped\n", st.polls, st.served,
           st.duplicates, st.gaps, st.retries, st.skipped);
    return 0;
}

static unsigned long number(const char *s)
{
    return strtoul(s, NULL, 0);
}

int main(int argc, char **argv)
{
    hubus_gpiod_cfg_t gcfg = {NULL, NULL, 17, 27, 0};
    hubus_master_cfg_t cfg;
    hubus_master_hal_t hal;
    hubus_master_t master;
    hubus_gpiod_t *gpio;
    hubus_status_t s;
    int rt = 0;
    int i = 1;
    int rc = 0;

    while (i < argc && strncmp(argv[i], "--", 2) == 0) {
        if (strcmp(argv[i], "--pullup") == 0) {
            gcfg.internal_pullup = 1;
            i++;
        } else if (i + 1 < argc && strcmp(argv[i], "--chip") == 0) {
            gcfg.chip_label = argv[i + 1];
            i += 2;
        } else if (i + 1 < argc && strcmp(argv[i], "--clk") == 0) {
            gcfg.clk_line = (unsigned)number(argv[i + 1]);
            i += 2;
        } else if (i + 1 < argc && strcmp(argv[i], "--data") == 0) {
            gcfg.data_line = (unsigned)number(argv[i + 1]);
            i += 2;
        } else if (i + 1 < argc && strcmp(argv[i], "--rt") == 0) {
            rt = (int)number(argv[i + 1]);
            i += 2;
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: %s [options] scan|burst|monitor|capture <mcid>|state <mcid>|info <mcid>|send <mcid> <opcode> [bytes]|update <mcid> <file.hex>\n", argv[0]);
        return 2;
    }

    gpio = hubus_gpiod_open(&gcfg);
    if (!gpio) {
        perror("open GPIO");
        return 1;
    }
    if (rt && hubus_gpiod_set_realtime(rt) != 0) {
        perror("real-time priority");
    }
    hal = hubus_gpiod_hal(gpio);
    hubus_master_cfg_default(&cfg);
    hubus_master_init(&master, &hal, &cfg);

    s = hubus_master_startup(&master);
    if (s != HUBUS_OK) {
        rc = fail("startup", s);
        goto done;
    }

    if (strcmp(argv[i], "scan") == 0) {
        static hubus_scan_t scan;
        unsigned mcid;

        s = hubus_master_scan(&master, &scan);
        if (s != HUBUS_OK) {
            rc = fail("scan", s);
            goto done;
        }
        for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
            if (scan.state[mcid] == HUBUS_SCAN_PRESENT) {
                const hubus_slave_frame_t *f = &scan.info[mcid];
                unsigned k;

                printf("mcid %2u: version %u caps 0x%02X max command %u app:", mcid, f->payload[HUBUS_INFO_VERSION],
                       f->payload[HUBUS_INFO_CAPS], f->payload[HUBUS_INFO_MAX_CMD]);
                for (k = HUBUS_INFO_APP; k < f->len; k++) {
                    printf(" %02X", f->payload[k]);
                }
                printf("\n");
            } else if (scan.state[mcid] == HUBUS_SCAN_SUSPECT) {
                printf("mcid %2u: suspected duplicate MCID or line problem\n", mcid);
            }
        }
    } else if (strcmp(argv[i], "burst") == 0) {
        rc = run_burst(&master);
    } else if (strcmp(argv[i], "monitor") == 0) {
        rc = run_burst(&master); /* collect what was pending before the reset */
        while (rc == 0) {
            hubus_burst_stats_t st;
            struct timespec pause = {0, 1000000}; /* 1 ms between idle checks */

            s = hubus_master_service(&master, print_frame, NULL, &st);
            if (s != HUBUS_OK) {
                rc = fail("service", s);
                break;
            }
            nanosleep(&pause, NULL);
        }
    } else if (strcmp(argv[i], "capture") == 0 && i + 1 < argc) {
        unsigned mcid = (unsigned)number(argv[i + 1]);

        s = hubus_master_status_capture(&master, mcid ? HUBUS_TARGET_LEN_EXACT : 0, (uint8_t)mcid);
        if (s != HUBUS_OK) {
            rc = fail("status capture", s);
        } else {
            rc = run_burst(&master);
        }
    } else if (strcmp(argv[i], "state") == 0 && i + 1 < argc) {
        unsigned mcid = (unsigned)number(argv[i + 1]);

        if (mcid == 0) {
            uint16_t n = 0;

            s = hubus_master_read_states(&master, print_state, NULL, &n);
            printf("%u slave(s) with a state\n", n);
        } else {
            hubus_slave_frame_t f;

            s = hubus_master_request_state(&master, (uint8_t)mcid, &f);
            if (s == HUBUS_OK) {
                print_state(NULL, &f);
            }
        }
        rc = s == HUBUS_OK ? 0 : fail("state", s);
    } else if (strcmp(argv[i], "info") == 0 && i + 1 < argc) {
        hubus_slave_frame_t f;
        unsigned k;

        s = hubus_master_request_info(&master, (uint8_t)number(argv[i + 1]), &f);
        if (s == HUBUS_OK) {
            printf("mcid %2u: version %u caps 0x%02X max command %u app:", f.mcid, f.payload[HUBUS_INFO_VERSION],
                   f.payload[HUBUS_INFO_CAPS], f.payload[HUBUS_INFO_MAX_CMD]);
            for (k = HUBUS_INFO_APP; k < f.len; k++) {
                printf(" %02X", f.payload[k]);
            }
            putchar('\n');
        }
        rc = s == HUBUS_OK ? 0 : fail("info", s);
    } else if (strcmp(argv[i], "send") == 0 && i + 2 < argc) {
        uint8_t args[HUBUS_MAX_PAYLOAD];
        unsigned mcid = (unsigned)number(argv[i + 1]);
        unsigned opcode = (unsigned)number(argv[i + 2]);
        unsigned n = 0;
        int k;

        for (k = i + 3; k < argc && n < sizeof args; k++) {
            args[n++] = (uint8_t)number(argv[k]);
        }
        s = hubus_master_send_command(&master, mcid ? HUBUS_TARGET_LEN_EXACT : 0, (uint8_t)mcid, (uint8_t)opcode, args,
                                      (uint8_t)n);
        rc = s == HUBUS_OK ? 0 : fail("send", s);
    } else if (strcmp(argv[i], "update") == 0 && i + 2 < argc) {
        static uint8_t image[65536];
        static char text[4 * 65536];
        hubus_update_image_t img;
        hubus_update_result_t res;
        FILE *fp = fopen(argv[i + 2], "rb");
        size_t n = fp ? fread(text, 1, sizeof text, fp) : 0;
        uint32_t len;

        if (fp) {
            fclose(fp);
        }
        if (!fp || n == sizeof text || hubus_ihex_parse(text, n, image, sizeof image, &len) != 0) {
            fprintf(stderr, "update: cannot read Intel HEX file %s\n", argv[i + 2]);
            rc = 1;
            goto done;
        }
        memset(&img, 0, sizeof img);
        img.data = image;
        img.len = len;
        s = hubus_master_update(&master, (uint8_t)number(argv[i + 1]), &img, print_progress, NULL, &res);
        putchar('\n');
        if (s == HUBUS_ERR_UPDATE) {
            fprintf(stderr, "update: %s (page 0x%04X, last result %u)\n", hubus_update_reason_str(res.reason), res.page,
                    res.last_result);
            rc = 1;
        } else {
            printf("%u pages written, %u repeated\n", res.pages, res.retries);
            rc = s == HUBUS_OK ? 0 : fail("update", s);
        }
    } else {
        fprintf(stderr, "unknown command %s\n", argv[i]);
        rc = 2;
    }

done:
    (void)hubus_master_shutdown(&master);
    hubus_gpiod_close(gpio);
    return rc;
}
