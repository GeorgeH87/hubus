/*
 * The topics below a slave that a master module may use (hubus_module.h). Its own library, so that a plugin that links
 * the helpers of hubus_module_ha.h gets them without the master library.
 */
#include "hubus_module.h"

#include <string.h>

static const char *const reserved_levels[] = {"availability", "capture", "cmd",  "config", "data",  "firmware",
                                              "info",         "mcid",    "read", "stats",  "update"};

bool hubus_module_topic_reserved(const char *topic)
{
    size_t n = strcspn(topic, "/");
    size_t i;

    for (i = 0; i < sizeof reserved_levels / sizeof reserved_levels[0]; i++) {
        if (strlen(reserved_levels[i]) == n && strncmp(topic, reserved_levels[i], n) == 0) {
            return true;
        }
    }
    return false;
}

bool hubus_module_topic_valid(const char *topic, bool filter)
{
    size_t i, level_start = 0;
    bool wildcard = false; /* in the current level */

    if (topic == NULL || topic[0] == '\0' || strlen(topic) > HUBUS_MODULE_TOPIC_MAX || hubus_module_topic_reserved(topic)) {
        return false;
    }
    for (i = 0;; i++) {
        char c = topic[i];

        if (c == '/' || c == '\0') {
            size_t n = i - level_start;

            if (n == 0u) {
                return false; /* an empty level */
            }
            /* a wildcard is a whole level, never the first, and '#' only the last */
            if (wildcard && (!filter || n != 1u || level_start == 0u || (topic[level_start] == '#' && c != '\0'))) {
                return false;
            }
            if (c == '\0') {
                return true;
            }
            level_start = i + 1u;
            wildcard = false;
        } else if (c == '+' || c == '#') {
            wildcard = true;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                     c == '-')) {
            return false;
        }
    }
}

bool hubus_module_topic_matches(const char *filter, const char *topic)
{
    for (;;) {
        size_t fn = strcspn(filter, "/");
        size_t tn = strcspn(topic, "/");

        if (fn == 1u && filter[0] == '#') {
            return true;
        }
        if (!(fn == 1u && filter[0] == '+') && (fn != tn || strncmp(filter, topic, fn) != 0)) {
            return false;
        }
        filter += fn;
        topic += tn;
        if (*filter == '\0' || *topic == '\0') {
            return *topic == '\0' && (*filter == '\0' || strcmp(filter, "/#") == 0); /* "a/#" takes "a" as well */
        }
        filter++;
        topic++;
    }
}

bool hubus_module_setting_valid(const char *key, const char *value)
{
    size_t i, n;

    if (key == NULL || (n = strlen(key)) == 0u || n > HUBUS_MODULE_SETTING_KEY_MAX || strcmp(key, "module") == 0) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = key[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    if (value == NULL) {
        return true;
    }
    for (i = 0; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];

        if (i >= HUBUS_MODULE_SETTING_MAX || c < 0x20u || c == 0x7Fu) {
            return false; /* bytes above 0x7F are let through: UTF-8 */
        }
    }
    return true;
}
