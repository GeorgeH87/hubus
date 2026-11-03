/*
 * Master modules: validation and selection by the hardware identifier.
 */
#include "hubus_module.h"

#include <stdio.h>
#include <string.h>

#define MODULE_MAX_COMMAND 31u
#define MODULE_MAX_PAYLOAD 31u

hubus_hwid_form_t hubus_hwid_form(const uint8_t *hwid, uint8_t len)
{
    if (len == 0u || len > HUBUS_HWID_MAX_LEN || hwid[0] == 0x00u) {
        return HUBUS_HWID_INVALID;
    }
    if (hwid[0] == HUBUS_HWID_TYPE_UPDATER) {
        return len == HUBUS_HWID_UPDATER_LEN ? HUBUS_HWID_UPDATER : HUBUS_HWID_INVALID;
    }
    if (hwid[0] < HUBUS_HWID_TYPE_FULL_MIN) {
        return len == 1u ? HUBUS_HWID_SHORT : HUBUS_HWID_INVALID;
    }
    return len >= HUBUS_HWID_FULL_MIN_LEN ? HUBUS_HWID_FULL : HUBUS_HWID_INVALID;
}

static bool rule_valid(const hubus_module_rule_t *r)
{
    uint8_t type = r->pattern[0];
    uint8_t i;

    if (r->len == 0u || r->len > HUBUS_RULE_MAX_LEN || type == 0x00u || r->mask[0] != 0xFFu) {
        return false; /* a rule always names a whole type */
    }
    if ((type < HUBUS_HWID_TYPE_FULL_MIN || type == HUBUS_HWID_TYPE_UPDATER) && r->len != 1u) {
        return false; /* the short form and the updater have no fields a rule could cover */
    }
    for (i = 0; i < r->len; i++) {
        if ((r->pattern[i] & (uint8_t)~r->mask[i]) != 0u) {
            return false;
        }
    }
    return true;
}

static bool blind_ops_valid(const hubus_module_t *m)
{
    uint8_t i, j;

    if (m->n_blind_ops > 0u && m->blind_ops == NULL) {
        return false;
    }
    for (i = 0; i < m->n_blind_ops; i++) {
        for (j = 0; j < i; j++) {
            if (m->blind_ops[i].opcode == m->blind_ops[j].opcode) {
                return false;
            }
        }
    }
    return true;
}

#define MODULE_MAX_DECIMALS 6u

