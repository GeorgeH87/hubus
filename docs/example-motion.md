# Example: Motion Sensor (HC-SR501)

One ATtiny85 that reports a PIR motion module to the master and does nothing else. The firmware is `examples/slave-motion/motion_example.c`, the HWID type `0x88` variant `1` ([types](hwid-types.md)), the master module `motion`. It is built with the firmware update, so the module states `has_update`.

## Hardware

| ATtiny85 pin | Function |
|---|---|
| PB2 | `CLK` |
| PB3 | `DATA` (external pull-up to 3.3 V, [§1.1](spec/01-physical-layer.md#11-bus-lines)) |
| PB0 | output of the HC-SR501 |

- **Level:** the output of the HC-SR501 is 3.3 V when it sees motion, so it goes to PB0 directly, without a pull-up or a divider.
- **Power:** the module needs 4.5 to 20 V; it has its own 5 V supply with a common ground. The 3.3 V of the slave is too low for it.
- **Module settings:** how long the output stays high after motion (0.3 s to 5 min) and whether motion during that time extends it (retrigger jumper) are set on the module with its potentiometer and jumper. The slave reports what the pin does.
- **Clock:** 8 MHz from the internal oscillator, as for the other examples (clear the `CKDIV8` fuse).

## Events

There are no commands and no state. The event buffer holds one byte, which the master collects in a burst:

| Byte | Content |
|---|---|
| 0 | `1` motion, `0` none |

- A change of the pin is reported once it has been stable for 10 ms.
- The module needs 30 to 60 s after power-up before its output means anything. The slave ignores the pin for `PIR_WARMUP_MS` (30 s) and then reports the current level once, so the master knows it from then on. Until then the master has no value for it.
- A newer event overwrites an unread one, as with every event buffer: a master that polls rarely sees the latest level, not every change.

## Master side

The module `motion` has one entity, `motion`, a binary sensor with the device class `motion`. It is built into hubusd (`daemon/hubusd_modules.c`).

## Firmware update

The application ends below the info page in front of the updater, checked by the build (`example_motion_size`); it takes about 2.1 KB of flash and 97 bytes of RAM. Installing and updating work as for the switch ([slave library](slave-library.md#firmware-update)).

## Tests

`test_avr_examples` runs the firmware on a simulated ATtiny85 with a shorter warm-up (`PIR_WARMUP_MS=100`): a level during the warm-up is ignored, the level afterwards is announced, glitches shorter than the debounce time are not, and REQUEST_STATE gets no answer.
