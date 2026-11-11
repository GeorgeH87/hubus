# Open Issues

What is still open. The specification is complete except for values that can only be measured on hardware; the implementation status is tracked in the [roadmap](../../TODO.md).

## H2: Master Clock Implementation

- **Status:** open. The specification part is resolved: all timing values are minimums, so jitter only slows the bus down ([§1.4](../spec/01-physical-layer.md#14-timing-parameters)).
- **Problem:** a bit-banged clock in Linux user space has jitter and occasional long pauses; the minimums `T_CLK_LOW`, `T_COND`, and `T_TURNAROUND` must still be met reliably.
- **Direction:** `libgpiod` in user space ([§1.6](../spec/01-physical-layer.md#16-nodes)). Measure the achievable clock rate and jitter on Pi 3, Pi 4, and Pi 5; use real-time priority if needed. `master/hubus_master_gpiod.c` uses the libgpiod v2 API (compile-tested against 2.2.1, not yet run on a Raspberry Pi). Whether a v1 variant is needed depends on the Raspberry Pi OS version of the target.

## H3: Concrete Timing Values

- **Status:** open.
- **Problem:** `T_CLK`, `T_CLK_LOW`, `T_COND`, `T_TURNAROUND`, `T_SAMPLE`, the line-fault threshold, and the pull-up value have no measured values; the defaults of the master are conservative placeholders ([§1.4](../spec/01-physical-layer.md#14-timing-parameters)).
- **Direction:** derive `T_CLK_LOW` from the measured interrupt latency of the slaves, and freeze all values after the first hardware experiments.
- **Measured in `simavr`** ([AVR tests](../development.md#avr-tests)): the ATtiny85 at 8 MHz with the test firmware answers every edge within 53 µs (speed class 9); the bus works down to a half period of 50 µs and fails at 45 µs. The master defaults (50 µs) are therefore not conservative for this device but just enough. The longest response is the start of an answer after the turnaround slot (`T_TURNAROUND`, default 200 µs); a few ordinary edges, at byte boundaries with the bitwise CRC, already take more than 45 µs.

## Speed Classes on the Master

- **Status:** the bus-wide part is implemented. Slaves announce their speed class in `SLAVE_INFO` ([§1.5](../spec/01-physical-layer.md#15-speed-classes)); the master library raises `T_CLK_LOW`, `T_CLK_HIGH`, `T_COND`, and `T_TURNAROUND` to the response time of the slowest known class plus a margin, never below the configured minimums, and hubusd warns when one slave slows down the whole bus ([master library](../master-library.md#speed-classes), [hubusd](../daemon.md#speed-classes)). Tested on the bus simulation; not run on hardware.
- **Open:** if measurements show a gain, clock the rest of a frame faster once the slower slaves have rejected its prefix. The microsecond values of the classes and the margin (10 µs by default) are checked against the measured response times ([H3](#h3-concrete-timing-values)).

## Stack Use on 128 B RAM Devices

- **Status:** measured; the default is unchanged.
- **Problem:** the default buffer size of 20 bytes for devices with 128 B RAM rested on an estimate of the interrupt path's stack use (about 40–50 bytes, from the compiler's stack usage report) ([slave library](../slave-library.md#supported-devices)).
- **Measured in `simavr`** ([AVR tests](../development.md#avr-tests)): 34 bytes of stack in total on the ATtiny85, of which 32 in the interrupt handler, with short callbacks (one of them writes the event buffer). The application's own callbacks and interrupts add to it.
- **Direction:** decide whether 128 B devices can keep larger buffers; a test firmware for the ATtiny25 would measure it on the device itself.

## Firmware Update

- **Status:** implemented for classic tinyAVR, tested in the bus simulation ([§6.8](../spec/06-operation.md#68-firmware-update)) and on a simulated ATtiny85 (`simavr`, [AVR tests](../development.md#avr-tests)); not yet run on hardware.
- **Open:** a hardware test (self-programming with `SELFPRGEN`, the hand-over from the application, the write time, the change of the MCID and its interruption); an updater for the tinyAVR 0/1/2-series, whose protected boot section at the start of flash changes the layout (application area after `BOOTEND`, no reset vector rule), with or without `UPDATE_SET_MCID`.

## MCID Assignment

- **Status:** resolved in software, not run on hardware ([slave library](../slave-library.md#assigning-the-mcid)). The source of the MCID is the choice of the firmware; recommended is the last EEPROM byte, written together with the image (`tools/flash-avr.sh`). Slaves with an updater that takes `UPDATE_SET_MCID` can get another MCID over the bus ([§6.8](../spec/06-operation.md#mcid)).
- **Accepted:** without a value per device, identical devices share the default MCID of their firmware; the commissioning scan reports them as duplicates, and one of them has to be given its MCID by hand. There is no automatic enumeration: two devices under the same MCID cannot be told apart and addressed one by one without a unique serial number. A firmware without update support can only get its MCID from the programmer.
- **Open:** the flash script with a real programmer, `EESAVE`, and avrdude older than 7.0, which may write the bytes in front of the MCID as well.

## D3: Fairness Strategy

- **Status:** largely resolved. A burst serves every slave pending at its start ([§6.2](../spec/06-operation.md#62-burst)); starvation across bursts is only possible if bursts are aborted before completion.
- **Direction:** priority aging or deferral windows only if tests show a need; any such policy would be master-side.

## Milestones

1. v0-docs: stable specification and examples. Done.
2. v0-master: Raspberry Pi prototype for clock, framing, and arbitration.
3. v0-slave: ATtiny85 prototype with arbitration and responses.
4. v0-multi: multi-node stress tests and timing tuning, with mixed device families.
5. v1-spec: fairness and commissioning improvements as needed.

## Deferred Topics

- Automatic MCID assignment at runtime (enumeration of devices that share an MCID, which needs a unique serial number).
- Security or authentication of bus payloads.
- Physical layer optimization for long distances.
