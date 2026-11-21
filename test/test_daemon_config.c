/* Configuration (YAML) and the JSON writer of hubusd. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hubusd_config.h"
#include "hubus_json.h"
#include "hubus_module_ha.h"
#include "hubusd_yaml.h"
#include "hubusd_modules.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static hubusd_config_t cfg;
static char err[256];

static int parse(const char *text)
{
    return hubusd_config_parse(text, strlen(text), &cfg, err, sizeof err);
}

static void expect_error(const char *text, const char *needle)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, parse(text), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, needle), err);
}

/* The longest times fit the nanoseconds of the master. */
static void test_longest_times(void)
{
    TEST_ASSERT_EQUAL_INT(0, parse("timing:\n  clk_low_us: 1000000\n  clk_high_us: 1000000\n  sample_us: 1000000\n"
                                   "  cond_us: 1000000\n  turnaround_us: 1000000\n  class_margin_us: 1000000\n"));
    TEST_ASSERT_EQUAL_UINT32(1000000000u, cfg.master.t_clk_low_ns);
    TEST_ASSERT_EQUAL_UINT32(1000000000u, cfg.master.t_clk_high_ns);
    TEST_ASSERT_EQUAL_UINT32(1000000000u, cfg.master.t_sample_ns);
    TEST_ASSERT_EQUAL_UINT32(1000000000u, cfg.master.t_cond_ns);
    TEST_ASSERT_EQUAL_UINT32(1000000000u, cfg.master.t_turnaround_ns);
    TEST_ASSERT_EQUAL_UINT32(1000000000u, cfg.master.t_class_margin_ns);
}

/* ---- YAML tree ------------------------------------------------------------ */

static void test_yaml_tree_and_line_numbers(void)
{
    const char *text = "a: 1\nb:\n  - x\n  - y: 2\nc: {d: e}\n";
    char e[128];
    yn_t *root = yn_parse(text, strlen(text), e, sizeof e);
    yn_t *b;

    TEST_ASSERT_NOT_NULL_MESSAGE(root, e);
    TEST_ASSERT_EQUAL(YN_MAP, root->type);
    TEST_ASSERT_EQUAL_STRING("1", yn_get(root, "a")->text);
    b = yn_get(root, "b");
    TEST_ASSERT_EQUAL(YN_SEQ, b->type);
    TEST_ASSERT_EQUAL_UINT(2, b->n);
    TEST_ASSERT_EQUAL_INT(3, b->item[0]->line);
    TEST_ASSERT_EQUAL_STRING("2", yn_get(b->item[1], "y")->text);
    TEST_ASSERT_EQUAL_STRING("e", yn_get(yn_get(root, "c"), "d")->text);
    TEST_ASSERT_NULL(yn_get(root, "missing"));
    yn_free(root);
}

static void test_yaml_errors(void)
{
    char e[128];
    const char *bad[] = {"a: [1, 2\n", "a: &x 1\nb: *x\n", "a: 1\n---\nb: 2\n", "a:\n\t- b\n"};
    unsigned i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        yn_t *root = yn_parse(bad[i], strlen(bad[i]), e, sizeof e);
        TEST_ASSERT_NULL_MESSAGE(root, bad[i]);
        TEST_ASSERT_NOT_NULL(strstr(e, "line "));
    }
}

static void test_empty_file_is_an_empty_mapping(void)
{
    char e[64];
    yn_t *root = yn_parse("", 0, e, sizeof e);

    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL(YN_MAP, root->type);
    yn_free(root);
}

/* ---- configuration --------------------------------------------------------- */

static void test_empty_configuration_gives_defaults(void)
{
    TEST_ASSERT_EQUAL_INT(0, parse(""));
    TEST_ASSERT_EQUAL_STRING("localhost", cfg.mqtt.host);
    TEST_ASSERT_EQUAL_INT(1883, cfg.mqtt.port);
    TEST_ASSERT_EQUAL_STRING("hubus", cfg.mqtt.base_topic);
    TEST_ASSERT_EQUAL_STRING("homeassistant", cfg.mqtt.discovery_prefix);
    TEST_ASSERT_TRUE(cfg.rt.mqtt.discovery);
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_SLAVE, cfg.rt.bus.readback);
    TEST_ASSERT_EQUAL_UINT(0, cfg.rt.n_slaves);
    TEST_ASSERT_EQUAL_UINT(0, cfg.n_warnings);
    TEST_ASSERT_EQUAL_UINT(50000, cfg.master.t_clk_low_ns);
}

static const char *full_config =
    "gpio:\n"
    "  chip: pinctrl-rp1\n"
    "  clk: 5\n"
    "  data: 6\n"
    "  realtime: 50\n"
    "timing:\n"
    "  clk_low_us: 100\n"
    "  clk_high_us: 120\n"
    "  sample_us: 60\n"
    "  cond_us: 90\n"
    "  turnaround_us: 300\n"
    "  retry_budget: 4\n"
    "  line_fault_ticks: 400\n"
    "  follow_fragments: false\n"
    "  max_fragments: 8\n"
    "  duplicate_threshold: 5\n"
    "  line_problem_count: 6\n"
    "mqtt:\n"
    "  host: broker.local\n"
    "  port: 8883\n"
    "  username: hub\n"
    "  password: secret\n"
    "  tls: true\n"
    "  cafile: /etc/ssl/ca.pem\n"
    "  base_topic: home/hubus\n"
    "  client_id: pi\n"
    "  discovery: false\n"
    "  stats_interval_s: 10\n"
    "bus:\n"
    "  readback: no\n"
    "  ping_interval_s: 5\n"
    "slaves:\n"
    "  - mcid: 3\n"
    "    name: Garden relay\n"
    "  - mcid: 5\n"
    "    readback: false\n"
    "  - mcid: 7\n"
    "    name: Cellar\n";

