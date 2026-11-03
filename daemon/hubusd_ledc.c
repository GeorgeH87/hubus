#include "hubusd_ledc.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ledvm.h"

#define NEST_MAX 24u /* of parentheses and operators; the stack of the slave is far smaller */

typedef struct {
    const char *start;
    const char *p;
    uint8_t *code;
    unsigned len;
    unsigned nest;
    char *err;
    size_t err_size;
    int failed;
} ctx_t;

/* One level of binary operators, loosest first. Two-character operators come before the ones they start with. */
typedef struct {
    const char *text;
    uint8_t op;
    uint8_t op2; /* follows `op` when it needs two instructions; 0: none */
} binary_t;

static const binary_t level_or[] = {{"|", LEDVM_OR, 0}, {NULL, 0, 0}};
static const binary_t level_xor[] = {{"^", LEDVM_XOR, 0}, {NULL, 0, 0}};
static const binary_t level_and[] = {{"&", LEDVM_AND, 0}, {NULL, 0, 0}};
static const binary_t level_equal[] = {{"==", LEDVM_EQ, 0}, {"!=", LEDVM_EQ, LEDVM_NOT}, {NULL, 0, 0}};
static const binary_t level_compare[] = {
    {"<=", LEDVM_GT, LEDVM_NOT}, {">=", LEDVM_LT, LEDVM_NOT}, {"<", LEDVM_LT, 0}, {">", LEDVM_GT, 0}, {NULL, 0, 0}};
static const binary_t level_shift[] = {{"<<", LEDVM_SHL, 0}, {">>", LEDVM_SHR, 0}, {NULL, 0, 0}};
static const binary_t level_add[] = {{"+", LEDVM_ADD, 0}, {"-", LEDVM_SUB, 0}, {NULL, 0, 0}};
static const binary_t level_mul[] = {{"*", LEDVM_MUL, 0}, {"/", LEDVM_DIV, 0}, {"%", LEDVM_MOD, 0}, {NULL, 0, 0}};

static const binary_t *const levels[] = {level_or, level_xor, level_and, level_equal, level_compare, level_shift, level_add, level_mul};
#define N_LEVELS (sizeof levels / sizeof levels[0])

static void fail(ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (c->failed) {
        return;
    }
    c->failed = 1;
    if (c->err_size == 0u) {
        return;
    }
    n = snprintf(c->err, c->err_size, "at %u: ", (unsigned)(c->p - c->start) + 1u);
    if (n >= 0 && (size_t)n < c->err_size) {
        va_start(ap, fmt);
        vsnprintf(c->err + n, c->err_size - (size_t)n, fmt, ap);
        va_end(ap);
    }
}

static void skip_space(ctx_t *c)
{
    while (isspace((unsigned char)*c->p)) {
        c->p++;
    }
}

static void emit(ctx_t *c, uint8_t byte)
{
    if (c->failed) {
        return;
    }
    if (c->len >= LEDVM_CODE_MAX) {
        fail(c, "the program does not fit into %u bytes", (unsigned)LEDVM_CODE_MAX);
        return;
    }
    c->code[c->len++] = byte;
}

static int accept(ctx_t *c, const char *text)
{
    size_t n = strlen(text);

    skip_space(c);
    if (strncmp(c->p, text, n) != 0) {
        return 0;
    }
    c->p += n;
    return 1;
}

static void expect(ctx_t *c, const char *text)
{
    if (!c->failed && !accept(c, text)) {
        fail(c, "'%s' expected", text);
    }
}

static void emit_number(ctx_t *c, unsigned long v)
{
    if (v <= 255u) {
        emit(c, LEDVM_K8);
        emit(c, (uint8_t)v);
    } else {
        emit(c, LEDVM_K16);
        emit(c, (uint8_t)(v & 0xFFu));
        emit(c, (uint8_t)(v >> 8));
    }
}

static void expression(ctx_t *c);

