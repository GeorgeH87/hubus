/*
 * Master modules as plugins (docs/spec/06-operation.md section 6.10): loading shared libraries, the checks before a module
 * is used, and conflicts between modules.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hubus_module_loader.h"
#include "unity.h"

#define PLUGIN(name) HUBUS_TEST_PLUGIN_DIR "/" name ".so"

static hubus_module_set_t set;

void setUp(void)
{
    hubus_module_set_init(&set);
    set.trust_all = true; /* the build directory may live on a file system without permissions */
}

void tearDown(void)
{
    hubus_module_set_unload(&set);
}

static hubus_load_result_t load(const char *name)
{
    char path[512];

    snprintf(path, sizeof path, "%s/%s.so", HUBUS_TEST_PLUGIN_DIR, name);
    return hubus_module_set_load_file(&set, path);
}

/* ---- permissions -------------------------------------------------------------------------------------------------- */

static void test_only_files_of_root_or_the_user_that_others_cannot_write_are_safe(void)
{
    TEST_ASSERT_TRUE(hubus_load_mode_ok(0, 0755, 1000));
    TEST_ASSERT_TRUE(hubus_load_mode_ok(1000, 0644, 1000));
    TEST_ASSERT_TRUE(hubus_load_mode_ok(1000, 0500, 1000));
    TEST_ASSERT_FALSE(hubus_load_mode_ok(1001, 0644, 1000)); /* another user */
    TEST_ASSERT_FALSE(hubus_load_mode_ok(1000, 0664, 1000)); /* group can write */
    TEST_ASSERT_FALSE(hubus_load_mode_ok(1000, 0646, 1000)); /* others can write */
    TEST_ASSERT_FALSE(hubus_load_mode_ok(0, 0666, 1000));
}

static void copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    FILE *out = fopen(to, "wb");
    char buf[4096];
    size_t n;

    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_NOT_NULL(out);
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        TEST_ASSERT_EQUAL_UINT(n, fwrite(buf, 1, n, out));
    }
    fclose(in);
    fclose(out);
}

static void test_a_file_or_directory_that_others_can_write_is_not_loaded(void)
{
    char dir[] = "/tmp/hubus-plugins-XXXXXX";
    char path[300];
    hubus_load_result_t r;

    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/copy.so", dir);
    copy_file(PLUGIN("a_good"), path);
    set.trust_all = false;

    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0666));
    r = hubus_module_set_load_file(&set, path);
    TEST_ASSERT_EQUAL(HUBUS_LOAD_UNSAFE, r.status);
    TEST_ASSERT_EQUAL_UINT(0, set.n);

    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0755));
    r = hubus_module_set_load_file(&set, path);
    TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, r.status);
    hubus_module_set_unload(&set);
    set.trust_all = false;

    TEST_ASSERT_EQUAL_INT(0, chmod(dir, 0777));
    TEST_ASSERT_EQUAL_UINT(0, hubus_module_set_load_dir(&set, dir, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, chmod(dir, 0700));
    TEST_ASSERT_EQUAL_UINT(1, hubus_module_set_load_dir(&set, dir, NULL, NULL));

    hubus_module_set_unload(&set);
    unlink(path);
    rmdir(dir);
}

/* ---- one file ----------------------------------------------------------------------------------------------------- */

static void test_a_plugin_is_loaded_and_selected_by_its_rules(void)
{
    const uint8_t hwid[] = {0x90, 0x01, 0x02, 0x00, 0x00, 0x00, 0x2A, 0x17};
    const hubus_module_t *out = NULL;
    hubus_load_result_t r = load("a_good");

    TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, r.status);
    TEST_ASSERT_EQUAL_STRING("plugin_good", r.name);
    TEST_ASSERT_EQUAL_UINT(1, set.n);
    TEST_ASSERT_NOT_NULL(set.plugin[0]);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_FOUND, hubus_module_select(set.module, set.n, hwid, sizeof hwid, &out));
    TEST_ASSERT_EQUAL_PTR(set.module[0], out);
    TEST_ASSERT_EQUAL_STRING("temperature", out->entities[0].key);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, hubus_module_validate(out));
}