static void test_full_configuration(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, parse(full_config), err);
    TEST_ASSERT_EQUAL_STRING("pinctrl-rp1", cfg.gpio.chip);
    TEST_ASSERT_EQUAL_UINT(5, cfg.gpio.clk_line);
    TEST_ASSERT_EQUAL_UINT(6, cfg.gpio.data_line);
    TEST_ASSERT_EQUAL_INT(50, cfg.gpio.realtime);
    TEST_ASSERT_EQUAL_UINT(100000, cfg.master.t_clk_low_ns);
    TEST_ASSERT_EQUAL_UINT(120000, cfg.master.t_clk_high_ns);
    TEST_ASSERT_EQUAL_UINT(60000, cfg.master.t_sample_ns);
    TEST_ASSERT_EQUAL_UINT(90000, cfg.master.t_cond_ns);
    TEST_ASSERT_EQUAL_UINT(300000, cfg.master.t_turnaround_ns);
    TEST_ASSERT_EQUAL_UINT(4, cfg.master.retry_budget);
    TEST_ASSERT_EQUAL_UINT(400, cfg.master.line_fault_ticks);
    TEST_ASSERT_EQUAL_UINT(0, cfg.master.follow_more);
    TEST_ASSERT_EQUAL_UINT(8, cfg.master.max_fragments);
    TEST_ASSERT_EQUAL_UINT(5, cfg.master.dup_fail_threshold);
    TEST_ASSERT_EQUAL_UINT(6, cfg.master.line_problem_count);
    TEST_ASSERT_EQUAL_STRING("broker.local", cfg.mqtt.host);
    TEST_ASSERT_EQUAL_INT(8883, cfg.mqtt.port);
    TEST_ASSERT_TRUE(cfg.mqtt.tls);
    TEST_ASSERT_EQUAL_STRING("home/hubus", cfg.mqtt.base_topic);
    TEST_ASSERT_FALSE(cfg.rt.mqtt.discovery);
    TEST_ASSERT_EQUAL_UINT(10, cfg.rt.mqtt.stats_interval_s);
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_NONE, cfg.rt.bus.readback);
    TEST_ASSERT_EQUAL_UINT(5, cfg.rt.bus.ping_interval_s);
    TEST_ASSERT_EQUAL_UINT(3, cfg.rt.n_slaves);
    TEST_ASSERT_EQUAL_UINT8(3, cfg.rt.slaves[0].mcid);
    TEST_ASSERT_EQUAL_STRING("Garden relay", cfg.rt.slaves[0].name);
    TEST_ASSERT_EQUAL_INT(-1, cfg.rt.slaves[0].readback);
    TEST_ASSERT_EQUAL_STRING("HUBUS slave 5", cfg.rt.slaves[1].name);
    TEST_ASSERT_EQUAL_INT(0, cfg.rt.slaves[1].readback);
    TEST_ASSERT_EQUAL_UINT(0, cfg.n_warnings);
}

static void test_unknown_keys_are_warnings_with_line_numbers(void)
{
    TEST_ASSERT_EQUAL_INT(0, parse("mqtt:\n  host: h\n  hots: x\nfoo: 1\nslaves:\n  - mcid: 2\n    colour: red\n"));
    TEST_ASSERT_EQUAL_UINT(3, cfg.n_warnings);
    TEST_ASSERT_NOT_NULL(strstr(cfg.warnings[0], "line 3"));
    TEST_ASSERT_NOT_NULL(strstr(cfg.warnings[0], "hots"));
    TEST_ASSERT_NOT_NULL(strstr(cfg.warnings[1], "colour"));
    TEST_ASSERT_NOT_NULL(strstr(cfg.warnings[2], "foo"));
}

static void test_error_classes(void)
{
    expect_error("mqtt: 5\n", "line 1");
    expect_error("mqtt:\n  port: abc\n", "'port' must be a number");
    expect_error("mqtt:\n  port: 70000\n", "between 1 and 65535");
    expect_error("mqtt:\n  tls: maybe\n", "true or false");
    expect_error("mqtt:\n  base_topic: a/+/b\n", "base_topic");
    expect_error("mqtt:\n  base_topic: /abc\n", "base_topic");
    expect_error("mqtt:\n  discovery_prefix: \"\"\n", "discovery_prefix");
    expect_error("mqtt:\n  host: [a, b]\n", "single value");
    expect_error("gpio:\n  clk: 4\n  data: 4\n", "different lines");
    expect_error("gpio:\n  realtime: 200\n", "between 0 and 99");
    expect_error("timing:\n  clk_low_us: 0\n", "clk_low_us");
    expect_error("timing:\n  clk_low_us: 1000001\n", "between 1 and 1000000");
    expect_error("timing:\n  turnaround_us: 10000000\n", "between 1 and 1000000"); /* would overflow in ns */
    expect_error("timing:\n  class_margin_us: 1000001\n", "between 0 and 1000000");
    expect_error("timing:\n  clk_high_us: 10\n  sample_us: 50\n", "sample_us");
    expect_error("slaves: 3\n", "must be a list");
    expect_error("slaves:\n  - name: x\n", "without 'mcid'");
    expect_error("slaves:\n  - mcid: 0\n", "between 1 and 63");
    expect_error("slaves:\n  - mcid: 64\n", "between 1 and 63");
    expect_error("slaves:\n  - mcid: 4\n  - mcid: 4\n", "configured twice");
    expect_error("slaves:\n  - 7\n", "mapping");
    expect_error("- a\n- b\n", "must be a mapping");
    expect_error("mqtt:\n  password_file: /does/not/exist\n", "password_file");
    expect_error("mqtt: {host: [\n", "line");
}

