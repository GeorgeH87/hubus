#include "ledvm.h"

#ifdef __AVR__
#include <avr/pgmspace.h>
#define LEDVM_READ(p) pgm_read_byte(p)
#else
#define PROGMEM
#define LEDVM_READ(p) (*(p))
#endif

/* Per instruction: bits 2..0 operands taken from the stack, 4..3 values pushed, 6..5 immediate bytes, 7 output.
 * 0: unknown instruction. */
#define OPS(pops, pushes, imm, out) ((uint8_t)((pops) | (pushes) << 3 | (imm) << 5 | (out) << 7))
#define VALUE OPS(0, 1, 0, 0)
#define UNARY OPS(1, 1, 0, 0)
#define BINARY OPS(2, 1, 0, 0)

static const uint8_t info[] PROGMEM = {
    OPS(0, 1, 1, 0), OPS(0, 1, 2, 0), VALUE, VALUE, VALUE, VALUE, OPS(1, 2, 0, 0), OPS(2, 2, 0, 0), /* 0x00 */
    VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE,                                         /* 0x08 P */
    VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE,                                         /* 0x10 PREV LOAD */
    OPS(1, 0, 0, 0), OPS(1, 0, 0, 0), OPS(1, 0, 0, 0), OPS(1, 0, 0, 0), OPS(1, 0, 0, 0), 0, 0, 0,   /* 0x18 STORE DROP */
    BINARY, BINARY, BINARY, BINARY, BINARY, BINARY, BINARY, BINARY,                                 /* 0x20 */
    BINARY, BINARY, BINARY, BINARY, BINARY, BINARY, BINARY, 0,                                      /* 0x28 */
    UNARY, UNARY, UNARY, UNARY, UNARY, 0, 0, 0,                                                     /* 0x30 */
    OPS(3, 1, 0, 0), 0, 0, 0, OPS(3, 0, 0, 1), OPS(4, 0, 0, 1), OPS(3, 0, 0, 1),                    /* 0x38 */
};

#define INFO_POPS(e) ((uint8_t)((e) & 7u))
#define INFO_PUSHES(e) ((uint8_t)(((e) >> 3) & 3u))
#define INFO_IMM(e) ((uint8_t)(((e) >> 5) & 3u))
#define INFO_OUT(e) ((e) & 0x80u)

/* Position of R, G, B, W within one LED, per layout. */
static const uint8_t channel_ofs[LEDVM_LAYOUT_MAX + 1][4] PROGMEM = {
    {1, 0, 2, 3}, /* GRB */
    {1, 0, 2, 3}, /* GRBW */
    {0, 1, 2, 3}, /* RGB */
    {0, 1, 2, 3}, /* RGBW */
};

/* Quarter wave: round(127 * sin(2 pi k / 256)), k = 0..64. */
static const uint8_t sine_quarter[65] PROGMEM = {
    0,   3,   6,   9,   12,  16,  19,  22,  25,  28,  31,  34,  37,  40,  43,  46,  49,  51,  54,  57,  60,  63,
    65,  68,  71,  73,  76,  78,  81,  83,  85,  88,  90,  92,  94,  96,  98,  100, 102, 104, 106, 107, 109, 111,
    112, 113, 115, 116, 117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127, 127,
};

static uint8_t info_of(uint8_t op)
{
    return op < sizeof info ? LEDVM_READ(&info[op]) : 0u;
}

uint8_t ledvm_check(const uint8_t *code, uint8_t len)
{
    uint8_t pc = 0;
    uint8_t depth = 0;

    if (len > LEDVM_CODE_MAX) {
        return 0;
    }
    while (pc < len) {
        uint8_t e = info_of(code[pc]);

        if (!e || depth < INFO_POPS(e)) {
            return 0;
        }
        depth = (uint8_t)(depth - INFO_POPS(e) + INFO_PUSHES(e));
        pc = (uint8_t)(pc + 1u + INFO_IMM(e));
        if (depth > LEDVM_STACK || pc > len) {
            return 0;
        }
        if (INFO_OUT(e)) {
            return pc == len && depth == 0;
        }
    }
    return 0; /* no output instruction */
}

void ledvm_start(ledvm_t *vm, uint8_t len, uint8_t layout)
{
    uint8_t k;

    for (k = 0; k < LEDVM_VARS; k++) {
        vm->v[k] = 0;
    }
    vm->rng = 0xACE1u;
    vm->layout = layout & 3u;
    vm->len = len;
}

