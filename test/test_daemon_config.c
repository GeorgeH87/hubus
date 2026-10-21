/* Configuration (YAML), device profiles, and the JSON writer of hubusd. */
#include <stdlib.h>
#include <string.h>

#include "hubusd_config.h"
#include "hubusd_json.h"
#include "hubusd_profile.h"
#include "hubusd_yaml.h"
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
    TEST_ASSERT_TRUE(cfg.mqtt.discovery);
    TEST_ASSERT_TRUE(cfg.bus.readback);
    TEST_ASSERT_EQUAL_UINT(0, cfg.n_slaves);
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
    "    profile: relay\n"
    "  - mcid: 5\n"
    "    readback: false\n"
    "  - mcid: 7\n"
    "    name: Cellar\n"
    "    profile: temperature\n";

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
    TEST_ASSERT_EQUAL_STRING("broker.local", cfg.mqtt.host);
    TEST_ASSERT_EQUAL_INT(8883, cfg.mqtt.port);
    TEST_ASSERT_TRUE(cfg.mqtt.tls);
    TEST_ASSERT_EQUAL_STRING("home/hubus", cfg.mqtt.base_topic);
    TEST_ASSERT_FALSE(cfg.mqtt.discovery);
    TEST_ASSERT_EQUAL_UINT(10, cfg.mqtt.stats_interval_s);
    TEST_ASSERT_FALSE(cfg.bus.readback);
    TEST_ASSERT_EQUAL_UINT(5, cfg.bus.ping_interval_s);
    TEST_ASSERT_EQUAL_UINT(3, cfg.n_slaves);
    TEST_ASSERT_EQUAL_UINT8(3, cfg.slaves[0].mcid);
    TEST_ASSERT_EQUAL_STRING("Garden relay", cfg.slaves[0].name);
    TEST_ASSERT_EQUAL_STRING("relay", cfg.slaves[0].profile);
    TEST_ASSERT_EQUAL_INT(-1, cfg.slaves[0].readback);
    TEST_ASSERT_EQUAL_STRING("HUBUS slave 5", cfg.slaves[1].name);
    TEST_ASSERT_EQUAL_INT(0, cfg.slaves[1].readback);
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
    expect_error("timing:\n  clk_high_us: 10\n  sample_us: 50\n", "sample_us");
    expect_error("slaves: 3\n", "must be a list");
    expect_error("slaves:\n  - name: x\n", "without 'mcid'");
    expect_error("slaves:\n  - mcid: 0\n", "between 1 and 63");
    expect_error("slaves:\n  - mcid: 64\n", "between 1 and 63");
    expect_error("slaves:\n  - mcid: 4\n  - mcid: 4\n", "configured twice");
    expect_error("slaves:\n  - mcid: 4\n    profile: toaster\n", "unknown profile 'toaster'");
    expect_error("slaves:\n  - 7\n", "mapping");
    expect_error("- a\n- b\n", "must be a mapping");
    expect_error("mqtt:\n  password_file: /does/not/exist\n", "password_file");
    expect_error("mqtt: {host: [\n", "line");
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
    TEST_ASSERT_EQUAL_UINT(63, cfg.n_slaves);
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

/* ---- custom profiles --------------------------------------------------------- */

static const char *custom_profiles =
    "profiles:\n"
    "  dimmer:\n"
    "    component: switch\n"
    "    device_type: 0x20\n"
    "    on:\n"
    "      opcode: 0x30\n"
    "      args: [255, 1]\n"
    "    off:\n"
    "      opcode: 0x30\n"
    "      args: [0]\n"
    "  pressure:\n"
    "    component: sensor\n"
    "    byte_offset: 1\n"
    "    byte_length: 2\n"
    "    byte_order: little\n"
    "    signed: true\n"
    "    scale: 0.1\n"
    "    offset: 900\n"
    "    decimals: 1\n"
    "    unit: hPa\n"
    "    device_class: pressure\n"
    "slaves:\n"
    "  - mcid: 9\n"
    "    profile: dimmer\n"
    "  - mcid: 10\n"
    "    profile: pressure\n";

static void test_custom_profiles(void)
{
    const hubusd_profile_t *d;
    const hubusd_profile_t *p;
    hubusd_cmd_t cmd;
    char out[32];
    const uint8_t data[] = {0xAA, 0x2C, 0x01}; /* little endian 0x012C = 300 */

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, parse(custom_profiles), err);
    d = hubusd_config_profile(&cfg, "dimmer");
    p = hubusd_config_profile(&cfg, "pressure");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(0x20, d->device_type);
    TEST_ASSERT_TRUE(hubusd_profile_command(d, "on", &cmd));
    TEST_ASSERT_EQUAL_UINT8(0x30, cmd.opcode);
    TEST_ASSERT_EQUAL_UINT8(2, cmd.n_args);
    TEST_ASSERT_EQUAL_UINT8(255, cmd.args[0]);
    TEST_ASSERT_TRUE(hubusd_profile_decode(p, data, 3, out, sizeof out) > 0);
    TEST_ASSERT_EQUAL_STRING("930.0", out); /* 300 * 0.1 + 900 */
    TEST_ASSERT_NOT_NULL(hubusd_config_profile(&cfg, "relay")); /* built-ins stay available */
}

