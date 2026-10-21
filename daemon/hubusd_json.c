#include "hubusd_json.h"

#include <stdio.h>
#include <string.h>

void jw_init(jw_t *w, char *buf, size_t cap)
{
    memset(w, 0, sizeof *w);
    w->buf = buf;
    w->cap = cap;
    if (cap) {
        buf[0] = '\0';
    }
}

int jw_ok(const jw_t *w)
{
    return !w->overflow && w->depth == 0;
}

static void put(jw_t *w, const char *s, size_t n)
{
    if (w->overflow) {
        return;
    }
    if (w->len + n + 1u > w->cap) {
        w->overflow = 1;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void put_str(jw_t *w, const char *s)
{
    put(w, s, strlen(s));
}

static void put_escaped(jw_t *w, const char *s)
{
    put(w, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char tmp[8];

        switch (c) {
        case '"': put(w, "\\\"", 2); break;
        case '\\': put(w, "\\\\", 2); break;
        case '\n': put(w, "\\n", 2); break;
        case '\r': put(w, "\\r", 2); break;
        case '\t': put(w, "\\t", 2); break;
        default:
            if (c < 0x20) {
                snprintf(tmp, sizeof tmp, "\\u%04x", c);
                put_str(w, tmp);
            } else {
                put(w, (const char *)&c, 1);
            }
        }
    }
    put(w, "\"", 1);
}

/* Separator and key before a value. */
static void item(jw_t *w, const char *key)
{
    if (w->has_item & (1u << w->depth)) {
        put(w, ",", 1);
    }
    w->has_item |= 1u << w->depth;
    if (key) {
        put_escaped(w, key);
        put(w, ":", 1);
    }
}

static void jw_open(jw_t *w, const char *key, char c)
{
    item(w, key);
    put(w, &c, 1);
    if (w->depth >= 30) {
        w->overflow = 1;
        return;
    }
    w->depth++;
    w->has_item &= ~(1u << w->depth);
}

static void jw_close(jw_t *w, char c)
{
    if (w->depth > 0) {
        w->depth--;
    }
    put(w, &c, 1);
}

void jw_obj_begin(jw_t *w, const char *key) { jw_open(w, key, '{'); }
void jw_obj_end(jw_t *w) { jw_close(w, '}'); }
void jw_arr_begin(jw_t *w, const char *key) { jw_open(w, key, '['); }
void jw_arr_end(jw_t *w) { jw_close(w, ']'); }

void jw_str(jw_t *w, const char *key, const char *value)
{
    item(w, key);
    put_escaped(w, value);
}

void jw_int(jw_t *w, const char *key, int64_t value)
{
    char tmp[32];

    item(w, key);
    snprintf(tmp, sizeof tmp, "%lld", (long long)value);
    put_str(w, tmp);
}

void jw_uint(jw_t *w, const char *key, uint64_t value)
{
    char tmp[32];

    item(w, key);
    snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)value);
    put_str(w, tmp);
}

void jw_bool(jw_t *w, const char *key, int value)
{
    item(w, key);
    put_str(w, value ? "true" : "false");
}

void jw_double(jw_t *w, const char *key, double value, int decimals)
{
    char tmp[48];

    item(w, key);
    snprintf(tmp, sizeof tmp, "%.*f", decimals, value);
    put_str(w, tmp);
}

void jw_null(jw_t *w, const char *key)
{
    item(w, key);
    put_str(w, "null");
}