static void test_modules_directory(void)
{
    TEST_ASSERT_EQUAL_INT(0, parse(""));
    TEST_ASSERT_EQUAL_STRING("", cfg.modules.dir); /* only the built-in modules */
    TEST_ASSERT_EQUAL_INT(0, parse("modules:\n  dir: /usr/local/lib/hubus/modules\n"));
    TEST_ASSERT_EQUAL_STRING("/usr/local/lib/hubus/modules", cfg.modules.dir);
    TEST_ASSERT_EQUAL_INT(0, parse("modules:\n  folder: /x\n"));
    TEST_ASSERT_EQUAL_UINT(1, cfg.n_warnings);
    TEST_ASSERT_NOT_NULL(strstr(cfg.warnings[0], "folder"));
    expect_error("modules: nothing\n", "mapping");
}

typedef struct {
    unsigned loaded, refused;
} plugin_reports_t;

static void count_reports(void *ctx, const char *path, const hubus_load_result_t *r)
{
    plugin_reports_t *c = ctx;

    (void)path;
    if (r->status == HUBUS_LOAD_OK) {
        c->loaded++;
    } else {
        c->refused++;
    }
}

static void test_the_plugins_of_the_directory_join_the_built_in_modules(void)
{
    static hubus_module_set_t set;
    plugin_reports_t rep = {0, 0};
    size_t builtin, i;

    (void)hubusd_builtin_modules(&builtin);
    TEST_ASSERT_EQUAL_INT(0, parse("modules:\n  dir: " HUBUS_TEST_PLUGIN_DIR "\n"));
    hubus_module_set_init(&set);
    set.trust_all = true; /* the build directory may live on a file system without permissions */
    TEST_ASSERT_EQUAL_UINT(2, hubusd_load_modules(&cfg, &set, count_reports, &rep));
    TEST_ASSERT_EQUAL_UINT(2, rep.loaded);
    TEST_ASSERT_EQUAL_UINT(5, rep.refused);
    TEST_ASSERT_EQUAL_UINT(builtin + 2u, set.n);
    for (i = 0; i < set.n; i++) {
        TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, hubus_module_validate(set.module[i]));
    }
    hubus_module_set_unload(&set);

    TEST_ASSERT_EQUAL_INT(0, parse(""));
    hubus_module_set_init(&set);
    TEST_ASSERT_EQUAL_UINT(0, hubusd_load_modules(&cfg, &set, count_reports, &rep)); /* no directory: built-in only */
    TEST_ASSERT_EQUAL_UINT(builtin, set.n);
    hubus_module_set_unload(&set);
}

static void test_readback_modes(void)
{
    TEST_ASSERT_EQUAL_INT(0, parse("bus:\n  readback: cache\nslaves:\n  - mcid: 1\n    readback: none\n  - mcid: 2\n    readback: slave\n  - mcid: 3\n    readback: cache\n  - mcid: 4\n"));
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_CACHE, cfg.rt.bus.readback);
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_NONE, cfg.rt.slaves[0].readback);
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_SLAVE, cfg.rt.slaves[1].readback);
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_CACHE, cfg.rt.slaves[2].readback);
    TEST_ASSERT_EQUAL_INT(-1, cfg.rt.slaves[3].readback);
    TEST_ASSERT_EQUAL_INT(0, parse(""));
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_SLAVE, cfg.rt.bus.readback); /* default */
    TEST_ASSERT_EQUAL_INT(0, parse("bus:\n  readback: true\n"));
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_SLAVE, cfg.rt.bus.readback);
    TEST_ASSERT_EQUAL_INT(0, parse("bus:\n  readback: false\n"));
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_NONE, cfg.rt.bus.readback);
    expect_error("bus:\n  readback: sometimes\n", "slave, cache, or none");
    expect_error("slaves:\n  - mcid: 1\n    readback: later\n", "slave, cache, default, or none");
    expect_error("bus:\n  readback: default\n", "slave, cache, or none"); /* only a slave has a default to go back to */
    TEST_ASSERT_EQUAL_INT(0, parse("slaves:\n  - mcid: 1\n    readback: default\n"));
    TEST_ASSERT_EQUAL_INT(-1, cfg.rt.slaves[0].readback);
}

