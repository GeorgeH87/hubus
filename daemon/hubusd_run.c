#include "hubusd_run.h"

#include <errno.h>
#include <mosquitto.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t stop_flag;
static volatile sig_atomic_t reload_flag;

void hubusd_run_stop(void)
{
    stop_flag = 1;
}

static void on_signal(int sig)
{
    if (sig == SIGHUP) {
        reload_flag = 1;
    } else {
        stop_flag = 1;
    }
}

/* ---- clock and log ---------------------------------------------------------------- */

static uint64_t mono_ms(void *ctx)
{
    struct timespec ts;

    (void)ctx;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t wall_s(void *ctx)
{
    (void)ctx;
    return (uint64_t)time(NULL);
}

static void log_to_stderr(void *ctx, int level, const char *msg)
{
    static const char *const prefix[] = {"<6>", "<4>", "<3>"}; /* syslog priorities understood by journald */
    const hubusd_run_opts_t *o = ctx;

    if (level == 0 && !o->verbose) {
        return;
    }
    fprintf(stderr, "%shubusd: %s\n", prefix[level < 0 || level > 2 ? 0 : level], msg);
}

/* ---- MQTT ------------------------------------------------------------------------------ */

typedef struct {
    struct mosquitto *client;
    hubusd_t *d;
} mq_t;

static int mq_publish(void *ctx, const char *topic, const char *payload, size_t len, int retain)
{
    mq_t *m = ctx;
    int rc = mosquitto_publish(m->client, NULL, topic, (int)len, payload, retain ? 1 : 0, retain != 0);

    return rc == MOSQ_ERR_SUCCESS ? 0 : -1;
}

static void subscribe_one(void *ctx, const char *filter)
{
    mq_t *m = ctx;

    mosquitto_subscribe(m->client, NULL, filter, 1);
}

static void cb_connect(struct mosquitto *client, void *obj, int rc)
{
    mq_t *m = obj;

    (void)client;
    if (rc != 0) {
        fprintf(stderr, "<3>hubusd: MQTT connection refused: %s\n", mosquitto_connack_string(rc));
        return;
    }
    fprintf(stderr, "<6>hubusd: connected to the MQTT broker\n");
    hubusd_subscriptions(m->d, subscribe_one, m);
    hubusd_on_connect(m->d);
}

static void cb_disconnect(struct mosquitto *client, void *obj, int rc)
{
    (void)client;
    (void)obj;
    if (rc != 0) {
        fprintf(stderr, "<4>hubusd: lost the MQTT connection (%s), reconnecting\n", mosquitto_strerror(rc));
    }
}

static void cb_message(struct mosquitto *client, void *obj, const struct mosquitto_message *msg)
{
    mq_t *m = obj;

    (void)client;
    hubusd_handle_message(m->d, msg->topic, msg->payload, (size_t)msg->payloadlen);
}

/* ---- reload -------------------------------------------------------------------------------- */

static void do_reload(hubusd_t *d, const char *path)
{
    hubusd_config_t *nc = malloc(sizeof *nc);
    char err[300];
    unsigned i;

    if (!nc) {
        return;
    }
    if (!path || hubusd_config_load(path, nc, err, sizeof err) != 0) {
        fprintf(stderr, "<3>hubusd: reload failed, keeping the old configuration: %s\n", path ? err : "no configuration file");
        free(nc);
        return;
    }
    for (i = 0; i < nc->n_warnings; i++) {
        fprintf(stderr, "<4>hubusd: %s\n", nc->warnings[i]);
    }
    hubusd_request_reload(d, nc);
}

/* ---- run ---------------------------------------------------------------------------------------- */

int hubusd_run(const hubusd_config_t *cfg, const hubus_master_hal_t *hal, const hubusd_run_opts_t *opts)
{
    hubusd_t *d = malloc(sizeof *d);
    mq_t mq;
    hubusd_mqtt_t iface;
    hubusd_clock_t clk = {NULL, mono_ms, wall_s};
    struct sigaction sa;
    char status_topic[160];
    int rc;
    int exit_code = 0;

    if (!d) {
        return 1;
    }
    mosquitto_lib_init();
    mq.client = mosquitto_new(cfg->mqtt.client_id, true, &mq);
    if (!mq.client) {
        fprintf(stderr, "<3>hubusd: cannot create the MQTT client\n");
        free(d);
        mosquitto_lib_cleanup();
        return 1;
    }
    iface.ctx = &mq;
    iface.publish = mq_publish;
    hubusd_init(d, cfg, hal, iface, clk);
    mq.d = d;
    hubusd_set_logger(d, log_to_stderr, (void *)opts);

    if (cfg->mqtt.username[0]) {
        mosquitto_username_pw_set(mq.client, cfg->mqtt.username, cfg->mqtt.password[0] ? cfg->mqtt.password : NULL);
    }
    if (cfg->mqtt.tls) {
        rc = mosquitto_tls_set(mq.client, cfg->mqtt.cafile[0] ? cfg->mqtt.cafile : NULL,
                               cfg->mqtt.cafile[0] ? NULL : "/etc/ssl/certs", NULL, NULL, NULL);
        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "<3>hubusd: TLS setup failed: %s\n", mosquitto_strerror(rc));
            exit_code = 1;
            goto done;
        }
    }
    hubusd_status_topic(d, status_topic, sizeof status_topic);
    mosquitto_will_set(mq.client, status_topic, 7, "offline", 1, true);
    mosquitto_reconnect_delay_set(mq.client, 1, 30, true);
    mosquitto_connect_callback_set(mq.client, cb_connect);
    mosquitto_disconnect_callback_set(mq.client, cb_disconnect);
    mosquitto_message_callback_set(mq.client, cb_message);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    stop_flag = 0;
    reload_flag = 0;

    rc = mosquitto_connect_async(mq.client, cfg->mqtt.host, cfg->mqtt.port, 30);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "<4>hubusd: MQTT connect to %s:%d failed (%s), will keep retrying\n", cfg->mqtt.host,
                cfg->mqtt.port, mosquitto_strerror(rc));
    }
    rc = mosquitto_loop_start(mq.client);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "<3>hubusd: cannot start the MQTT thread: %s\n", mosquitto_strerror(rc));
        exit_code = 1;
        goto done;
    }

    while (!stop_flag) {
        int busy;

        if (reload_flag) {
            reload_flag = 0;
            do_reload(d, opts->config_path);
        }
        if (opts->poll_hook) {
            opts->poll_hook(opts->poll_ctx, d);
        }
        busy = hubusd_step(d);
        if (!busy) {
            struct timespec pause = {0, 2000000}; /* 2 ms */

            nanosleep(&pause, NULL);
        }
    }

    hubusd_shutdown(d);
    {
        struct timespec flush = {0, 300000000}; /* let the MQTT thread send the offline messages */

        nanosleep(&flush, NULL);
    }
    mosquitto_disconnect(mq.client);
    mosquitto_loop_stop(mq.client, false);
done:
    mosquitto_destroy(mq.client);
    hubusd_destroy(d);
    free(d);
    mosquitto_lib_cleanup();
    return exit_code;
}