static void test_the_more_specific_rule_of_another_plugin_wins(void)
{
    const uint8_t v5[] = {0x90, 0x01, 0x05, 0x00};
    const uint8_t v6[] = {0x90, 0x01, 0x06, 0x00};
    const hubus_module_t *out;

    TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, load("a_good").status);
    TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, load("b_override").status);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_FOUND, hubus_module_select(set.module, set.n, v5, sizeof v5, &out));
    TEST_ASSERT_EQUAL_STRING("plugin_override", out->name);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_FOUND, hubus_module_select(set.module, set.n, v6, sizeof v6, &out));
    TEST_ASSERT_EQUAL_STRING("plugin_good", out->name);
}

static void test_a_plugin_that_cannot_be_used_is_refused_and_closed(void)
{
    hubus_load_result_t r;

    r = load("c_bad_api");
    TEST_ASSERT_EQUAL(HUBUS_LOAD_INVALID, r.status);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_API, r.invalid);
    TEST_ASSERT_EQUAL(HUBUS_LOAD_NO_ENTRY, load("f_no_entry").status);
    TEST_ASSERT_EQUAL(HUBUS_LOAD_NO_MODULE, load("g_null").status);
    TEST_ASSERT_EQUAL(HUBUS_LOAD_OPEN, load("does_not_exist").status);
    TEST_ASSERT_EQUAL_UINT(0, set.n);
}

static void test_a_name_in_use_and_equally_specific_rules_are_refused(void)
{
    hubus_load_result_t r;

    TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, load("a_good").status);
    r = load("e_dup_name");
    TEST_ASSERT_EQUAL(HUBUS_LOAD_DUPLICATE, r.status);
    TEST_ASSERT_EQUAL_PTR(set.module[0], r.module);
    r = load("d_conflict");
    TEST_ASSERT_EQUAL(HUBUS_LOAD_CONFLICT, r.status);
    TEST_ASSERT_EQUAL_STRING("plugin_conflict", r.name);
    TEST_ASSERT_EQUAL_PTR(set.module[0], r.module);
    TEST_ASSERT_EQUAL_UINT(1, set.n);
}

static void test_the_same_file_twice_is_a_duplicate(void)
{
    TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, load("a_good").status);
    TEST_ASSERT_EQUAL(HUBUS_LOAD_DUPLICATE, load("a_good").status);
    TEST_ASSERT_EQUAL_UINT(1, set.n);
}

/* ---- a directory -------------------------------------------------------------------------------------------------- */

typedef struct {
    unsigned n;
    hubus_load_status_t status[16];
    char file[16][32];
} reports_t;

static void collect(void *ctx, const char *path, const hubus_load_result_t *r)
{
    reports_t *rep = ctx;
    const char *base = strrchr(path, '/');

    TEST_ASSERT_TRUE(rep->n < 16u);
    rep->status[rep->n] = r->status;
    snprintf(rep->file[rep->n], sizeof rep->file[0], "%s", base ? base + 1 : path);
    rep->n++;
}

static void test_a_directory_is_read_in_the_order_of_the_names(void)
{
    static const hubus_load_status_t expect[] = {HUBUS_LOAD_OK,       HUBUS_LOAD_OK,        HUBUS_LOAD_INVALID,
                                                 HUBUS_LOAD_CONFLICT, HUBUS_LOAD_DUPLICATE, HUBUS_LOAD_NO_ENTRY,
                                                 HUBUS_LOAD_NO_MODULE};
    static const char *const files[] = {"a_good.so",     "b_override.so", "c_bad_api.so", "d_conflict.so",
                                        "e_dup_name.so", "f_no_entry.so", "g_null.so"};
    reports_t rep;
    unsigned i;

    memset(&rep, 0, sizeof rep);
    TEST_ASSERT_EQUAL_UINT(2, hubus_module_set_load_dir(&set, HUBUS_TEST_PLUGIN_DIR, collect, &rep));
    TEST_ASSERT_EQUAL_UINT(2, set.n);
    TEST_ASSERT_EQUAL_UINT(7, rep.n);
    for (i = 0; i < rep.n; i++) {
        TEST_ASSERT_EQUAL_STRING(files[i], rep.file[i]);
        TEST_ASSERT_EQUAL_MESSAGE(expect[i], rep.status[i], files[i]);
    }
}