static void test_poll_interval(void)
{
    TEST_ASSERT_EQUAL_INT(0, parse("slaves:\n  - mcid: 1\n    poll_interval_s: 60\n  - mcid: 2\n"));
    TEST_ASSERT_EQUAL_UINT(60, cfg.rt.slaves[0].poll_interval_s);
    TEST_ASSERT_EQUAL_UINT(0, cfg.rt.slaves[1].poll_interval_s); /* default: no polling */
    TEST_ASSERT_EQUAL_UINT(0, cfg.n_warnings);
    expect_error("slaves:\n  - mcid: 1\n    poll_interval_s: -1\n", "between 0 and 86400");
}

static void test_error_reports_the_right_line(void)
{
    TEST_ASSERT_EQUAL_INT(-1, parse("gpio:\n  clk: 4\nmqtt:\n  host: a\n  port: x\n"));
    TEST_ASSERT_NOT_NULL(strstr(err, "line 5:"));
}

static void test_too_many_slaves(void)
{
    char text[8192] = "slaves:\n";
    unsigned i;

    for (i = 1; i <= 63; i++) {
        char line[40];
        snprintf(line, sizeof line, "  - mcid: %u\n", i);
        strcat(text, line);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, parse(text), err);
    TEST_ASSERT_EQUAL_UINT(63, cfg.rt.n_slaves);
}

static void test_password_from_file_and_environment(void)
{
    char path[] = "/tmp/hubusd-pw-XXXXXX";
    char text[200];
    int fd = mkstemp(path);
    FILE *f;

    TEST_ASSERT_TRUE(fd >= 0);
    f = fdopen(fd, "w");
    fputs("from-file\nsecond line ignored\n", f);
    fclose(f);
    snprintf(text, sizeof text, "mqtt:\n  password: inline\n  password_file: %s\n", path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, parse(text), err);
    TEST_ASSERT_EQUAL_STRING("from-file", cfg.mqtt.password);

    setenv("HUBUSD_MQTT_PASSWORD", "from-env", 1);
    TEST_ASSERT_EQUAL_INT(0, parse(text));
    TEST_ASSERT_EQUAL_STRING("from-env", cfg.mqtt.password);
    unsetenv("HUBUSD_MQTT_PASSWORD");
    remove(path);
}

static void test_load_reports_the_file_name(void)
{
    TEST_ASSERT_EQUAL_INT(-1, hubusd_config_load("/does/not/exist.yaml", &cfg, err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "/does/not/exist.yaml"));
}

/* ---- the runtime part ---------------------------------------------------------- */

static const char *main_yaml = "gpio:\n  clk: 5\n  data: 6\nmqtt:\n  host: broker\n  stats_interval_s: 30\n"
                               "bus:\n  ping_interval_s: 20\nslaves:\n  - mcid: 3\n    name: Relay\n"
                               "  - mcid: 5\n    name: Button\n    settings:\n      module: switch\n      mode: \"2\"\n";

/* Over MQTT the change gets the runtime part only; as a runtime file it goes through the loader, warnings and all. */
#define HUBUSD_MERGE_FILE 0
#define HUBUSD_MERGE_MQTT 1

static int merge(const char *text, int mode)
{
    return mode == HUBUSD_MERGE_MQTT ? hubusd_config_merge(text, strlen(text), &cfg.rt, err, sizeof err)
                                     : hubusd_config_load_runtime(text, strlen(text), &cfg, err, sizeof err);
}

static int merge_slave(const char *text, unsigned mcid)
{
    return hubusd_config_merge_slave(text, strlen(text), &cfg.rt, mcid, err, sizeof err);
}

static void expect_merge_error(const char *text, const char *needle)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, merge(text, HUBUSD_MERGE_MQTT), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, needle), err);
}

static void test_the_runtime_file_has_a_default(void)
{
    TEST_ASSERT_EQUAL_INT(0, parse(""));
    TEST_ASSERT_EQUAL_STRING("/var/lib/hubus/runtime.yaml", cfg.runtime_file);
    TEST_ASSERT_EQUAL_INT(0, parse("runtime_file: \"\"\n"));
    TEST_ASSERT_EQUAL_STRING("", cfg.runtime_file);
}

/* JSON is YAML too: a change over MQTT names what it changes, and the rest stays. */
static void test_a_change_over_mqtt_changes_only_what_it_names(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, parse(main_yaml), err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, merge("{\"bus\":{\"ping_interval_s\":10,\"readback\":\"cache\"},"
                                           "\"mqtt\":{\"discovery\":false,\"discovery_cleanup\":true}}", HUBUSD_MERGE_MQTT),
                                  err);
    TEST_ASSERT_EQUAL_UINT(10, cfg.rt.bus.ping_interval_s);
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_CACHE, cfg.rt.bus.readback);
    TEST_ASSERT_EQUAL_UINT(3, cfg.rt.bus.ping_failures);  /* not named: as it was */
    TEST_ASSERT_EQUAL_UINT(30, cfg.rt.mqtt.stats_interval_s);
    TEST_ASSERT_FALSE(cfg.rt.mqtt.discovery);
    TEST_ASSERT_TRUE(cfg.rt.mqtt.discovery_cleanup);
    TEST_ASSERT_EQUAL_STRING("broker", cfg.mqtt.host);
    TEST_ASSERT_EQUAL_UINT(2, cfg.rt.n_slaves);
    TEST_ASSERT_EQUAL_INT(0, merge("{}", HUBUSD_MERGE_MQTT));
}

