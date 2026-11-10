# Example: Distance Sensor (HC-SR04)

One ATtiny85 with an HC-SR04 ultrasonic module. The master can read the distance at any time, and the slave announces an event when an object comes closer than a **trigger distance** that the master has set, and again when it leaves. The firmware is `examples/slave-distance/distance_example.c`, the HWID type `0x87` variant `1` ([types](hwid-types.md)), the master module `distance` (`daemon/hubusd_distance.c`). It is built with the firmware update.

## Hardware

| ATtiny85 pin | Function |
|---|---|
| PB2 | `CLK` |
| PB3 | `DATA` (external pull-up to 3.3 V, [§1.1](spec/01-physical-layer.md#11-bus-lines)) |
| PB0 | `TRIG` of the HC-SR04 |
| PB1 | `ECHO` of the HC-SR04, through a divider |

- **Level:** the HC-SR04 needs 5 V and answers with a 5 V echo, while the slave runs at 3.3 V ([§1.2](spec/01-physical-layer.md#12-logic-levels-and-voltage)). Put a divider in the echo line, for example 1.8 kOhm in series and 3.3 kOhm to ground, or use a module for 3.3 V (HC-SR04P, RCWL-1601). The 3.3 V trigger input of the HC-SR04 is accepted by most modules.
- **Power:** the module draws about 15 mA from 5 V; its ground is common with the slave.
- **Clock:** 8 MHz from the internal oscillator, as for the other examples (clear the `CKDIV8` fuse).

## State and Events

State and event are the same five bytes:

| Byte | Content |
|---|---|
| 0 | `1` an object is nearer than the trigger distance, else `0` (also `0` while no trigger distance is set) |
| 1-2 | distance in mm, big endian; `0`: no echo (nothing within about 5 m, or an object closer than 2 cm) |
| 3-4 | trigger distance in mm, big endian; `0`: not set |

- The slave measures about every 60 ms, which the module needs between two measurements. The master reads the last result with `REQUEST_STATE`, or collects it with `STATUS_CAPTURE`.
- An object counts as near after two measurements in a row below the trigger distance, and as gone after two in a row above the trigger distance plus 20 mm, or without an echo. A single wrong echo or a distance on the edge makes no events.
- Every change of the near flag is an event. Power-up is one too, with a trigger distance of `0`.
- The distance is not an event: it changes with every measurement. A master that wants it polls the state (hubusd: every `poll_interval_s`).

## Commands

| Opcode | Name | Arguments | Effect |
|---|---|---|---|
| `0x40` | `SET_THRESHOLD` | `mm_hi mm_lo` | Sets the trigger distance in mm, `0` to `4000`. `0` switches the events off. Other values are ignored. |

The trigger distance lives in RAM only: after a power-up it is `0`, and the slave says so in its first event. The master keeps the value and sends it again ([below](#master-side)); the slave holds no non-volatile copy.

## Measuring

- The echo pulse is measured against timer 0 (`F_CPU / 64`, 8 us per tick) that the main loop polls, with interrupts enabled. The interrupts of the bus therefore delay the detection of the end of the pulse by their duration (about 50 us, 8 mm), but they never lose time; no section blocks the bus.
- 1 tick is 1.375 mm (speed of sound 343 m/s, round trip). The result is accurate to a few mm in the simulation and to 2.5 cm with the bus busy; temperature changes the speed of sound by about 0.17 % per kelvin, which this firmware does not correct. The HC-SR04 itself is accurate to about 3 mm under good conditions.
- Without a start of the echo within 2 ms, or with an echo longer than 30 ms (nothing in range, the module then holds it for 38 ms), the distance is `0`.

## Master side

The module `distance` has three entities:

| Entity | Kind | Content |
|---|---|---|
| `distance` | sensor, device class `distance`, cm | the distance from bytes 1-2, with one decimal; `0.0` means no echo |
| `near` | binary sensor | byte 0 |
| `threshold` | number, 0 to 400 cm, step 1 | the trigger distance; `0` switches the events off |

The module sends the trigger distance to the slave. The **default is 30 cm**, until the user sets another value (Home Assistant, or `hubus/slave/<n>/threshold/set`). Every frame of the slave carries its trigger distance; whenever it differs from the value of the module, the module sends it again. After a power-up of the slave it is `0`, so the slave gets its value back with the first event, and a slave that missed a command gets it with the next frame. The user's value is kept as long as hubusd runs; after a restart of hubusd the default applies again.

## Firmware update

The application ends below the info page in front of the updater, checked by the build (`example_distance_size`); it takes about 2.6 KB of flash and 111 bytes of RAM. Installing and updating work as for the switch ([slave library](slave-library.md#firmware-update)).

## Tests

`test_avr_examples` runs the firmware on a simulated ATtiny85 with a model of the module on PB0 and PB1 (a trigger pulse of at least 8 us, the echo 0.4 ms later for 5.83 us per mm, a module without echo, and one that never ends its echo): the reading, the power-up event, the near and gone events with hysteresis, a single wrong echo, the commands (`0`, a value above 4000 mm, a new value while an object is near), and 400 transactions of the master while the distance is measured. `test_daemon_core` runs the module on the simulated bus: discovery, the default, the value after a power-up, and the range of the number.
