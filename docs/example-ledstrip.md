# Example: LED Strip and Push Button

One ATtiny85 with two jobs: it drives a WS2812B LED strip that the master controls over HUBUS, and it reports a push button to the master. The firmware is `examples/slave-ledstrip/ledstrip_example.c`, the animation interpreter `examples/slave-ledstrip/ledvm.c`.

The slave keeps a raw frame buffer, which the master controls in one of two ways:

- **Fixed frame.** The master writes bytes into the buffer and tells the slave to send it to the strip once.
- **Animation.** The master sends a short program in bytecode ([Animations](#animations)). The slave runs it for every LED of every frame and shows the result, again and again, until the master stops it.

The slave has no built-in effects, colours, or brightness levels: what the strip shows is either the master's bytes or the result of the master's program ([design principles](overview.md#design-principles)). The strip itself cannot be read back. The state only tells the master which fixed frame the slave last sent and whether a program runs.

## Hardware

| ATtiny85 pin | Function |
|---|---|
| PB2 | `CLK` |
| PB3 | `DATA` (external pull-up to 3.3 V, [§1.1](spec/01-physical-layer.md#11-bus-lines)) |
| PB0 | WS2812B data input, through a 5 V buffer |
| PB1 | push button to ground (internal pull-up) |

- **Clock:** 8 MHz from the internal oscillator: clear the `CKDIV8` fuse (the factory setting is 1 MHz). The firmware refuses to build for any other `F_CPU`. 16 MHz from the PLL is outside the specification at 3.3 V.
- **Level:** the slave runs at 3.3 V ([§1.2](spec/01-physical-layer.md#12-logic-levels-and-voltage)), while a WS2812B powered from 5 V needs at least 3.5 V for a high input. Put a single 5 V buffer with TTL input levels between PB0 and the strip, for example a 74AHCT1G125 or a 74HCT14 used twice. The buffer sits on the strip side, not on the bus, so the rule against active components on the bus still holds. Without it the strip often works, but without margin.
- **Power:** the strip gets its own 5 V supply sized for its current (up to 60 mA per LED at full white); the 3.3 V regulator of the slave can share it. Strip, slave, and master need a common ground. Route the strip current so that it does not flow through the bus ground, and add a large capacitor at the strip input.

## Commands

| Opcode | Name | Arguments | Effect |
|---|---|---|---|
| `0x40` | `WRITE` | `off_hi off_lo data[1..28]` | Copies the data to byte offset `off` of the frame buffer. |
| `0x41` | `FILL` | `off_hi off_lo cnt_hi cnt_lo pattern[1..4]` | Repeats the pattern over `cnt` bytes from byte offset `off`. A 3-byte pattern is one RGB LED, a 4-byte pattern one RGBW LED. |
| `0x42` | `SHOW` | `[seq]` | Stops a running program and sends the frame buffer to the strip once. `seq` (`1..255`) goes into the state once the frame has been sent. |
| `0x43` | `PROG_WRITE` | `off code[1..29]` | Stops a running program and copies bytecode to offset `off` of the program buffer (48 bytes). |
| `0x44` | `PROG_RUN` | `len crc layout period` | Checks the program and starts it at time 0; without arguments it only stops the program. |
| `0x45` | `PARAM` | `k value[1..8]` | Sets the program parameters `p[k]`, `p[k+1]`, … (eight bytes). |
| `0x46` | `TIME` | `t_hi t_lo` | Sets the program time to `t` milliseconds. |

- Offsets and counts are in bytes, not LEDs, and bytes go to the strip in buffer order: GRB for the WS2812B, GRBW for an SK6812 RGBW strip. The same firmware therefore serves both.
- Bytes beyond a buffer are dropped. The frame buffer has `LED_BYTES` bytes, 180 by default (60 RGB LEDs).
- `WRITE` and `FILL` change only the buffer; nothing is visible before `SHOW`, or before the next frame of a program, which overwrites it.
- The opcodes stay clear of `0x10`, which the relay example uses. A group command still reaches every device type in the group, so send `SHOW` or `TIME` to a group only if the group holds nothing but LED slaves (see [Synchronous strips](#synchronous-strips)).

## State and Events

The state is three bytes, and the event buffer carries the same three bytes:

| Byte | Content |
|---|---|
| 0 | push button: `1` pressed, `0` released |
| 1 | `seq` of the last `SHOW` that was sent; `0` after power-up |
| 2 | program: `0` none, `1` running, `2` refused by the last `PROG_RUN` |

- A debounced button change writes the three bytes into the event buffer, and the master collects them in a burst ([§6.2](spec/06-operation.md#62-burst)).
- Power-up writes them too. A `0` in bytes 1 and 2 tells the master that the strip is dark and both buffers are empty, so it can send the frame or the program again. The master keeps them for this; the slave has no non-volatile copy.
- After a `SHOW` with a `seq` the master can check with `REQUEST_STATE` that the frame has been sent, and after a `PROG_RUN` that the program was accepted. This tells it that the commands arrived, not what the LEDs show.

## Animations

A program is one expression that computes the colour of one LED. The slave evaluates it for every LED of a frame, with the index of the LED, the program time, and the parameters as inputs, and writes the result into the frame buffer. Programs are small: the parameters change colours or speed without sending the program again.

The code is postfix (stack) bytecode: operands first, then the operation. The master compiles it from a readable formula; the slave only interprets and checks it.

```text
hsv(i * 4 + (t >> 3), 255, sin8((t >> 2) + i * 16))     # moving rainbow that pulses along the strip

I K8 4 MUL  T K8 3 SHR ADD   K8 255   T K8 2 SHR I K8 16 MUL ADD SIN8   HSV
02 00 04 22 04 00 03 29 20   00 FF    04 00 02 29 02 00 10 22 20 33    3E      (22 bytes)
```

### Instructions

Values are 16 bits. Addition, subtraction, multiplication, and shifts wrap around; comparisons are signed. `a b SUB` is `a - b`.

| Code | Name | Stack | Result |
|---|---|---|---|
| `0x00 v` | `K8` | → x | constant `v` (`0..255`) |
| `0x01 lo hi` | `K16` | → x | 16-bit constant |
| `0x02` | `I` | → x | index of the LED, `0..n-1` |
| `0x03` | `N` | → x | number of LEDs |
| `0x04` | `T` | → x | program time in ms, wraps after 65.5 s |
| `0x05` | `RAND` | → x | random number `0..255` |
| `0x06` | `DUP` | a → a a | |
| `0x07` | `SWAP` | a b → b a | |
| `0x08+k` | `P k` | → x | parameter `k` (`0..7`), `0..255` |
| `0x10+c` | `PREV c` | → x | current value of channel `c` of this LED (`0` R, `1` G, `2` B, `3` W) |
| `0x14+k` | `LOAD k` | → x | variable `k` (`0..3`) |
| `0x18+k` | `STORE k` | a → | into variable `k` |
| `0x1C` | `DROP` | a → | |
| `0x20`..`0x24` | `ADD SUB MUL DIV MOD` | a b → x | arithmetic; `DIV` and `MOD` by `0` give `0` |
| `0x25`..`0x27` | `AND OR XOR` | a b → x | bitwise |
| `0x28`, `0x29` | `SHL SHR` | a b → x | shift by `b & 15`; `SHR` is logical |
| `0x2A`, `0x2B` | `MIN MAX` | a b → x | |
| `0x2C`..`0x2E` | `LT GT EQ` | a b → x | `1` if true, else `0` |
| `0x30`..`0x32` | `NEG NOT ABS` | a → x | `NOT` is `1` for `0`, else `0` |
| `0x33` | `SIN8` | a → x | sine of `a & 255` (one period over 256), `1..255`, `128` at `0` |
| `0x34` | `TRI8` | a → x | triangle of `a & 255`, `0..254` |
| `0x38` | `SEL` | c a b → x | `c != 0 ? a : b` |
| `0x3C` | `RGB` | r g b → | colour of the LED, W = `0`; ends the program |
| `0x3D` | `RGBW` | r g b w → | colour with white; on an RGB layout W is dropped |
| `0x3E` | `HSV` | h s v → | hue, saturation, value (`0..255` each), W = `0` |

- **Outputs** are clamped to `0..255` and written in the byte order of the layout, so a program does not depend on the strip type.
- **Variables** keep their values from LED to LED and from frame to frame. `PROG_RUN` clears them and seeds the random numbers, so a program starts the same way every time. With them a program can carry a value along the strip or over time.
- **`PREV`** reads what the LED holds before the program writes it: the previous frame, or what `WRITE` and `FILL` put there. This gives trails, fading, and effects that build on a fixed frame.
- **Time:** `T >> k` with `k <= 8` runs through `SIN8` and `TRI8` without a jump when `T` wraps. `T / x` does not.

### Program check

`PROG_RUN` takes `len` (bytes of code), `crc` (CRC-8 of the code, as in the frames, [§4.12](spec/04-frames.md#412-integrity-check-crc8)), `layout`, and `period`:

| `layout` | Bytes per LED | Byte order |
|---|---|---|
| `0` | 3 | GRB (WS2812B) |
| `1` | 4 | GRBW (SK6812 RGBW) |
| `2` | 3 | RGB |
| `3` | 4 | RGBW |

`period` is the time between two frames in ms (`1..255`); frames that cannot be shown in time are dropped while the program time keeps running.

The slave starts the program only if the CRC matches and the code passes the check: only known instructions, complete operands, never more than eight values and never fewer than an instruction takes on the stack, and exactly one output instruction, at the end, which leaves the stack empty. Otherwise byte 2 of the state becomes `2`. There are no jumps, so a program runs at most one instruction per byte and always ends.

A `PROG_WRITE` stops the program before it changes the code. A program sent in several pieces therefore never runs half written, and a lost piece shows up as a CRC mismatch. The interpreter also checks the stack at run time, so code that changes while a frame is computed cannot do harm.

### Showing the frames

A program frame is sent to the strip in an **interruptible section** ([slave library](slave-library.md#api)), not in a blind section: the master does not ask for it, so it cannot keep away from the slave. The slave starts a frame only while the bus is between transactions. Before each byte it checks for an edge on `CLK` or `DATA`; when one arrives it gives up the rest of the frame after the current byte, and the interrupt handler takes the edge. The slave never misses a transaction, and nothing has to be configured on the master.

The animation pauses while the master is busy on the bus. A frame that was given up is sent again as a whole once the bus is free, or the next one is computed; the strip may show the first LEDs of the new frame and the rest of the old one until then. Between two frames the slave keeps the line low for at least 384 µs, the reset time of the strip (280 µs for the WS2812B-V5), so that a new frame never continues an earlier one.

### Synchronous Strips

Every slave counts the program time with its own oscillator, so strips with the same program drift apart. `PROG_RUN` or `TIME` to a group sets the program time of every strip in it at once; repeat `TIME` from time to time to keep them together. The group must hold LED slaves only, for example MCIDs `48..55` with the prefix `110` (`TARGET_LEN = 3`).

The same holds for fixed frames: the master writes every buffer with exact addressing and then sends one `SHOW` to the group. Every strip in the group is blind afterwards; hubusd waits for each of them according to its own blind class.

## Timing

`WRITE`, `PROG_WRITE`, `PARAM`, and `TIME` are short copies in the command callback. `FILL` and `PROG_RUN` take too long for the interrupt handler ([§7.7](spec/07-slave.md#77-implementation-notes)), so the callback hands them to the main loop:

- `FILL` runs at once, at about 1 µs per byte. Even the next short command frame takes several milliseconds on the bus, so the buffer is filled before it can arrive.
- `PROG_RUN` checks the program in well under a millisecond.
- `SHOW` sends the frame in a **blind section** ([§7.8](spec/07-slave.md#78-blind-sections)). The WS2812B timing (1.25 µs per bit, the high phase accurate to about 150 ns) leaves no room for the HUBUS interrupt, so the slave disables all interrupts for about 12.5 µs per byte, 2.2 ms for 60 RGB LEDs.

```c
if (show_req && strip_ready() && hubus_slave_blind_begin()) {   /* 0 while the slave is busy on the bus */
    show_req = 0;
    ws2812_send(0);
    state[1] = show_seq;
    hubus_slave_blind_end();                                     /* resynchronize, interrupts on again */
}
```

The firmware announces the blind time in `SLAVE_INFO`: the blind class is computed from `LED_BYTES` as the smallest class that covers `LED_BYTES × 12.5 µs + 1.5 ms` (the frame, the reset time of the strip, and the wait for the slave to be free), class 5 (4 ms) for the default 180 bytes. The master does not address the slave for that time after every command to it; the other slaves are served meanwhile ([§6.9](spec/06-operation.md#69-blind-time-after-a-command)). After the section the slave takes the current pin levels as its reference and waits for the next START, so the traffic it missed cannot confuse it. A pending button event is not announced in the meantime: it would hold `DATA` low and delay the section ([§7.8](spec/07-slave.md#78-blind-sections)).

The blind class applies to every command, also to those that do not make the slave blind: the master cannot tell the opcodes apart. That costs a few milliseconds per command, against about 28 ms for a full `WRITE` on the bus.

With the default timing (100 µs clock period, [hubusd timing](daemon.md#timing)) a `WRITE` with 28 data bytes takes about 28 ms on the bus. A full frame for 60 LEDs is seven writes and a `SHOW`, about 0.25 s with the blind times, so about four full frames per second. A solid colour is one `FILL` and one `SHOW`, about 25 ms. A faster bus clock speeds up the transfers in proportion. A program needs none of this: once it runs, the bus carries only parameter changes.

A program frame is computed with interrupts enabled. A short program such as `rgb(i + 10, p0, p1)` takes about 10 ms for 60 LEDs; each instruction costs roughly 25 µs per LED. `period` is therefore a lower bound: the frame rate is limited by the length of the program, and on a busy bus by the gaps between the transactions.

Measured in the simulation (`test/avr/test_avr_ledstrip.c`, ATtiny85 at 8 MHz): bit high times of 375 ns and 875 ns, at most 2 µs low inside a frame, and a longest response to an edge of 54 µs while a program runs on a busy bus, against 53 µs for the plain test firmware. The firmware declares no speed class: in the worst case an edge waits for one byte of the strip, up to 12.5 µs, on top of the longest interrupt handler.

## Sizes

ATtiny85, `LED_BYTES` 180, event buffer reduced to 3 bytes (`HUBUS_TX_BUF_SIZE=3`): 6042 bytes of flash and 356 bytes of RAM, of which the program buffer, parameters, and variables take about 70. Each LED adds 3 bytes of RAM. Of the 512 bytes, keep about 130 bytes for the stack (the simulation measures 78 with a program running), so `LED_BYTES` can go up to about 200 (66 RGB LEDs).

## Master Side

The master needs no protocol changes and no changes to the master library: the commands are ordinary `SEND_COMMAND` frames, and the button uses the regular event path. hubusd reads the blind class from `SLAVE_INFO` and waits by itself ([hubusd](daemon.md#slaves-that-are-blind-after-a-command)); nothing has to be configured for it.

With the default `readback: slave`, hubusd reads the state back after every command once the blind time is over; after a `SHOW` the state carries its `seq`, which confirms that the frame was sent, and after a `PROG_RUN` byte 2 says whether the program runs. `readback: none` saves that state read when the confirmation is not needed.

Through MQTT the strip can already be driven with raw commands on the `cmd` topic ([hubusd topics](daemon.md#commands-subscribed)):

```sh
mosquitto_pub -t hubus/slave/11/cmd -m '0x41 0 0 0 180 0 255 0'   # FILL: all 60 LEDs red (GRB 00 FF 00)
mosquitto_pub -t hubus/slave/11/cmd -m '0x42 1'                   # SHOW, seq 1
```

A program goes the same way, as hand-assembled bytecode. `rgb(p0, (p1 * sin8((t >> 3) + i * 8)) >> 8, 0)`, a wave of green running along the red strip:

```sh
mosquitto_pub -t hubus/slave/11/cmd -m '0x45 0 255 200'                       # PARAM: p0 = 255, p1 = 200
mosquitto_pub -t hubus/slave/11/cmd -m '0x43 0 8 9 4 0 3 41 2 0 8 34 32 51 34 0 8 41 0 0 60'   # PROG_WRITE
mosquitto_pub -t hubus/slave/11/cmd -m '0x44 19 71 0 20'                      # PROG_RUN: 19 bytes, CRC 71, GRB, 20 ms
```

A compiler from formulas to bytecode in hubusd is not implemented yet.

The commands of one frame or program are separate queue entries: if the queue overflows, part of a frame can be lost ([hubusd](daemon.md#how-it-works)). A topic that takes a whole frame would avoid that; it is not implemented yet. A lost program piece is caught by the CRC.

The button appears in Home Assistant with a custom profile:

```yaml
profiles:
  ledstrip:
    component: binary_sensor
    device_type: 0x04
    byte_offset: 0
slaves:
  - mcid: 11
    name: Hall strip
    profile: ledstrip
```