static void test_a_directory_that_does_not_exist_is_reported(void)
{
    reports_t rep;

    memset(&rep, 0, sizeof rep);
    TEST_ASSERT_EQUAL_UINT(0, hubus_module_set_load_dir(&set, "/nonexistent/hubus", collect, &rep));
    TEST_ASSERT_EQUAL_UINT(1, rep.n);
    TEST_ASSERT_EQUAL(HUBUS_LOAD_OPEN, rep.status[0]);
}

/* ---- the set ------------------------------------------------------------------------------------------------------ */

static void test_the_set_holds_a_limited_number_of_modules(void)
{
    static hubus_module_t modules[HUBUS_MODULE_SET_MAX + 1u];
    static hubus_module_rule_t rules[HUBUS_MODULE_SET_MAX + 1u];
    static char names[HUBUS_MODULE_SET_MAX + 1u][16];
    unsigned i;

    for (i = 0; i <= HUBUS_MODULE_SET_MAX; i++) {
        snprintf(names[i], sizeof names[i], "m%u", i);
        rules[i] = (hubus_module_rule_t){.pattern = {(uint8_t)(0x80u + i)}, .mask = {0xFF}, .len = 1};
        modules[i] = (hubus_module_t){HUBUS_MODULE_INIT, .name = names[i], .rules = &rules[i], .n_rules = 1, .max_command = 1};
    }
    for (i = 0; i < HUBUS_MODULE_SET_MAX; i++) {
        TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, hubus_module_set_add(&set, &modules[i]).status);
    }
    TEST_ASSERT_EQUAL(HUBUS_LOAD_FULL, hubus_module_set_add(&set, &modules[HUBUS_MODULE_SET_MAX]).status);
}

static void test_statically_linked_modules_join_the_set(void)
{
    hubus_module_rule_t rule = {.pattern = {0x95}, .mask = {0xFF}, .len = 1};
    hubus_module_t m = {HUBUS_MODULE_INIT, .name = "static", .rules = &rule, .n_rules = 1, .max_command = 1};
    hubus_module_t invalid = m;

    TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, hubus_module_set_add(&set, &m).status);
    TEST_ASSERT_NULL(set.plugin[0]);
    invalid.name = "other";
    invalid.max_command = 0;
    TEST_ASSERT_EQUAL(HUBUS_LOAD_INVALID, hubus_module_set_add(&set, &invalid).status);
    TEST_ASSERT_EQUAL(HUBUS_LOAD_DUPLICATE, hubus_module_set_add(&set, &m).status);
    TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, load("a_good").status); /* statically linked and plugins in one set */
    TEST_ASSERT_EQUAL_UINT(2, set.n);
}

/* ---- conflicts ---------------------------------------------------------------------------------------------------- */

static hubus_module_t with_rule(const hubus_module_rule_t *r)
{
    return (hubus_module_t){HUBUS_MODULE_INIT, .name = "x", .rules = r, .n_rules = 1, .max_command = 1};
}

static void test_conflicts_need_the_same_number_of_mask_bits_and_a_common_device(void)
{
    const hubus_module_rule_t variant = {.pattern = {0x81, 0x02}, .mask = {0xFF, 0xFF}, .len = 2};
    const hubus_module_rule_t other_variant = {.pattern = {0x81, 0x03}, .mask = {0xFF, 0xFF}, .len = 2};
    const hubus_module_rule_t version = {.pattern = {0x81, 0x02, 0x07}, .mask = {0xFF, 0xFF, 0xFF}, .len = 3};
    const hubus_module_rule_t version_7 = {.pattern = {0x81, 0x02, 0x07, 0x00}, .mask = {0xFF, 0xFF, 0xFF, 0x00}, .len = 4};
    const hubus_module_rule_t revision = {.pattern = {0x81, 0x02, 0x00, 0x01}, .mask = {0xFF, 0xFF, 0x00, 0xFF}, .len = 4};
    const hubus_module_rule_t other_type = {.pattern = {0x82, 0x02}, .mask = {0xFF, 0xFF}, .len = 2};
    hubus_module_t a = with_rule(&variant);
    hubus_module_t same = with_rule(&variant);
    hubus_module_t other = with_rule(&other_variant);
    hubus_module_t specific = with_rule(&version);
    hubus_module_t v7 = with_rule(&version_7);
    hubus_module_t rev = with_rule(&revision);
    hubus_module_t type = with_rule(&other_type);

    TEST_ASSERT_TRUE(hubus_module_conflicts(&a, &same));
    TEST_ASSERT_FALSE(hubus_module_conflicts(&a, &other));    /* no common device */
    TEST_ASSERT_FALSE(hubus_module_conflicts(&a, &type));     /* another type */
    TEST_ASSERT_FALSE(hubus_module_conflicts(&a, &specific)); /* the more specific rule wins */
    TEST_ASSERT_TRUE(hubus_module_conflicts(&v7, &rev));      /* 24 bits each; version 7 on revision 1 matches both */
    TEST_ASSERT_TRUE(hubus_module_conflicts(&rev, &v7));
}