static void test_the_fixed_part_cannot_change_at_runtime(void)
{
    TEST_ASSERT_EQUAL_INT(0, parse(main_yaml));
    expect_merge_error("{\"gpio\":{\"clk\":4}}", "'gpio' can only be set in the main configuration file");
    expect_merge_error("{\"timing\":{}}", "'timing' can only be set");
    expect_merge_error("{\"modules\":{\"dir\":\"/tmp\"}}", "'modules' can only be set");
    expect_merge_error("{\"runtime_file\":\"/tmp/x\"}", "'runtime_file' can only be set");
    expect_merge_error("{\"mqtt\":{\"host\":\"evil\"}}", "'mqtt.host' can only be set");
    expect_merge_error("{\"mqtt\":{\"base_topic\":\"x\"}}", "'mqtt.base_topic' can only be set");
    TEST_ASSERT_EQUAL_INT(-1, merge("gpio:\n  clk: 4\n", HUBUSD_MERGE_FILE)); /* nor in the runtime file */
    TEST_ASSERT_EQUAL_UINT(5, cfg.gpio.clk_line);
}

/* Over MQTT nobody reads warnings: what hubusd does not know is an error, as are values out of range. */
static void test_a_change_over_mqtt_is_checked_strictly(void)
{
    TEST_ASSERT_EQUAL_INT(0, parse(main_yaml));
    expect_merge_error("{\"bus\":{\"ping_intervall_s\":5}}", "unknown key 'ping_intervall_s' in 'bus'");
    expect_merge_error("{\"busses\":{}}", "unknown key 'busses'");
    expect_merge_error("{\"bus\":{\"ping_failures\":0}}", "between 1 and 100");
    expect_merge_error("{\"mqtt\":{\"discovery\":\"maybe\"}}", "true or false");
    expect_merge_error("[1, 2]", "must be a mapping");
    expect_merge_error("{\"bus\": [", "line");
    TEST_ASSERT_EQUAL_INT(0, merge("bus:\n  ping_intervall_s: 5\n", HUBUSD_MERGE_FILE)); /* a file: a warning */
    TEST_ASSERT_EQUAL_UINT(1, cfg.n_warnings);
}

/* A list of slaves replaces the list; over MQTT the settings stay with the modules. */
static void test_a_list_of_slaves_replaces_the_list(void)
{
    TEST_ASSERT_EQUAL_INT(0, parse(main_yaml));
    TEST_ASSERT_EQUAL_STRING("switch", cfg.rt.slaves[1].settings.module);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, merge("{\"slaves\":[{\"mcid\":5,\"name\":\"Door\",\"settings\":{\"module\":\"x\",\"mode\":\"9\"}},"
                                           "{\"mcid\":9,\"readback\":\"none\",\"poll_interval_s\":60}]}",
                                           HUBUSD_MERGE_MQTT),
                                  err);
    TEST_ASSERT_EQUAL_UINT(2, cfg.rt.n_slaves);
    TEST_ASSERT_EQUAL_UINT8(5, cfg.rt.slaves[0].mcid);
    TEST_ASSERT_EQUAL_STRING("Door", cfg.rt.slaves[0].name);
    TEST_ASSERT_EQUAL_STRING("switch", cfg.rt.slaves[0].settings.module); /* kept, not taken from the message */
    TEST_ASSERT_EQUAL_STRING("2", cfg.rt.slaves[0].settings.item[0].value);
    TEST_ASSERT_EQUAL_STRING("HUBUS slave 9", cfg.rt.slaves[1].name);
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_NONE, cfg.rt.slaves[1].readback);
    TEST_ASSERT_EQUAL_UINT(60, cfg.rt.slaves[1].poll_interval_s);
    TEST_ASSERT_EQUAL_UINT8(0, cfg.rt.slaves[1].settings.n);
    TEST_ASSERT_EQUAL_INT(0, merge("{\"slaves\":[]}", HUBUSD_MERGE_MQTT));
    TEST_ASSERT_EQUAL_UINT(0, cfg.rt.n_slaves);
    expect_merge_error("{\"slaves\":[{\"mcid\":4},{\"mcid\":4}]}", "configured twice");
    expect_merge_error("{\"slaves\":[{\"mcid\":4,\"colour\":1}]}", "unknown key 'colour' in 'slaves'");
}