uint8_t ledvm_sin8(uint8_t x)
{
    uint8_t q = (uint8_t)(x & 63u);

    switch (x >> 6) {
    case 0:
        return (uint8_t)(128u + LEDVM_READ(&sine_quarter[q]));
    case 1:
        return (uint8_t)(128u + LEDVM_READ(&sine_quarter[64u - q]));
    case 2:
        return (uint8_t)(128u - LEDVM_READ(&sine_quarter[q]));
    default:
        return (uint8_t)(128u - LEDVM_READ(&sine_quarter[64u - q]));
    }
}

/* a * b / 255, close enough: exact for b = 0 and b = 255. */
static uint8_t scale8(uint8_t a, uint8_t b)
{
    return (uint8_t)((unsigned)a * (b + 1u) >> 8);
}

void ledvm_hsv(uint8_t h, uint8_t s, uint8_t v, uint8_t rgb[3])
{
    uint8_t region = (uint8_t)(h / 43u);
    uint8_t rem = (uint8_t)((h - region * 43u) * 6u); /* 0..252 */
    uint8_t p = scale8(v, (uint8_t)(255u - s));
    uint8_t q = scale8(v, (uint8_t)(255u - scale8(s, rem)));
    uint8_t t = scale8(v, (uint8_t)(255u - scale8(s, (uint8_t)(255u - rem))));

    switch (region) {
    case 0:
        rgb[0] = v, rgb[1] = t, rgb[2] = p;
        break;
    case 1:
        rgb[0] = q, rgb[1] = v, rgb[2] = p;
        break;
    case 2:
        rgb[0] = p, rgb[1] = v, rgb[2] = t;
        break;
    case 3:
        rgb[0] = p, rgb[1] = q, rgb[2] = v;
        break;
    case 4:
        rgb[0] = t, rgb[1] = p, rgb[2] = v;
        break;
    default:
        rgb[0] = v, rgb[1] = p, rgb[2] = q;
        break;
    }
}

static uint8_t clamp8(int16_t x)
{
    return x < 0 ? 0u : x > 255 ? 255u : (uint8_t)x;
}

static int16_t wrap(unsigned x)
{
    return (int16_t)(uint16_t)x;
}

/* Writes channel c (0..3 = R G B W); a channel the layout does not have is dropped. */
static void put(const ledvm_t *vm, uint8_t *led, uint8_t c, uint8_t value)
{
    if (c < ledvm_bpp(vm->layout)) {
        led[LEDVM_READ(&channel_ofs[vm->layout][c])] = value;
    }
}

