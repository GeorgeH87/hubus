/*
 * Tiny JSON writer into a caller-provided buffer. Overflow is sticky: after
 * the first overflow every call is ignored and jw_ok() returns 0.
 */
#ifndef HUBUSD_JSON_H
#define HUBUSD_JSON_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int overflow;
    int depth;
    uint32_t has_item; /* bit per depth: a comma is needed before the next item */
} jw_t;

void jw_init(jw_t *w, char *buf, size_t cap);
int jw_ok(const jw_t *w);

/* `key` is NULL inside arrays and for the top-level value. */
void jw_obj_begin(jw_t *w, const char *key);
void jw_obj_end(jw_t *w);
void jw_arr_begin(jw_t *w, const char *key);
void jw_arr_end(jw_t *w);
void jw_str(jw_t *w, const char *key, const char *value);
void jw_int(jw_t *w, const char *key, int64_t value);
void jw_uint(jw_t *w, const char *key, uint64_t value);
void jw_bool(jw_t *w, const char *key, int value);
void jw_double(jw_t *w, const char *key, double value, int decimals);
void jw_null(jw_t *w, const char *key);

#endif /* HUBUSD_JSON_H */