static void test_one_slave_is_added_changed_and_removed(void)
{
    hubusd_slave_cfg_t *sl;

    TEST_ASSERT_EQUAL_INT(0, parse(main_yaml));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, merge_slave("{\"name\":\"Pump\"}", 9), err);
    TEST_ASSERT_EQUAL_UINT(3, cfg.rt.n_slaves);
    sl = hubusd_config_slave(&cfg.rt, 9);
    TEST_ASSERT_NOT_NULL(sl);
    TEST_ASSERT_EQUAL_STRING("Pump", sl->name);
    TEST_ASSERT_EQUAL_INT(-1, sl->readback);

    TEST_ASSERT_EQUAL_INT(0, merge_slave("{\"readback\":\"cache\",\"mcid\":9}", 9));
    TEST_ASSERT_EQUAL_STRING("Pump", sl->name); /* stays */
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_CACHE, sl->readback);
    TEST_ASSERT_EQUAL_INT(0, merge_slave("readback: default\n", 9));
    TEST_ASSERT_EQUAL_INT(-1, sl->readback);
    TEST_ASSERT_EQUAL_INT(0, merge_slave("", 11)); /* empty: the defaults */
    TEST_ASSERT_EQUAL_STRING("HUBUS slave 11", hubusd_config_slave(&cfg.rt, 11)->name);

    TEST_ASSERT_EQUAL_INT(-1, merge_slave("{\"mcid\":8}", 9));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "is not the slave of the topic"), err);
    TEST_ASSERT_EQUAL_INT(-1, merge_slave("{\"nme\":\"x\"}", 9));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "unknown key 'nme'"), err);
    TEST_ASSERT_EQUAL_INT(-1, merge_slave("{}", 64));
    TEST_ASSERT_EQUAL_UINT(4, cfg.rt.n_slaves);

    TEST_ASSERT_EQUAL_INT(0, hubusd_config_remove_slave(&cfg.rt, 3));
    TEST_ASSERT_EQUAL_INT(-1, hubusd_config_remove_slave(&cfg.rt, 3));
    TEST_ASSERT_NULL(hubusd_config_slave(&cfg.rt, 3));
    TEST_ASSERT_EQUAL_UINT(3, cfg.rt.n_slaves);
    TEST_ASSERT_EQUAL_STRING("Button", cfg.rt.slaves[0].name);
}

static void test_settings_are_checked(void)
{
    char text[600];
    unsigned i;
    int n;

    expect_error("slaves:\n  - mcid: 1\n    settings:\n      level: \"1\"\n", "settings without 'module'");
    expect_error("slaves:\n  - mcid: 1\n    settings:\n      module: m\n      bad key: \"1\"\n", "setting 'bad key'");
    expect_error("slaves:\n  - mcid: 1\n    settings:\n      module: m\n      level:\n        - 1\n", "setting 'level'");
    n = snprintf(text, sizeof text, "slaves:\n  - mcid: 1\n    settings:\n      module: m\n");
    for (i = 0; i <= HUBUS_MODULE_SETTINGS_MAX; i++) {
        n += snprintf(text + n, sizeof text - (size_t)n, "      k%u: v\n", i);
    }
    expect_error(text, "more than 8 settings");
    TEST_ASSERT_TRUE(hubus_module_setting_valid("threshold", "50"));
    TEST_ASSERT_TRUE(hubus_module_setting_valid("a-b_C9", NULL));
    TEST_ASSERT_FALSE(hubus_module_setting_valid("module", "x")); /* the name of the module has this key */
    TEST_ASSERT_FALSE(hubus_module_setting_valid("", "x"));
    TEST_ASSERT_FALSE(hubus_module_setting_valid("a/b", "x"));
    TEST_ASSERT_FALSE(hubus_module_setting_valid("aaaaaaaaaaaaaaaaaaaaaaaa", "x")); /* 24 characters */
    TEST_ASSERT_FALSE(hubus_module_setting_valid("k", "line\nbreak"));
}

/* What hubusd writes, it reads back as it was: names with quotes, colons and backslashes included. */
static void test_the_runtime_file_reads_back_what_was_written(void)
{
    static hubusd_config_t back;
    static char text[8192];
    size_t len;

    TEST_ASSERT_EQUAL_INT(0, parse(main_yaml));
    snprintf(cfg.rt.slaves[0].name, sizeof cfg.rt.slaves[0].name, "Say \"hi\": a\\b #1 \x01");
    cfg.rt.slaves[0].readback = HUBUSD_READBACK_NONE;
    cfg.rt.slaves[0].poll_interval_s = 7;
    cfg.rt.mqtt.discovery = 0;
    cfg.rt.bus.scan_interval_s = 0;
    len = hubusd_config_format_runtime(&cfg.rt, text, sizeof text);
    TEST_ASSERT_TRUE(len > 0u);
    TEST_ASSERT_EQUAL_UINT(0, hubusd_config_format_runtime(&cfg.rt, text, 40)); /* too small: nothing */
    len = hubusd_config_format_runtime(&cfg.rt, text, sizeof text);

    hubusd_config_defaults(&back);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hubusd_config_load_runtime(text, len, &back, err, sizeof err), err);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, back.n_warnings, text);
    TEST_ASSERT_EQUAL_UINT(2, back.rt.n_slaves);
    TEST_ASSERT_EQUAL_STRING(cfg.rt.slaves[0].name, back.rt.slaves[0].name);
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_NONE, back.rt.slaves[0].readback);
    TEST_ASSERT_EQUAL_UINT(7, back.rt.slaves[0].poll_interval_s);
    TEST_ASSERT_EQUAL_INT(-1, back.rt.slaves[1].readback);
    TEST_ASSERT_EQUAL_STRING("switch", back.rt.slaves[1].settings.module);
    TEST_ASSERT_EQUAL_UINT8(1, back.rt.slaves[1].settings.n);
    TEST_ASSERT_EQUAL_STRING("mode", back.rt.slaves[1].settings.item[0].key);
    TEST_ASSERT_EQUAL_STRING("2", back.rt.slaves[1].settings.item[0].value);
    TEST_ASSERT_EQUAL_UINT(20, back.rt.bus.ping_interval_s);
    TEST_ASSERT_EQUAL_UINT(0, back.rt.bus.scan_interval_s);
    TEST_ASSERT_EQUAL_UINT(30, back.rt.mqtt.stats_interval_s);
    TEST_ASSERT_FALSE(back.rt.mqtt.discovery);

    cfg.rt.n_slaves = 0;
    len = hubusd_config_format_runtime(&cfg.rt, text, sizeof text);
    TEST_ASSERT_NOT_NULL(strstr(text, "slaves: []\n"));
    back.rt.n_slaves = 5;
    TEST_ASSERT_EQUAL_INT(0, hubusd_config_load_runtime(text, len, &back, err, sizeof err));
    TEST_ASSERT_EQUAL_UINT(0, back.rt.n_slaves);
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    TEST_ASSERT_NOT_NULL(f);
    fputs(text, f);
    fclose(f);
}