/* An identifier at the position, copied. Returns its length, 0 if there is none. */
static size_t identifier(ctx_t *c, char *out, size_t size)
{
    size_t n = 0;

    skip_space(c);
    while ((isalnum((unsigned char)c->p[n]) || c->p[n] == '_') && !(n == 0 && isdigit((unsigned char)c->p[n]))) {
        if (n + 1u < size) {
            out[n] = c->p[n];
        }
        n++;
    }
    out[n < size ? n : size - 1u] = '\0';
    return n;
}

/* p0..p7, v0..v3: the digit if `name` is `letter` and a digit below `limit`, otherwise -1. */
static int indexed(const char *name, char letter, int limit)
{
    if (name[0] == letter && isdigit((unsigned char)name[1]) && name[2] == '\0' && name[1] - '0' < limit) {
        return name[1] - '0';
    }
    return -1;
}

static void arguments(ctx_t *c, unsigned count)
{
    unsigned i;

    expect(c, "(");
    for (i = 0; i < count && !c->failed; i++) {
        if (i) {
            expect(c, ",");
        }
        expression(c);
    }
    expect(c, ")");
}

static void primary(ctx_t *c)
{
    char name[16];
    size_t n;
    int k;

    skip_space(c);
    if (accept(c, "(")) {
        expression(c);
        expect(c, ")");
        return;
    }
    if (isdigit((unsigned char)*c->p)) {
        char *end;
        unsigned long v = strtoul(c->p, &end, (c->p[0] == '0' && (c->p[1] == 'x' || c->p[1] == 'X')) ? 16 : 10);

        if (v > 65535ul) {
            fail(c, "a number goes up to 65535");
            return;
        }
        c->p = end;
        emit_number(c, v);
        return;
    }
    n = identifier(c, name, sizeof name);
    if (n == 0u) {
        fail(c, *c->p ? "unexpected '%c'" : "the formula ends too early", *c->p);
        return;
    }
    if (n >= sizeof name) {
        fail(c, "unknown name");
        return;
    }
    c->p += n;
    if (strcmp(name, "i") == 0) {
        emit(c, LEDVM_I);
    } else if (strcmp(name, "n") == 0) {
        emit(c, LEDVM_N);
    } else if (strcmp(name, "t") == 0) {
        emit(c, LEDVM_T);
    } else if (strcmp(name, "rand") == 0) {
        emit(c, LEDVM_RAND);
    } else if ((k = indexed(name, 'p', LEDVM_PARAMS)) >= 0) {
        emit(c, (uint8_t)LEDVM_P(k));
    } else if ((k = indexed(name, 'v', LEDVM_VARS)) >= 0) {
        emit(c, (uint8_t)LEDVM_LOAD(k));
    } else if (strcmp(name, "sin8") == 0) {
        arguments(c, 1);
        emit(c, LEDVM_SIN8);
    } else if (strcmp(name, "tri8") == 0) {
        arguments(c, 1);
        emit(c, LEDVM_TRI8);
    } else if (strcmp(name, "abs") == 0) {
        arguments(c, 1);
        emit(c, LEDVM_ABS);
    } else if (strcmp(name, "min") == 0) {
        arguments(c, 2);
        emit(c, LEDVM_MIN);
    } else if (strcmp(name, "max") == 0) {
        arguments(c, 2);
        emit(c, LEDVM_MAX);
    } else if (strcmp(name, "prev") == 0) {
        char *end;
        unsigned long channel;

        expect(c, "(");
        skip_space(c);
        channel = strtoul(c->p, &end, 10);
        if (c->failed) {
            return;
        }
        if (end == c->p || channel > 3u) {
            fail(c, "prev() takes a channel, 0..3");
            return;
        }
        c->p = end;
        expect(c, ")");
        emit(c, (uint8_t)LEDVM_PREV(channel));
    } else {
        c->p -= n;
        fail(c, "unknown name '%s'", name);
    }
}

