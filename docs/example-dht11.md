# Example: Temperature and Humidity Sensor (DHT11)

One ATtiny85 that reads a DHT11 module and offers the values to the master. It only answers reads and can be updated, nothing else. The firmware is `examples/slave-dht11/dht11_example.c`, the HWID type `0x86` variant `1` ([types](hwid-types.md)), the master module `dht11`.

## Hardware

| ATtiny85 pin | Function |
|---|---|
| PB2 | `CLK` |
| PB3 | `DATA` (external pull-up to 3.3 V, [§1.1](spec/01-physical-layer.md#11-bus-lines)) |
| PB0 | data pin of the DHT11 |

- **Pull-up:** the data pin needs 4.7 to 10 kOhm to 3.3 V; most DHT11 modules on a small board have it, a bare sensor does not.
- **Supply:** the DHT11 works from 3.3 V. Then its data pin is a 3.3 V signal and needs no level shifting.
- **Clock:** 8 MHz from the internal oscillator, as for the other examples (clear the `CKDIV8` fuse).

## State

There are no events and no commands; the master reads the state with `REQUEST_STATE` (hubusd: every `poll_interval_s`).

| Byte | Content |
|---|---|
| 0 | relative humidity in percent |
| 1 | temperature in degrees Celsius |
| 2 | fault: `0` the last reading was good; `1` no good reading yet, or two readings in a row failed (the values are then the last good ones, or `0`) |

The DHT11 delivers whole numbers; the decimal bytes of its answer are ignored. It is read once after one second and then every two seconds, which stays below its limit of one reading per second.

## Reading without disturbing the bus

A reading takes about 4 ms, and the sensor tells a 0 from a 1 by the length of a pulse (27 us against 70 us), so the slave cannot be interrupted in the middle. It uses an [interruptible section](slave-library.md) of the library:

1. The start signal, 20 ms of the line pulled low, runs with the interrupts on. The slave never blocks the bus for it.
2. The section begins only when the bus is between two transactions. Until then the line stays low for up to 10 ms more, which the sensor tolerates.
3. In the section the pin is sampled with all interrupts disabled. A bit is a 1 when its high phase lasts longer than the low phase before it, so the result does not depend on the clock. Every 12 us or so the code checks whether the bus has an edge waiting; if it has, the section ends at once and the reading is dropped.
4. A reading that was cut off by the bus is repeated after 100 ms, up to three times, and does not count as a fault of the sensor. When the bus kept the slave from reading for all three tries, it tries again after 0.5 s. A wrong checksum, a value out of range (humidity above 100 %, temperature above 60 degrees), or silence from the sensor counts: two such readings in a row set the fault byte.

The slave therefore reacts to an edge within about 12 us plus the interrupt latency, and the speed class stays undeclared (`0`), as for the LED strip. A bus that never rests for 4 ms keeps the sensor from being read; the master sees the old values, with the fault byte unchanged.

## Master side

The module `dht11` offers three entities: `temperature` (device class `temperature`, in degrees Celsius), `humidity` (device class `humidity`, in percent) and `fault`, a binary sensor for the fault byte. It is built into hubusd (`daemon/hubusd_modules.c`). The module has no state of its own, so a value that the slave could not refresh stays as it was; `fault` shows that.

## Firmware update

The application ends below the info page in front of the updater, checked by the build (`example_dht11_size`); it takes about 2 KB of flash and 62 bytes of RAM. Installing and updating work as for the switch ([slave library](slave-library.md#firmware-update)).

## Tests

`test_avr_examples` runs the firmware on a simulated ATtiny85 with a model of the DHT11 on PB0: start signal of at least 18 ms, then the 30 us, 80 us, 80 us, and 40 bits of 50 us low and 27 or 70 us high, driven by simavr cycle timers. The tests cover the values and their refresh, a wrong checksum, a sensor that does not answer, a value out of range, and 600 transactions of the master while the sensor is read: all of them succeed, and the reading is made once the bus rests.