/* The runtime file overrides the main file where it has something; without one the main file has the start values. */
static void test_load_takes_the_runtime_file_over_the_main_file(void)
{
    char dir[] = "/tmp/hubusd-rt-XXXXXX";
    char main_path[64], rt_path[64], text[600];

    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    snprintf(main_path, sizeof main_path, "%s/hubusd.yaml", dir);
    snprintf(rt_path, sizeof rt_path, "%s/runtime.yaml", dir);
    snprintf(text, sizeof text, "runtime_file: %s\n%s", rt_path, main_yaml);
    write_text(main_path, text);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hubusd_config_load(main_path, &cfg, err, sizeof err), err);
    TEST_ASSERT_EQUAL_UINT(2, cfg.rt.n_slaves); /* no runtime file yet */
    TEST_ASSERT_EQUAL_UINT(0, cfg.n_warnings);

    cfg.rt.n_slaves = 1;
    snprintf(cfg.rt.slaves[0].name, sizeof cfg.rt.slaves[0].name, "Only one");
    cfg.rt.bus.ping_interval_s = 99;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hubusd_config_write_runtime(&cfg.rt, rt_path, err, sizeof err), err);
    snprintf(text, sizeof text, "%s.new", rt_path);
    TEST_ASSERT_NOT_EQUAL(0, access(text, F_OK)); /* replaced, not left beside it */

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hubusd_config_load(main_path, &cfg, err, sizeof err), err);
    TEST_ASSERT_EQUAL_UINT(1, cfg.rt.n_slaves);
    TEST_ASSERT_EQUAL_STRING("Only one", cfg.rt.slaves[0].name);
    TEST_ASSERT_EQUAL_UINT(99, cfg.rt.bus.ping_interval_s);
    TEST_ASSERT_EQUAL_STRING("broker", cfg.mqtt.host); /* the fixed part is from the main file */
    TEST_ASSERT_EQUAL_UINT(5, cfg.gpio.clk_line);
    TEST_ASSERT_EQUAL_UINT(1, cfg.n_warnings); /* the main file has a runtime part as well */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cfg.warnings[0], "overrides"), cfg.warnings[0]);

    write_text(rt_path, "bus:\n  ping_interval_s: 5\n  pingg: 1\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hubusd_config_load(main_path, &cfg, err, sizeof err), err);
    TEST_ASSERT_EQUAL_UINT(2, cfg.rt.n_slaves); /* the runtime file has none: those of the main file */
    TEST_ASSERT_EQUAL_UINT(5, cfg.rt.bus.ping_interval_s);
    TEST_ASSERT_EQUAL_UINT(2, cfg.n_warnings);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cfg.warnings[0], rt_path), cfg.warnings[0]);

    write_text(rt_path, "gpio:\n  clk: 1\n");
    TEST_ASSERT_EQUAL_INT(-1, hubusd_config_load(main_path, &cfg, err, sizeof err));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, rt_path), err);

    snprintf(text, sizeof text, "%s/missing/runtime.yaml", dir);
    TEST_ASSERT_EQUAL_INT(-1, hubusd_config_write_runtime(&cfg.rt, text, err, sizeof err));
    remove(rt_path);
    remove(main_path);
    rmdir(dir);
}

/* The two example files: the main one has the fixed part only, the runtime one the rest, and both without a warning. */
static void test_the_example_files_go_together(void)
{
    char main_path[300], rt_path[300], text[700];
    char dir[] = "/tmp/hubusd-ex-XXXXXX";
    char copy[64];
    char line[300];
    FILE *in, *out;

    snprintf(main_path, sizeof main_path, "%s/daemon/hubusd.example.yaml", HUBUS_SOURCE_DIR);
    snprintf(rt_path, sizeof rt_path, "%s/daemon/hubusd.runtime.example.yaml", HUBUS_SOURCE_DIR);
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    snprintf(copy, sizeof copy, "%s/hubusd.yaml", dir);
    in = fopen(main_path, "r");
    out = fopen(copy, "w");
    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_NOT_NULL(out);
    while (fgets(line, sizeof line, in)) {
        if (strncmp(line, "runtime_file:", 13) == 0) {
            fprintf(out, "runtime_file: %s\n", rt_path); /* the example next to it */
        } else {
            fputs(line, out);
        }
    }
    fclose(in);
    fclose(out);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hubusd_config_load(copy, &cfg, err, sizeof err), err);
    snprintf(text, sizeof text, "%u warnings, the first: %s", cfg.n_warnings, cfg.n_warnings ? cfg.warnings[0] : "");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, cfg.n_warnings, text);
    TEST_ASSERT_EQUAL_UINT(6, cfg.rt.n_slaves);
    TEST_ASSERT_EQUAL_INT(HUBUSD_READBACK_CACHE, cfg.rt.slaves[2].readback);
    TEST_ASSERT_EQUAL_STRING("distance", cfg.rt.slaves[5].settings.module);
    TEST_ASSERT_EQUAL_STRING("45", cfg.rt.slaves[5].settings.item[0].value);
    remove(copy);
    rmdir(dir);
}