static void unary(ctx_t *c)
{
    if (c->nest >= NEST_MAX) {
        fail(c, "nested too deep");
        return;
    }
    c->nest++;
    skip_space(c);
    if (*c->p == '-') {
        c->p++;
        unary(c);
        emit(c, LEDVM_NEG);
    } else if (*c->p == '!' && c->p[1] != '=') {
        c->p++;
        unary(c);
        emit(c, LEDVM_NOT);
    } else if (*c->p == '~') {
        c->p++;
        unary(c);
        emit_number(c, 0xFFFFu);
        emit(c, LEDVM_XOR);
    } else {
        primary(c);
    }
    c->nest--;
}

/* A single '&', '|', '<', or '>' must not take the first character of "&&", "||", "<<", or ">>". */
static int operator_at(ctx_t *c, const binary_t *b)
{
    size_t n = strlen(b->text);

    skip_space(c);
    if (strncmp(c->p, b->text, n) != 0) {
        return 0;
    }
    if (n == 1u && strchr("&|<>", b->text[0]) && c->p[1] == b->text[0]) {
        return 0;
    }
    if (n == 1u && strchr("<>", b->text[0]) && c->p[1] == '=') {
        return 0;
    }
    c->p += n;
    return 1;
}

static void binary(ctx_t *c, size_t level)
{
    int found = 1;

    if (level >= N_LEVELS) {
        unary(c);
        return;
    }
    binary(c, level + 1u);
    while (found && !c->failed) {
        const binary_t *b;

        found = 0;
        for (b = levels[level]; b->text; b++) {
            if (operator_at(c, b)) {
                binary(c, level + 1u);
                emit(c, b->op);
                if (b->op2) {
                    emit(c, b->op2);
                }
                found = 1;
                break;
            }
        }
    }
}

static void expression(ctx_t *c)
{
    if (c->nest >= NEST_MAX) {
        fail(c, "nested too deep");
        return;
    }
    c->nest++;
    binary(c, 0);
    if (!c->failed && accept(c, "?")) {
        expression(c);
        expect(c, ":");
        expression(c);
        emit(c, LEDVM_SEL);
    }
    c->nest--;
}

/* v0 = ...; or the output at the end. Returns 1 if it was the output. */
static int statement(ctx_t *c)
{
    char name[16];
    size_t n = identifier(c, name, sizeof name);
    int k = n < sizeof name ? indexed(name, 'v', LEDVM_VARS) : -1;
    unsigned count = 0;
    uint8_t op = 0;

    if (k >= 0) {
        const char *mark = c->p;

        c->p += n;
        if (accept(c, "=") && *c->p != '=') {
            expression(c);
            expect(c, ";");
            emit(c, (uint8_t)LEDVM_STORE(k));
            return 0;
        }
        c->p = mark;
    }
    if (strcmp(name, "rgb") == 0) {
        count = 3;
        op = LEDVM_RGB;
    } else if (strcmp(name, "rgbw") == 0) {
        count = 4;
        op = LEDVM_RGBW;
    } else if (strcmp(name, "hsv") == 0) {
        count = 3;
        op = LEDVM_HSV;
    } else {
        fail(c, "a formula ends with rgb(), rgbw(), or hsv()");
        return 1;
    }
    c->p += n;
    arguments(c, count);
    emit(c, op);
    return 1;
}

uint8_t hubusd_ledc_compile(const char *formula, uint8_t *code, char *err, size_t err_size)
{
    ctx_t c;

    memset(&c, 0, sizeof c);
    c.start = formula;
    c.p = formula;
    c.code = code;
    c.err = err;
    c.err_size = err_size;
    if (err_size) {
        err[0] = '\0';
    }
    while (!c.failed && !statement(&c)) {
    }
    skip_space(&c);
    if (!c.failed && *c.p) {
        fail(&c, "unexpected text after the colour");
    }
    if (!c.failed && !ledvm_check(code, (uint8_t)c.len)) {
        c.p = c.start;
        fail(&c, "too many values at once for the stack of the slave (%u)", (unsigned)LEDVM_STACK);
    }
    return c.failed ? 0u : (uint8_t)c.len;
}
