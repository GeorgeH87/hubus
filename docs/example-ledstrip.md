# Example: LED Strip and Push Button

One ATtiny85 with two jobs: it drives a WS2812B LED strip that the master controls over HUBUS, and it reports a push button to the master. The firmware is `examples/slave-ledstrip/ledstrip_example.c`.

The strip is controlled close to its data channel: the slave keeps a raw frame buffer, the master writes bytes into it and tells the slave when to send it. The slave knows nothing about colours, brightness, or effects; all of that is the master's job ([design principles](overview.md#design-principles)). The strip itself cannot be read back. The state only tells the master which frame the slave last sent.

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
| `0x42` | `SHOW` | `[seq]` | Sends the frame buffer to the strip. `seq` (`1..255`) goes into the state once the frame has been sent. |

- Offsets and counts are in bytes, not LEDs, and bytes go to the strip in buffer order: GRB for the WS2812B, GRBW for an SK6812 RGBW strip. The same firmware therefore serves both.
- Bytes beyond the buffer are dropped. The buffer has `LED_BYTES` bytes, 180 by default (60 RGB LEDs).
- `WRITE` and `FILL` change only the buffer; nothing is visible before `SHOW`. Several writes followed by one `SHOW` change the strip at once.
- The opcodes stay clear of `0x10`, which the relay example uses. A group command still reaches every device type in the group, so send `SHOW` to a group only if the group holds nothing but LED slaves (see [Synchronous strips](#synchronous-strips)).

## State and Events

The state is two bytes, and the event buffer carries the same two bytes:

| Byte | Content |
|---|---|
| 0 | push button: `1` pressed, `0` released |
| 1 | `seq` of the last `SHOW` that was sent; `0` after power-up |

- A debounced button change writes both bytes into the event buffer, and the master collects them in a burst ([§6.2](spec/06-operation.md#62-burst)).
- Power-up writes them too. A `0` in byte 1 tells the master that the strip is dark and the frame buffer empty, so it can send the frame again. The master keeps the last frame for this; the slave has no non-volatile copy.
- After a `SHOW` with a `seq` the master can check with `REQUEST_STATE` that the frame has been sent. This tells it that the commands arrived, not what the LEDs show.

## Timing

`WRITE` is a short copy in the command callback. `FILL` and `SHOW` take too long for the interrupt handler ([§7.7](spec/07-slave.md#77-implementation-notes)), so the callback hands them to the main loop:

- `FILL` runs at once, at about 1 µs per byte. Even the next short command frame takes several milliseconds on the bus, so the buffer is filled before it can arrive.
- `SHOW` waits until the transaction has ended with STOP (`hubus_slave_is_idle()`), then sends the frame with interrupts disabled. The WS2812B timing (1.25 µs per bit, the high phase accurate to about 150 ns) leaves no room for the HUBUS interrupt.

While interrupts are disabled the slave misses `CLK` and `DATA` edges. It would lose bits or mistake them for START or STOP, could then answer a request meant for another slave, or lose a command meant for itself. The master therefore keeps the bus quiet after a `SHOW`: no clock, no transaction, and no idle event handshake for

```
t_quiet = LED_BYTES × 11 µs + 300 µs
```

about 2.3 ms for 60 RGB LEDs. The 300 µs are the WS2812B latch time. `SHOW` is the only command that needs this; `WRITE` and `FILL` keep the slave responsive.

With the default timing (100 µs clock period, [hubusd timing](daemon.md#timing)) a `WRITE` with 28 data bytes takes about 28 ms on the bus. A full frame for 60 LEDs is seven writes and a `SHOW`, about 0.2 s, so about five full frames per second. A solid colour is one `FILL` and one `SHOW`, about 15 ms. A faster bus clock speeds up both in proportion.

## Sizes

ATtiny85, `LED_BYTES` 180, event buffer reduced to 2 bytes (`HUBUS_TX_BUF_SIZE=2`): 2412 bytes of flash and 261 bytes of RAM. Each LED adds 3 bytes of RAM. Of the 512 bytes, keep about 130 bytes for the stack (the interrupt path alone needs 40 to 50), so `LED_BYTES` can go up to about 300 (100 RGB LEDs).

## Master Side

The master needs no protocol changes and no changes to the master library: the commands are ordinary `SEND_COMMAND` frames, and the button uses the regular event path.

hubusd does not yet keep the bus quiet after a `SHOW` ([open issues](project/open-issues.md#quiet-time-after-a-command)). Without animations a `SHOW` is rare, and a collision with other traffic is unlikely. One collision is certain, though: with the default `readback: slave`, hubusd reads the state of a slave right after every command, including raw ones, and this `REQUEST_STATE` would fall into every `SHOW`. Configure the strip slave with `readback: none` (below). To check that a frame was sent, read the state later on the `read` topic and compare `seq`.

Through MQTT the strip can already be driven with raw commands on the `cmd` topic ([hubusd topics](daemon.md#commands-subscribed)):

```sh
mosquitto_pub -t hubus/slave/11/cmd -m '0x41 0 0 0 180 0 255 0'   # FILL: all 60 LEDs red (GRB 00 FF 00)
mosquitto_pub -t hubus/slave/11/cmd -m '0x42 1'                   # SHOW, seq 1
```

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
    readback: none        # no REQUEST_STATE right after SHOW
```

### Synchronous Strips

To switch several strips at once, the master writes every buffer with exact addressing and then sends one `SHOW` to the group. The group must hold LED slaves only, for example MCIDs `48..55` with the prefix `110` (`TARGET_LEN = 3`). The quiet time is that of the longest strip.
