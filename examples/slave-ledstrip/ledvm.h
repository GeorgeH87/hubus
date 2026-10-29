/*
 * Bytecode for LED animations (docs/example-ledstrip.md, "Animations").
 *
 * The master sends a short program; the slave runs it once per LED and frame and writes the colour it computes into
 * the frame buffer. A program is one expression in postfix order, ending with exactly one output instruction (RGB,
 * RGBW, or HSV). There are no jumps, so a program runs at most one instruction per byte and always ends. Values are
 * 16 bits; arithmetic wraps around, comparisons are signed, and the outputs are clamped to 0..255.
 *
 * The instruction set (operands in push order, `a b ADD` is a + b):
 *
 *   0x00 K8 v         push v (0..255)             0x20 ADD   a + b         0x30 NEG   -a
 *   0x01 K16 lo hi    push hi:lo                  0x21 SUB   a - b         0x31 NOT   a == 0
 *   0x02 I            LED index 0..n-1            0x22 MUL   a * b         0x32 ABS   |a|
 *   0x03 N            number of LEDs              0x23 DIV   a / b (0: 0)  0x33 SIN8  sine of a & 255, 1..255
 *   0x04 T            time in ms (wraps)          0x24 MOD   a % b (0: 0)  0x34 TRI8  triangle of a & 255, 0..254
 *   0x05 RAND         random 0..255               0x25 AND   a & b
 *   0x06 DUP          a -> a a                    0x26 OR    a | b         0x38 SEL   c a b -> c ? a : b
 *   0x07 SWAP         a b -> b a                  0x27 XOR   a ^ b
 *   0x08+k P k        parameter k (0..7), 0..255  0x28 SHL   a << (b & 15) 0x3C RGB   r g b    (W = 0)
 *   0x10+c PREV c     current value of channel c  0x29 SHR   a >> (b & 15), logical
 *                     of this LED (R G B W)       0x2A MIN   0x2B MAX      0x3D RGBW  r g b w  (W dropped on RGB)
 *   0x14+k LOAD k     variable k (0..3)           0x2C LT    a < b
 *   0x18+k STORE k    pop into variable k         0x2D GT    a > b         0x3E HSV   h s v    (W = 0)
 *   0x1C DROP         a ->                        0x2E EQ    a == b
 *
 * The variables keep their values from LED to LED and from frame to frame; ledvm_start() clears them. `T >> k` with
 * k <= 8 runs through SIN8 and TRI8 without a jump when T wraps.
 */
#ifndef LEDVM_H
#define LEDVM_H

#include <stdint.h>

#ifndef LEDVM_CODE_MAX
#define LEDVM_CODE_MAX 48
#endif
#define LEDVM_STACK 8
#define LEDVM_PARAMS 8
#define LEDVM_VARS 4

/* Byte order of one LED in the frame buffer. */
#define LEDVM_LAYOUT_GRB 0  /* WS2812B */
#define LEDVM_LAYOUT_GRBW 1 /* SK6812 RGBW */
#define LEDVM_LAYOUT_RGB 2
#define LEDVM_LAYOUT_RGBW 3
#define LEDVM_LAYOUT_MAX 3

#define LEDVM_K8 0x00
#define LEDVM_K16 0x01
#define LEDVM_I 0x02
#define LEDVM_N 0x03
#define LEDVM_T 0x04
#define LEDVM_RAND 0x05
#define LEDVM_DUP 0x06
#define LEDVM_SWAP 0x07
#define LEDVM_P(k) (0x08 + (k))
#define LEDVM_PREV(c) (0x10 + (c))
#define LEDVM_LOAD(k) (0x14 + (k))
#define LEDVM_STORE(k) (0x18 + (k))
#define LEDVM_DROP 0x1C
#define LEDVM_ADD 0x20
#define LEDVM_SUB 0x21
#define LEDVM_MUL 0x22
#define LEDVM_DIV 0x23
#define LEDVM_MOD 0x24
#define LEDVM_AND 0x25
#define LEDVM_OR 0x26
#define LEDVM_XOR 0x27
#define LEDVM_SHL 0x28
#define LEDVM_SHR 0x29
#define LEDVM_MIN 0x2A
#define LEDVM_MAX 0x2B
#define LEDVM_LT 0x2C
#define LEDVM_GT 0x2D
#define LEDVM_EQ 0x2E
#define LEDVM_NEG 0x30
#define LEDVM_NOT 0x31
#define LEDVM_ABS 0x32
#define LEDVM_SIN8 0x33
#define LEDVM_TRI8 0x34
#define LEDVM_SEL 0x38
#define LEDVM_RGB 0x3C
#define LEDVM_RGBW 0x3D
#define LEDVM_HSV 0x3E

typedef struct {
    uint8_t code[LEDVM_CODE_MAX];
    uint8_t len;    /* program length; 0: no program */
    uint8_t layout; /* LEDVM_LAYOUT_* */
    uint8_t p[LEDVM_PARAMS];
    int16_t v[LEDVM_VARS];
    uint16_t rng;
} ledvm_t;

/* Bytes per LED of a layout. */
static inline uint8_t ledvm_bpp(uint8_t layout)
{
    return (uint8_t)(3u + (layout & 1u));
}

/* 1 if code[0..len) is a valid program: known instructions, complete operands, the stack within LEDVM_STACK and never
 * below empty, and exactly one output instruction, as the last one, which leaves the stack empty. */
uint8_t ledvm_check(const uint8_t *code, uint8_t len);

/* Prepares a checked program of `len` bytes in vm->code for a run: clears the variables and seeds the random numbers.
 * The parameters stay as they are. */
void ledvm_start(ledvm_t *vm, uint8_t len, uint8_t layout);

/*
 * Runs the program for LED `i` of `n` at time `t` and writes its colour to `led` (ledvm_bpp() bytes in the layout
 * order). PREV reads `led` before. Also safe if the code changes meanwhile: an instruction that would leave the stack
 * leaves the LED as it is.
 */
void ledvm_led(ledvm_t *vm, uint8_t *led, uint16_t i, uint16_t n, uint16_t t);

/* The functions behind SIN8, TRI8, and HSV, also for tests. */
uint8_t ledvm_sin8(uint8_t x);
void ledvm_hsv(uint8_t h, uint8_t s, uint8_t v, uint8_t rgb[3]);

#endif /* LEDVM_H */