static bool key_valid(const char *key)
{
    size_t i;

    if (key == NULL || key[0] == '\0' || strlen(key) > HUBUS_ENTITY_KEY_MAX) {
        return false;
    }
    for (i = 0; key[i] != '\0'; i++) {
        char c = key[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool decode_valid(const hubus_module_decode_t *d)
{
    if (d->len > 2u || (unsigned)d->offset + d->len > MODULE_MAX_PAYLOAD || d->decimals > MODULE_MAX_DECIMALS) {
        return false;
    }
    return d->len == 0u || d->scale != 0.0;
}

static bool command_valid(const hubus_module_command_t *c, uint8_t max_command)
{
    return !c->used || (c->n_args <= HUBUS_MODULE_CMD_MAX_ARGS && (unsigned)c->n_args + 1u <= max_command);
}

static bool entity_valid(const hubus_module_t *m, const hubus_entity_t *e)
{
    if (!key_valid(e->key) || e->name == NULL || (unsigned)e->component >= HUBUS_COMPONENT_COUNT ||
        (unsigned)e->device_class >= HUBUS_DEVICE_CLASS_COUNT || (unsigned)e->unit >= HUBUS_UNIT_COUNT ||
        !decode_valid(&e->decode) || !command_valid(&e->on, m->max_command) ||
        !command_valid(&e->off, m->max_command)) {
        return false;
    }
    switch (e->component) {
    case HUBUS_COMPONENT_SENSOR:
    case HUBUS_COMPONENT_BINARY_SENSOR:
        return e->decode.len != 0u;
    case HUBUS_COMPONENT_SWITCH:
        return m->on_set != NULL || (e->on.used && e->off.used);
    case HUBUS_COMPONENT_LIGHT:
    case HUBUS_COMPONENT_TEXT:
        return m->on_set != NULL;
    default:
        return false;
    }
}

static bool entities_valid(const hubus_module_t *m)
{
    uint8_t i, j;

    if ((m->n_entities > 0u && m->entities == NULL) || m->n_entities > HUBUS_MODULE_ENTITIES_MAX) {
        return false;
    }
    for (i = 0; i < m->n_entities; i++) {
        if (!entity_valid(m, &m->entities[i])) {
            return false;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(m->entities[i].key, m->entities[j].key) == 0) {
                return false;
            }
        }
    }
    return true;
}

hubus_module_error_t hubus_module_validate(const hubus_module_t *m)
{
    uint8_t i;

    if (m->api_version != HUBUS_MODULE_API_VERSION || m->struct_size < sizeof(hubus_module_t)) {
        return HUBUS_MODULE_ERR_API;
    }
    if (m->name == NULL || m->name[0] == '\0') {
        return HUBUS_MODULE_ERR_NAME;
    }
    if (m->rules == NULL || m->n_rules == 0u) {
        return HUBUS_MODULE_ERR_RULE;
    }
    for (i = 0; i < m->n_rules; i++) {
        if (!rule_valid(&m->rules[i])) {
            return HUBUS_MODULE_ERR_RULE;
        }
    }
    if (m->max_command == 0u || m->max_command > MODULE_MAX_COMMAND || (m->has_status_capture && !m->has_events)) {
        return HUBUS_MODULE_ERR_PROPERTIES;
    }
    if (!blind_ops_valid(m)) {
        return HUBUS_MODULE_ERR_BLIND;
    }
    if ((m->create == NULL) != (m->destroy == NULL)) {
        return HUBUS_MODULE_ERR_FUNCTIONS;
    }
    if (!entities_valid(m)) {
        return HUBUS_MODULE_ERR_ENTITY;
    }
    if ((m->firmware == NULL) != (m->firmware_len == 0u) || (m->firmware != NULL && !m->has_update)) {
        return HUBUS_MODULE_ERR_FIRMWARE;
    }
    return HUBUS_MODULE_VALID;
}

static bool rule_matches(const hubus_module_rule_t *r, const uint8_t *hwid, uint8_t len)
{
    uint8_t i;

    if (len < r->len) {
        return false;
    }
    for (i = 0; i < r->len; i++) {
        if (((hwid[i] ^ r->pattern[i]) & r->mask[i]) != 0u) {
            return false;
        }
    }
    return true;
}

static unsigned rule_bits(const hubus_module_rule_t *r)
{
    unsigned bits = 0;
    uint8_t i;

    for (i = 0; i < r->len; i++) {
        uint8_t b = r->mask[i];

        while (b != 0u) {
            bits += b & 1u;
            b = (uint8_t)(b >> 1);
        }
    }
    return bits;
}

hubus_module_match_t hubus_module_select(const hubus_module_t *const *modules, size_t n_modules, const uint8_t *hwid,
                                         uint8_t len, const hubus_module_t **out)
{
    const hubus_module_t *best = NULL;
    unsigned best_bits = 0;
    bool conflict = false;
    size_t k;
    uint8_t i;

    *out = NULL;
    if (hubus_hwid_form(hwid, len) == HUBUS_HWID_INVALID) {
        return HUBUS_MODULE_INVALID;
    }
    for (k = 0; k < n_modules; k++) {
        const hubus_module_t *m = modules[k];

        for (i = 0; i < m->n_rules; i++) {
            unsigned bits;

            if (!rule_matches(&m->rules[i], hwid, len)) {
                continue;
            }
            bits = rule_bits(&m->rules[i]);
            if (bits > best_bits) {
                best = m;
                best_bits = bits;
                conflict = false;
            } else if (bits == best_bits && m != best) {
                conflict = true;
            }
        }
    }
    if (best == NULL) {
        return HUBUS_MODULE_UNKNOWN;
    }
    if (conflict) {
        return HUBUS_MODULE_CONFLICT;
    }
    *out = best;
    return HUBUS_MODULE_FOUND;
}

static bool rules_overlap(const hubus_module_rule_t *a, const hubus_module_rule_t *b)
{
    uint8_t n = a->len < b->len ? a->len : b->len;
    uint8_t i;

    for (i = 0; i < n; i++) {
        if (((a->pattern[i] ^ b->pattern[i]) & a->mask[i] & b->mask[i]) != 0u) {
            return false;
        }
    }
    return true;
}

bool hubus_module_conflicts(const hubus_module_t *a, const hubus_module_t *b)
{
    uint8_t i, j;

    for (i = 0; i < a->n_rules; i++) {
        for (j = 0; j < b->n_rules; j++) {
            if (rule_bits(&a->rules[i]) == rule_bits(&b->rules[j]) && rules_overlap(&a->rules[i], &b->rules[j])) {
                return true;
            }
        }
    }
    return false;
}

int hubus_module_find_entity(const hubus_module_t *m, const char *key)
{
    uint8_t i;

    for (i = 0; i < m->n_entities; i++) {
        if (strcmp(m->entities[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

size_t hubus_entity_decode(const hubus_entity_t *e, const uint8_t *data, uint8_t len, char *out, size_t out_size)
{
    const hubus_module_decode_t *d = &e->decode;
    int32_t raw;
    int n;

    if (d->len == 0u || (unsigned)d->offset + d->len > len) {
        return 0;
    }
    if (d->len == 1u) {
        raw = d->is_signed ? (int32_t)(int8_t)data[d->offset] : (int32_t)data[d->offset];
    } else {
        uint16_t hi = d->big_endian ? data[d->offset] : data[d->offset + 1u];
        uint16_t lo = d->big_endian ? data[d->offset + 1u] : data[d->offset];
        uint16_t v = (uint16_t)((hi << 8) | lo);

        raw = d->is_signed ? (int32_t)(int16_t)v : (int32_t)v;
    }
    if (e->component == HUBUS_COMPONENT_SENSOR) {
        n = snprintf(out, out_size, "%.*f", d->decimals, (double)raw * d->scale + d->add);
    } else {
        n = snprintf(out, out_size, "%s", raw != 0 ? "ON" : "OFF");
    }
    return (n > 0 && (size_t)n < out_size) ? (size_t)n : 0u;
}

hubus_module_match_t hubus_module_select_info(const hubus_module_t *const *modules, size_t n_modules,
                                              const uint8_t *payload, uint8_t len, const hubus_module_t **out)
{
    *out = NULL;
    if (len <= HUBUS_INFO_HWID || HUBUS_INFO_VERSION(payload[HUBUS_INFO_BYTE0]) != HUBUS_PROTOCOL_VERSION) {
        return HUBUS_MODULE_INVALID;
    }
    return hubus_module_select(modules, n_modules, &payload[HUBUS_INFO_HWID], (uint8_t)(len - HUBUS_INFO_HWID), out);
}

bool hubus_module_has_blind_time(const hubus_module_t *m)
{
    uint8_t i;

    for (i = 0; i < m->n_blind_ops; i++) {
        if (m->blind_ops[i].ms != 0u) {
            return true;
        }
    }
    return m->blind_default_ms != 0u;
}

uint16_t hubus_module_blind_ms(const hubus_module_t *m, uint8_t opcode)
{
    uint8_t i;

    for (i = 0; i < m->n_blind_ops; i++) {
        if (m->blind_ops[i].opcode == opcode) {
            return m->blind_ops[i].ms;
        }
    }
    return m->blind_default_ms;
}
