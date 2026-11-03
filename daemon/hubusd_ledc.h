/*
 * A compiler from formulas to the bytecode of LED animations (examples/slave-ledstrip/ledvm.h, docs/example-ledstrip.md).
 *
 *   v0 = v0 + 1;                     optional assignments to the variables v0..v3, each ending with ';'
 *   hsv(i * 4 + (t >> 3), 255, v0)   the colour: rgb(r, g, b), rgbw(r, g, b, w), or hsv(h, s, v)
 *
 * Expressions work as in C, with the same precedence: ?: | ^ & == != < > <= >= << >> + - * / % and the unary - ! ~. There are
 * no jumps in the bytecode, so `c ? a : b` computes both sides.
 *
 *   numbers       0..65535, decimal or 0x hexadecimal
 *   inputs        i (index of the LED), n (number of LEDs), t (time in ms), rand, p0..p7 (parameters), v0..v3 (variables)
 *   functions     sin8(a), tri8(a), abs(a), min(a, b), max(a, b), prev(c): what channel c of this LED holds (0 R, 1 G, 2 B, 3 W)
 */
#ifndef HUBUSD_LEDC_H
#define HUBUSD_LEDC_H

#include <stddef.h>
#include <stdint.h>

/* Compiles `formula` to `code` (room for LEDVM_CODE_MAX bytes) and returns its length. 0 if it cannot: `err` says why. What
 * comes out has passed ledvm_check(), the check of the slave. */
uint8_t hubusd_ledc_compile(const char *formula, uint8_t *code, char *err, size_t err_size);

#endif /* HUBUSD_LEDC_H */