/* A new file with the content of the plugin under this name, as `install` and `mv` make it. */
static void install_plugin(const char *from, const char *to)
{
    static char buf[1 << 20];
    char tmp[300];
    size_t n;
    FILE *in = fopen(from, "rb");
    FILE *out;

    TEST_ASSERT_NOT_NULL(in);
    snprintf(tmp, sizeof tmp, "%s.new", to);
    out = fopen(tmp, "wb");
    TEST_ASSERT_NOT_NULL(out);
    n = fread(buf, 1, sizeof buf, in);
    TEST_ASSERT_EQUAL_size_t(n, fwrite(buf, 1, n, out));
    fclose(in);
    fclose(out);
    TEST_ASSERT_EQUAL_INT(0, chmod(tmp, 0755));
    TEST_ASSERT_EQUAL_INT(0, rename(tmp, to));
}

static void test_the_same_file_gives_the_same_module_and_a_new_file_a_new_one(void)
{
    char dir[] = "/tmp/hubus_plugin_XXXXXX";
    char path[200];
    hubus_module_set_t second, third;
    const hubus_module_t *first;

    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/good.so", dir);
    install_plugin(PLUGIN("a_good"), path);
    TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, hubus_module_set_load_file(&set, path).status);
    first = set.module[0];

    hubus_module_set_init(&second);
    second.trust_all = true;
    TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, hubus_module_set_load_file(&second, path).status);
    TEST_ASSERT_EQUAL_PTR(first, second.module[0]);

    install_plugin(PLUGIN("a_good"), path);
    hubus_module_set_init(&third);
    third.trust_all = true;
    TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, hubus_module_set_load_file(&third, path).status);
    TEST_ASSERT_TRUE(third.module[0] != first);

    /* each stays usable while its set is loaded, and closing one set leaves the other alone */
    hubus_module_set_unload(&second);
    TEST_ASSERT_EQUAL_STRING("plugin_good", first->name);
    hubus_module_set_unload(&set);
    TEST_ASSERT_EQUAL_STRING("plugin_good", third.module[0]->name);
    hubus_module_set_unload(&third);
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    TEST_ASSERT_EQUAL_INT(0, rmdir(dir));
}

static void test_a_plugin_that_is_kept_stays_loaded(void)
{
    const hubus_module_t *m;

    TEST_ASSERT_EQUAL(HUBUS_LOAD_OK, load("a_good").status);
    m = set.module[0];
    hubus_module_set_keep(&set, 0);
    hubus_module_set_unload(&set);
    TEST_ASSERT_EQUAL_STRING("plugin_good", m->name);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_only_files_of_root_or_the_user_that_others_cannot_write_are_safe);
    RUN_TEST(test_a_file_or_directory_that_others_can_write_is_not_loaded);
    RUN_TEST(test_a_plugin_is_loaded_and_selected_by_its_rules);
    RUN_TEST(test_the_more_specific_rule_of_another_plugin_wins);
    RUN_TEST(test_a_plugin_that_cannot_be_used_is_refused_and_closed);
    RUN_TEST(test_a_name_in_use_and_equally_specific_rules_are_refused);
    RUN_TEST(test_the_same_file_twice_is_a_duplicate);
    RUN_TEST(test_a_directory_is_read_in_the_order_of_the_names);
    RUN_TEST(test_a_directory_that_does_not_exist_is_reported);
    RUN_TEST(test_the_set_holds_a_limited_number_of_modules);
    RUN_TEST(test_the_same_file_gives_the_same_module_and_a_new_file_a_new_one);
    RUN_TEST(test_a_plugin_that_is_kept_stays_loaded);
    RUN_TEST(test_statically_linked_modules_join_the_set);
    RUN_TEST(test_conflicts_need_the_same_number_of_mask_bits_and_a_common_device);
    return UNITY_END();
}