void ledvm_led(ledvm_t *vm, uint8_t *led, uint16_t i, uint16_t n, uint16_t t)
{
    int16_t s[LEDVM_STACK];
    uint8_t sp = 0;
    uint8_t pc = 0;
    uint8_t len = vm->len;
    uint8_t rgb[3];

    if (len > LEDVM_CODE_MAX) {
        return;
    }
    while (pc < len) {
        uint8_t op = vm->code[pc++];
        uint8_t e = info_of(op);
        int16_t *o;

        if (!e || sp < INFO_POPS(e) || sp - INFO_POPS(e) + INFO_PUSHES(e) > LEDVM_STACK || pc + INFO_IMM(e) > len) {
            return;
        }
        sp = (uint8_t)(sp - INFO_POPS(e));
        o = &s[sp]; /* the operands, then the results */
        switch (op) {
        case LEDVM_K8:
            o[0] = vm->code[pc++];
            break;
        case LEDVM_K16:
            o[0] = wrap((unsigned)vm->code[pc] | (unsigned)vm->code[pc + 1u] << 8);
            pc = (uint8_t)(pc + 2u);
            break;
        case LEDVM_I:
            o[0] = wrap(i);
            break;
        case LEDVM_N:
            o[0] = wrap(n);
            break;
        case LEDVM_T:
            o[0] = wrap(t);
            break;
        case LEDVM_RAND:
            vm->rng ^= (uint16_t)(vm->rng << 7);
            vm->rng ^= (uint16_t)(vm->rng >> 9);
            vm->rng ^= (uint16_t)(vm->rng << 8);
            o[0] = (int16_t)(vm->rng & 0xFFu);
            break;
        case LEDVM_DUP:
            o[1] = o[0];
            break;
        case LEDVM_SWAP: {
            int16_t x = o[0];

            o[0] = o[1];
            o[1] = x;
            break;
        }
        case LEDVM_DROP:
            break;
        case LEDVM_ADD:
            o[0] = wrap((unsigned)(uint16_t)o[0] + (uint16_t)o[1]);
            break;
        case LEDVM_SUB:
            o[0] = wrap((unsigned)(uint16_t)o[0] - (uint16_t)o[1]);
            break;
        case LEDVM_MUL:
            o[0] = wrap((unsigned)(uint16_t)o[0] * (uint16_t)o[1]);
            break;
        case LEDVM_DIV: /* a / -1 as a negation: -32768 / -1 does not fit */
            o[0] = o[1] == 0 ? 0 : o[1] == -1 ? wrap(0u - (uint16_t)o[0]) : (int16_t)(o[0] / o[1]);
            break;
        case LEDVM_MOD:
            o[0] = (o[1] == 0 || o[1] == -1) ? 0 : (int16_t)(o[0] % o[1]);
            break;
        case LEDVM_AND:
            o[0] = (int16_t)(o[0] & o[1]);
            break;
        case LEDVM_OR:
            o[0] = (int16_t)(o[0] | o[1]);
            break;
        case LEDVM_XOR:
            o[0] = (int16_t)(o[0] ^ o[1]);
            break;
        case LEDVM_SHL:
            o[0] = wrap((unsigned)(uint16_t)o[0] << (o[1] & 15));
            break;
        case LEDVM_SHR:
            o[0] = wrap((unsigned)(uint16_t)o[0] >> (o[1] & 15));
            break;
        case LEDVM_MIN:
            o[0] = o[0] < o[1] ? o[0] : o[1];
            break;
        case LEDVM_MAX:
            o[0] = o[0] > o[1] ? o[0] : o[1];
            break;
        case LEDVM_LT:
            o[0] = o[0] < o[1];
            break;
        case LEDVM_GT:
            o[0] = o[0] > o[1];
            break;
        case LEDVM_EQ:
            o[0] = o[0] == o[1];
            break;
        case LEDVM_NEG:
            o[0] = wrap(0u - (uint16_t)o[0]);
            break;
        case LEDVM_NOT:
            o[0] = o[0] == 0;
            break;
        case LEDVM_ABS:
            o[0] = o[0] < 0 ? wrap(0u - (uint16_t)o[0]) : o[0];
            break;
        case LEDVM_SIN8:
            o[0] = ledvm_sin8((uint8_t)o[0]);
            break;
        case LEDVM_TRI8: {
            uint8_t x = (uint8_t)o[0];

            o[0] = (int16_t)(x < 128u ? x * 2u : (255u - x) * 2u);
            break;
        }
        case LEDVM_SEL:
            o[0] = o[0] ? o[1] : o[2];
            break;
        case LEDVM_RGB:
        case LEDVM_RGBW:
            put(vm, led, 0, clamp8(o[0]));
            put(vm, led, 1, clamp8(o[1]));
            put(vm, led, 2, clamp8(o[2]));
            put(vm, led, 3, op == LEDVM_RGBW ? clamp8(o[3]) : 0u);
            return;
        case LEDVM_HSV:
            ledvm_hsv(clamp8(o[0]), clamp8(o[1]), clamp8(o[2]), rgb);
            put(vm, led, 0, rgb[0]);
            put(vm, led, 1, rgb[1]);
            put(vm, led, 2, rgb[2]);
            put(vm, led, 3, 0);
            return;
        default:
            if (op >= LEDVM_STORE(0)) { /* 0x18..0x1B */
                vm->v[op - LEDVM_STORE(0)] = o[0];
            } else if (op >= LEDVM_LOAD(0)) { /* 0x14..0x17 */
                o[0] = vm->v[op - LEDVM_LOAD(0)];
            } else if (op >= LEDVM_PREV(0)) { /* 0x10..0x13 */
                uint8_t c = (uint8_t)(op - LEDVM_PREV(0));

                o[0] = c < ledvm_bpp(vm->layout) ? led[LEDVM_READ(&channel_ofs[vm->layout][c])] : 0;
            } else { /* 0x08..0x0F */
                o[0] = vm->p[op - LEDVM_P(0)];
            }
            break;
        }
        sp = (uint8_t)(sp + INFO_PUSHES(e));
    }
}
