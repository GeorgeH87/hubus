/*
 * The module of the LED strip (examples/slave-ledstrip, docs/example-ledstrip.md): a lamp in one colour, animations from
 * formulas, and the push button of the device. The firmware shows bytes or runs bytecode; everything else is here.
 */
#include <stdlib.h>
#include <string.h>

#include "hubus_crc.h"
#include "hubusd_ledc.h"
#include "hubusd_modules.h"
#include "ledvm.h"

#define OP_FILL 0x41u
#define OP_SHOW 0x42u
#define OP_PROG_WRITE 0x43u
#define OP_PROG_RUN 0x44u

#define PROG_REFUSED 2u
#define PROG_WRITE_CHUNK 29u
#define ANIMATION_PERIOD_MS 20u
#define NO_ANIMATION "none"

/* State and events of the slave: button, seq of the last SHOW, program. */
#define STATE_SEQ 1u
#define STATE_PROGRAM 2u
#define STATE_LEN 3u

struct hubus_module_instance {
    bool on;
    uint8_t r, g, b;
    uint8_t seq;   /* of the last SHOW; 0: none sent yet */
    uint8_t len;   /* of the program; 0: the lamp is what shows */
    uint8_t code[LEDVM_CODE_MAX];
};

static hubus_module_instance_t *strip_create(const hubus_module_host_t *host)
{
    hubus_module_instance_t *self = calloc(1, sizeof *self);

    (void)host;
    if (self) {
        self->r = self->g = self->b = 255; /* white until the user has chosen a colour */
    }
    return self;
}

static void strip_destroy(hubus_module_instance_t *self)
{
    free(self);
}

/* One colour on the whole strip. A count beyond the buffer of the slave is cut off there, so the module need not know its
 * length; the byte order is that of the WS2812B. A SHOW also stops a program. */
static hubus_module_status_t send_colour(hubus_module_instance_t *self, const hubus_module_host_t *host)
{
    uint8_t fill[7] = {0, 0, 0xFF, 0xFF, 0, 0, 0};
    uint8_t seq = self->seq == 255u ? 1u : (uint8_t)(self->seq + 1u);
    hubus_module_status_t st;

    if (self->on) {
        fill[4] = self->g;
        fill[5] = self->r;
        fill[6] = self->b;
    }
    st = host->send(host, OP_FILL, fill, sizeof fill);
    if (st == HUBUS_MODULE_OK) {
        st = host->send(host, OP_SHOW, &seq, 1);
    }
    if (st == HUBUS_MODULE_OK) {
        self->seq = seq;
    }
    return st;
}

/* Sends the program in pieces. A PROG_WRITE stops what runs, so a program never runs half written; a lost piece shows as a
 * CRC mismatch in PROG_RUN. */
static hubus_module_status_t send_program(hubus_module_instance_t *self, const hubus_module_host_t *host)
{
    uint8_t piece[1u + PROG_WRITE_CHUNK];
    uint8_t run[4] = {self->len, hubus_crc8(self->code, self->len), LEDVM_LAYOUT_GRB, ANIMATION_PERIOD_MS};
    uint8_t off;
    hubus_module_status_t st = HUBUS_MODULE_OK;

    for (off = 0; off < self->len && st == HUBUS_MODULE_OK; off = (uint8_t)(off + PROG_WRITE_CHUNK)) {
        uint8_t n = (uint8_t)(self->len - off) < PROG_WRITE_CHUNK ? (uint8_t)(self->len - off) : (uint8_t)PROG_WRITE_CHUNK;

        piece[0] = off;
        memcpy(&piece[1], &self->code[off], n);
        st = host->send(host, OP_PROG_WRITE, piece, (uint8_t)(1u + n));
    }
    return st == HUBUS_MODULE_OK ? host->send(host, OP_PROG_RUN, run, sizeof run) : st;
}

static hubus_module_status_t set_lamp(hubus_module_instance_t *self, const hubus_module_host_t *host, const hubus_value_t *value)
{
    const hubus_module_instance_t before = *self;
    bool on = self->on;
    uint8_t r = self->r, g = self->g, b = self->b;
    hubus_module_status_t st;

    if (value->type == HUBUS_VALUE_RGB) {
        on = true;
        r = value->as.rgb.r;
        g = value->as.rgb.g;
        b = value->as.rgb.b;
    } else if (value->type == HUBUS_VALUE_BOOL) {
        on = value->as.on;
    } else {
        return HUBUS_MODULE_REJECTED;
    }
    /* Home Assistant sends the colour and then ON: what the strip already shows costs no second frame. */
    if (on != self->on || r != self->r || g != self->g || b != self->b || self->len != 0u || self->seq == 0u) {
        self->on = on;
        self->r = r;
        self->g = g;
        self->b = b;
        st = send_colour(self, host);
        if (st != HUBUS_MODULE_OK) {
            *self = before; /* so that the same value can try again */
            return st;
        }
        if (self->len != 0u) {
            self->len = 0;
            (void)host->set_state(host, "animation", NO_ANIMATION);
        }
    }
    return host->set_state(host, "lamp", on ? "ON" : "OFF");
}