/* ---- profiles are gone --------------------------------------------------------- */

static void test_profiles_are_no_longer_supported(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, parse("profiles:\n  x:\n    component: sensor\nslaves:\n  - mcid: 4\n    profile: x\n"), err);
    TEST_ASSERT_EQUAL_UINT(2, cfg.n_warnings);
    TEST_ASSERT_NOT_NULL(strstr(cfg.warnings[0], "no longer supported"));
    TEST_ASSERT_NOT_NULL(strstr(cfg.warnings[0], "module"));
    TEST_ASSERT_NOT_NULL(strstr(cfg.warnings[1], "no longer supported"));
}

static void test_the_built_in_modules_are_valid_and_have_entities(void)
{
    const hubus_module_t *const *modules;
    size_t n, i;
    unsigned with_entities = 0;

    modules = hubusd_builtin_modules(&n);
    TEST_ASSERT_TRUE(n > 0u);
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(HUBUS_MODULE_VALID, hubus_module_validate(modules[i]), modules[i]->name);
        TEST_ASSERT_TRUE_MESSAGE(hubus_ha_valid(modules[i]), modules[i]->name); /* all of them use the table */
        with_entities += hubus_ha_of(modules[i])->n_entities > 0u;
    }
    TEST_ASSERT_EQUAL_UINT(n - 1u, with_entities); /* all but the updater */
}

/* ---- JSON writer ------------------------------------------------------------------- */

static void test_json_writer(void)
{
    char buf[256];
    jw_t w;

    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_str(&w, "name", "a \"quoted\"\\ \n value");
    jw_int(&w, "neg", -5);
    jw_uint(&w, "big", 4000000000ull);
    jw_bool(&w, "yes", 1);
    jw_double(&w, "pi", 3.14159, 2);
    jw_null(&w, "nothing");
    jw_arr_begin(&w, "list");
    jw_int(&w, NULL, 1);
    jw_obj_begin(&w, NULL);
    jw_str(&w, "k", "v");
    jw_obj_end(&w);
    jw_arr_end(&w);
    jw_obj_end(&w);
    TEST_ASSERT_TRUE(jw_ok(&w));
    TEST_ASSERT_EQUAL_STRING("{\"name\":\"a \\\"quoted\\\"\\\\ \\n value\",\"neg\":-5,\"big\":4000000000,\"yes\":true,"
                             "\"pi\":3.14,\"nothing\":null,\"list\":[1,{\"k\":\"v\"}]}",
                             buf);
}

static void test_json_overflow_is_sticky_and_safe(void)
{
    char buf[16];
    jw_t w;

    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_str(&w, "key", "a value that is much too long for the buffer");
    jw_str(&w, "more", "x");
    jw_obj_end(&w);
    TEST_ASSERT_FALSE(jw_ok(&w));
    TEST_ASSERT_TRUE(strlen(buf) < sizeof buf);
}

static void test_json_unbalanced_is_not_ok(void)
{
    char buf[32];
    jw_t w;

    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    TEST_ASSERT_FALSE(jw_ok(&w));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_yaml_tree_and_line_numbers);
    RUN_TEST(test_yaml_errors);
    RUN_TEST(test_empty_file_is_an_empty_mapping);
    RUN_TEST(test_empty_configuration_gives_defaults);
    RUN_TEST(test_full_configuration);
    RUN_TEST(test_longest_times);
    RUN_TEST(test_unknown_keys_are_warnings_with_line_numbers);
    RUN_TEST(test_error_classes);
    RUN_TEST(test_modules_directory);
    RUN_TEST(test_the_plugins_of_the_directory_join_the_built_in_modules);
    RUN_TEST(test_readback_modes);
    RUN_TEST(test_poll_interval);
    RUN_TEST(test_error_reports_the_right_line);
    RUN_TEST(test_too_many_slaves);
    RUN_TEST(test_password_from_file_and_environment);
    RUN_TEST(test_load_reports_the_file_name);
    RUN_TEST(test_the_runtime_file_has_a_default);
    RUN_TEST(test_a_change_over_mqtt_changes_only_what_it_names);
    RUN_TEST(test_the_fixed_part_cannot_change_at_runtime);
    RUN_TEST(test_a_change_over_mqtt_is_checked_strictly);
    RUN_TEST(test_a_list_of_slaves_replaces_the_list);
    RUN_TEST(test_one_slave_is_added_changed_and_removed);
    RUN_TEST(test_settings_are_checked);
    RUN_TEST(test_the_runtime_file_reads_back_what_was_written);
    RUN_TEST(test_load_takes_the_runtime_file_over_the_main_file);
    RUN_TEST(test_the_example_files_go_together);
    RUN_TEST(test_profiles_are_no_longer_supported);
    RUN_TEST(test_the_built_in_modules_are_valid_and_have_entities);
    RUN_TEST(test_json_writer);
    RUN_TEST(test_json_overflow_is_sticky_and_safe);
    RUN_TEST(test_json_unbalanced_is_not_ok);
    return UNITY_END();
}
