#include "hubusd_yaml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

static void set_err(char *err, size_t errsz, int line, const char *msg)
{
    if (err && errsz) {
        snprintf(err, errsz, "line %d: %s", line, msg);
    }
}

static yn_t *new_node(yn_type_t type, int line)
{
    yn_t *n = calloc(1, sizeof *n);

    if (n) {
        n->type = type;
        n->line = line;
    }
    return n;
}

void yn_free(yn_t *n)
{
    unsigned i;

    if (!n) {
        return;
    }
    for (i = 0; i < n->n; i++) {
        yn_free(n->item[i]);
        if (n->key) {
            free(n->key[i]);
        }
    }
    free(n->item);
    free(n->key);
    free(n->key_line);
    free(n->used);
    free(n->text);
    free(n);
}

static int grow(yn_t *n)
{
    yn_t **item = realloc(n->item, (n->n + 1u) * sizeof *item);

    if (!item) {
        return 0;
    }
    n->item = item;
    if (n->type == YN_MAP) {
        char **key = realloc(n->key, (n->n + 1u) * sizeof *key);
        int *kl;
        unsigned char *used;

        if (!key) {
            return 0;
        }
        n->key = key;
        kl = realloc(n->key_line, (n->n + 1u) * sizeof *kl);
        if (!kl) {
            return 0;
        }
        n->key_line = kl;
        used = realloc(n->used, (n->n + 1u) * sizeof *used);
        if (!used) {
            return 0;
        }
        n->used = used;
    }
    return 1;
}

/* Builds the node that starts with `ev` (already read, deleted here). */
static yn_t *build(yaml_parser_t *p, yaml_event_t *ev, char *err, size_t errsz, int depth)
{
    int line = (int)ev->start_mark.line + 1;
    yn_t *n = NULL;

    if (depth > 32) {
        set_err(err, errsz, line, "nesting too deep");
        yaml_event_delete(ev);
        return NULL;
    }
    switch (ev->type) {
    case YAML_SCALAR_EVENT:
        n = new_node(YN_SCALAR, line);
        if (n) {
            n->text = malloc(ev->data.scalar.length + 1u);
            if (!n->text) {
                yn_free(n);
                n = NULL;
            } else {
                memcpy(n->text, ev->data.scalar.value, ev->data.scalar.length);
                n->text[ev->data.scalar.length] = '\0';
            }
        }
        yaml_event_delete(ev);
        break;
    case YAML_SEQUENCE_START_EVENT:
    case YAML_MAPPING_START_EVENT: {
        int is_map = ev->type == YAML_MAPPING_START_EVENT;

        n = new_node(is_map ? YN_MAP : YN_SEQ, line);
        yaml_event_delete(ev);
        if (!n) {
            break;
        }
        for (;;) {
            yaml_event_t e;
            char *key = NULL;
            int key_line = 0;
            yn_t *child;

            if (!yaml_parser_parse(p, &e)) {
                set_err(err, errsz, (int)p->problem_mark.line + 1, p->problem ? p->problem : "syntax error");
                yn_free(n);
                return NULL;
            }
            if (e.type == (is_map ? YAML_MAPPING_END_EVENT : YAML_SEQUENCE_END_EVENT)) {
                yaml_event_delete(&e);
                return n;
            }
            if (is_map) {
                if (e.type != YAML_SCALAR_EVENT) {
                    set_err(err, errsz, (int)e.start_mark.line + 1, "mapping keys must be plain scalars");
                    yaml_event_delete(&e);
                    yn_free(n);
                    return NULL;
                }
                key = malloc(e.data.scalar.length + 1u);
                if (!key) {
                    yaml_event_delete(&e);
                    yn_free(n);
                    return NULL;
                }
                memcpy(key, e.data.scalar.value, e.data.scalar.length);
                key[e.data.scalar.length] = '\0';
                key_line = (int)e.start_mark.line + 1;
                yaml_event_delete(&e);
                if (!yaml_parser_parse(p, &e)) {
                    set_err(err, errsz, (int)p->problem_mark.line + 1, p->problem ? p->problem : "syntax error");
                    free(key);
                    yn_free(n);
                    return NULL;
                }
            }
            child = build(p, &e, err, errsz, depth + 1);
            if (!child || !grow(n)) {
                free(key);
                yn_free(child);
                yn_free(n);
                if (child && err && !err[0]) {
                    set_err(err, errsz, line, "out of memory");
                }
                return NULL;
            }
            n->item[n->n] = child;
            if (is_map) {
                n->key[n->n] = key;
                n->key_line[n->n] = key_line;
                n->used[n->n] = 0;
            }
            n->n++;
        }
    }
    case YAML_ALIAS_EVENT:
        set_err(err, errsz, line, "aliases are not supported");
        yaml_event_delete(ev);
        break;
    default:
        set_err(err, errsz, line, "unexpected YAML structure");
        yaml_event_delete(ev);
        break;
    }
    return n;
}

yn_t *yn_parse(const char *text, size_t len, char *err, size_t errsz)
{
    yaml_parser_t p;
    yaml_event_t ev;
    yn_t *root = NULL;

    if (err && errsz) {
        err[0] = '\0';
    }
    if (!yaml_parser_initialize(&p)) {
        set_err(err, errsz, 0, "cannot initialise the YAML parser");
        return NULL;
    }
    yaml_parser_set_input_string(&p, (const unsigned char *)text, len);

    /* STREAM-START, DOCUMENT-START */
    for (int i = 0; i < 2; i++) {
        if (!yaml_parser_parse(&p, &ev)) {
            set_err(err, errsz, (int)p.problem_mark.line + 1, p.problem ? p.problem : "syntax error");
            yaml_parser_delete(&p);
            return NULL;
        }
        if (i == 1 && ev.type == YAML_STREAM_END_EVENT) { /* empty file */
            yaml_event_delete(&ev);
            yaml_parser_delete(&p);
            return new_node(YN_MAP, 1);
        }
        yaml_event_delete(&ev);
    }
    if (!yaml_parser_parse(&p, &ev)) {
        set_err(err, errsz, (int)p.problem_mark.line + 1, p.problem ? p.problem : "syntax error");
        yaml_parser_delete(&p);
        return NULL;
    }
    root = build(&p, &ev, err, errsz, 0);
    if (root) {
        /* DOCUMENT-END, then STREAM-END; a second document is an error */
        int ok = yaml_parser_parse(&p, &ev);

        if (ok) {
            yaml_event_delete(&ev);
            ok = yaml_parser_parse(&p, &ev);
            if (ok && ev.type != YAML_STREAM_END_EVENT) {
                set_err(err, errsz, (int)ev.start_mark.line + 1, "only one YAML document is allowed");
                yn_free(root);
                root = NULL;
            }
            if (ok) {
                yaml_event_delete(&ev);
            }
        }
        if (!ok && root) {
            set_err(err, errsz, (int)p.problem_mark.line + 1, p.problem ? p.problem : "syntax error");
            yn_free(root);
            root = NULL;
        }
    }
    yaml_parser_delete(&p);
    return root;
}

yn_t *yn_get(yn_t *map, const char *key)
{
    unsigned i;

    if (!map || map->type != YN_MAP) {
        return NULL;
    }
    for (i = 0; i < map->n; i++) {
        if (strcmp(map->key[i], key) == 0) {
            map->used[i] = 1;
            return map->item[i];
        }
    }
    return NULL;
}