static hubus_module_status_t set_animation(hubus_module_instance_t *self, const hubus_module_host_t *host, const char *formula)
{
    uint8_t code[LEDVM_CODE_MAX];
    uint8_t saved_len = self->len;
    char err[96];
    hubus_module_status_t st;

    if (formula[0] == '\0' || strcmp(formula, NO_ANIMATION) == 0) {
        self->len = 0;
        st = send_colour(self, host); /* back to the lamp */
        if (st != HUBUS_MODULE_OK) {
            self->len = saved_len;
            return st;
        }
        (void)host->set_state(host, "lamp", self->on ? "ON" : "OFF");
        return host->set_state(host, "animation", NO_ANIMATION);
    }
    self->len = hubusd_ledc_compile(formula, code, err, sizeof err);
    if (self->len == 0u) {
        self->len = saved_len;
        (void)host->log(host, err);
        return HUBUS_MODULE_REJECTED;
    }
    memcpy(self->code, code, self->len);
    st = send_program(self, host);
    if (st != HUBUS_MODULE_OK) {
        return st;
    }
    (void)host->set_state(host, "lamp", "ON");
    return host->set_state(host, "animation", formula);
}

static hubus_module_status_t strip_on_set(hubus_module_instance_t *self, const hubus_module_host_t *host,
                                          const hubus_entity_t *entity, const hubus_value_t *value)
{
    if (strcmp(entity->key, "lamp") == 0) {
        return set_lamp(self, host, value);
    }
    if (strcmp(entity->key, "animation") == 0 && value->type == HUBUS_VALUE_TEXT) {
        return set_animation(self, host, value->as.text);
    }
    return HUBUS_MODULE_REJECTED;
}

/*
 * After a power-up the slave reports an event with no program and no SHOW: the strip is dark, and the master sends again what
 * it should show. Only an event says so: a state that is read while the commands of a new program are on their way shows no
 * program either.
 */
static void strip_on_frame(hubus_module_instance_t *self, const hubus_module_host_t *host, hubus_frame_kind_t kind,
                           const uint8_t *data, uint8_t len, bool gap)
{
    (void)gap;
    if (len < STATE_LEN) {
        return;
    }
    if (self->len != 0u && data[STATE_PROGRAM] == PROG_REFUSED) {
        self->len = 0;
        (void)host->set_state(host, "animation", "refused by the slave");
        return;
    }
    if (kind != HUBUS_FRAME_EVENT) {
        return;
    }
    if (self->len != 0u && data[STATE_PROGRAM] == 0u) {
        (void)send_program(self, host);
    } else if (self->len == 0u && self->seq != 0u && data[STATE_SEQ] == 0u) {
        (void)send_colour(self, host);
    }
}

/* Only SHOW makes the strip blind: 180 bytes of 12.5 us, the reset time of the strip, and the wait for the slave to be free
 * (docs/example-ledstrip.md). */
static const hubus_module_rule_t strip_rules[] = {{.pattern = {0x84, 0x01}, .mask = {0xFF, 0xFF}, .len = 2}};
static const hubus_module_rule_t strip_update_rules[] = {{.pattern = {0x84, 0x02}, .mask = {0xFF, 0xFF}, .len = 2}};
static const hubus_module_blind_t strip_blind[] = {{.opcode = OP_SHOW, .ms = 4}};
static const hubus_entity_t strip_entities[] = {
    {.key = "button", .name = "Button", .component = HUBUS_COMPONENT_BINARY_SENSOR, .decode = {.offset = 0, .len = 1, .scale = 1.0}},
    {.key = "lamp", .name = "Lamp", .component = HUBUS_COMPONENT_LIGHT},
    {.key = "animation", .name = "Animation", .component = HUBUS_COMPONENT_TEXT},
};

/* The same firmware with and without update support: only the variant and has_update differ. */
#define STRIP_MODULE                                                                                                   \
    HUBUS_MODULE_INIT, .max_command = HUBUS_MAX_PAYLOAD, .has_state = true, .has_events = true,                        \
        .has_status_capture = true, .blind_ops = strip_blind, .n_blind_ops = HUBUS_MODULE_LEN(strip_blind),            \
        .entities = strip_entities, .n_entities = HUBUS_MODULE_LEN(strip_entities), .create = strip_create,            \
        .destroy = strip_destroy, .on_frame = strip_on_frame, .on_set = strip_on_set

const hubus_module_t hubusd_ledstrip_module = {
    STRIP_MODULE,
    .name = "ledstrip",
    .rules = strip_rules,
    .n_rules = HUBUS_MODULE_LEN(strip_rules),
};

const hubus_module_t hubusd_ledstrip_update_module = {
    STRIP_MODULE,
    .name = "ledstrip-update",
    .rules = strip_update_rules,
    .n_rules = HUBUS_MODULE_LEN(strip_update_rules),
    .has_update = true,
};