static void test_profile_errors(void)
{
    expect_error("profiles:\n  x:\n    byte_offset: 1\n", "needs a 'component'");
    expect_error("profiles:\n  x:\n    component: lamp\n", "unknown component");
    expect_error("profiles:\n  relay:\n    component: sensor\n", "built in");
    expect_error("profiles:\n  x:\n    component: switch\n", "needs 'on' and 'off'");
    expect_error("profiles:\n  x:\n    component: switch\n    on:\n      opcode: 5\n    off:\n      opcode: 0x10\n",
                 "opcode");
    expect_error("profiles:\n  x:\n    component: sensor\n    byte_length: 3\n", "byte_length");
    expect_error("profiles:\n  x:\n    component: sensor\n    byte_order: middle\n", "byte_order");
    expect_error("profiles:\n  x:\n    component: switch\n    on:\n      opcode: 0x10\n      args: [1, 300]\n"
                 "    off:\n      opcode: 0x10\n",
                 "between 0 and 255");
    expect_error("profiles:\n  x:\n    component: sensor\n    byte_offset: 30\n    byte_length: 2\n", "payload");
}

/* ---- built-in profiles --------------------------------------------------------- */

static void test_builtin_profiles(void)
{
    const hubusd_profile_t *relay = hubusd_builtin_profile("relay");
    const hubusd_profile_t *sw = hubusd_builtin_profile("switch");
    const hubusd_profile_t *temp = hubusd_builtin_profile("temperature");
    hubusd_cmd_t cmd;
    char out[32];
    uint8_t on = 1, off = 0;
    const uint8_t raw[] = {0x01, 0x2A}; /* 298 -> 25 degC */

    TEST_ASSERT_NOT_NULL(relay);
    TEST_ASSERT_NOT_NULL(sw);
    TEST_ASSERT_NOT_NULL(temp);
    TEST_ASSERT_NULL(hubusd_builtin_profile("toaster"));
    TEST_ASSERT_EQUAL_PTR(relay, hubusd_builtin_profile_by_type(0x01));
    TEST_ASSERT_EQUAL_PTR(sw, hubusd_builtin_profile_by_type(0x02));
    TEST_ASSERT_EQUAL_PTR(temp, hubusd_builtin_profile_by_type(0x03));
    TEST_ASSERT_NULL(hubusd_builtin_profile_by_type(0x77));

    TEST_ASSERT_TRUE(hubusd_profile_decode(relay, &on, 1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("ON", out);
    TEST_ASSERT_TRUE(hubusd_profile_decode(relay, &off, 1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("OFF", out);
    TEST_ASSERT_EQUAL_UINT(0, hubusd_profile_decode(relay, &on, 0, out, sizeof out)); /* too short */
    TEST_ASSERT_TRUE(hubusd_profile_decode(temp, raw, 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("25", out);
    TEST_ASSERT_EQUAL_UINT(0, hubusd_profile_decode(temp, raw, 1, out, sizeof out));
    TEST_ASSERT_EQUAL_UINT(0, hubusd_profile_decode(relay, &on, 1, out, 2)); /* buffer too small */

    TEST_ASSERT_TRUE(hubusd_profile_command(relay, "ON", &cmd));
    TEST_ASSERT_EQUAL_UINT8(0x10, cmd.opcode);
    TEST_ASSERT_EQUAL_UINT8(1, cmd.args[0]);
    TEST_ASSERT_TRUE(hubusd_profile_command(relay, "off", &cmd));
    TEST_ASSERT_EQUAL_UINT8(0, cmd.args[0]);
    TEST_ASSERT_FALSE(hubusd_profile_command(relay, "toggle", &cmd));
    TEST_ASSERT_FALSE(hubusd_profile_command(sw, "ON", &cmd)); /* sensors take no commands */
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
    RUN_TEST(test_unknown_keys_are_warnings_with_line_numbers);
    RUN_TEST(test_error_classes);
    RUN_TEST(test_error_reports_the_right_line);
    RUN_TEST(test_too_many_slaves);
    RUN_TEST(test_password_from_file_and_environment);
    RUN_TEST(test_load_reports_the_file_name);
    RUN_TEST(test_custom_profiles);
    RUN_TEST(test_profile_errors);
    RUN_TEST(test_builtin_profiles);
    RUN_TEST(test_json_writer);
    RUN_TEST(test_json_overflow_is_sticky_and_safe);
    RUN_TEST(test_json_unbalanced_is_not_ok);
    return UNITY_END();
}
