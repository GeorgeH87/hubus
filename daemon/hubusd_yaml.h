/*
 * Minimal YAML tree on top of libyaml: scalars, mappings, and sequences with
 * line numbers. Aliases, anchors, and multiple documents are rejected. Used by
 * the configuration loader, which marks every key it reads so that unknown
 * keys can be reported.
 */
#ifndef HUBUSD_YAML_H
#define HUBUSD_YAML_H

#include <stddef.h>

typedef enum { YN_SCALAR, YN_MAP, YN_SEQ } yn_type_t;

typedef struct yn {
    yn_type_t type;
    int line;            /* 1-based */
    char *text;          /* scalars */
    unsigned n;          /* children */
    struct yn **item;    /* children (map values or sequence items) */
    char **key;          /* map keys */
    int *key_line;
    unsigned char *used; /* map entries read through yn_get() */
} yn_t;

/* Returns NULL on error and writes "line N: message" to `err`. */
yn_t *yn_parse(const char *text, size_t len, char *err, size_t errsz);
void yn_free(yn_t *n);

/* Value of `key` in a mapping (marks it as used), or NULL. */
yn_t *yn_get(yn_t *map, const char *key);

#endif /* HUBUSD_YAML_H */
