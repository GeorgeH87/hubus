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

- **Status:** open. Slaves announce their speed class in `SLAVE_INFO` ([§1.5](../spec/01-physical-layer.md#15-speed-classes)); the master library and hubusd do not evaluate it yet and keep using their configured minimums. hubusd only publishes it.
- **Direction:** first derive the bus-wide minimums from the slowest known slave and warn when one slave slows down the whole bus; later, if measurements show a gain, clock the rest of a frame faster once the slower slaves have rejected its prefix. The microsecond values of the classes are checked against the measured response times ([H3](#h3-concrete-timing-values)).

## Stack Use on 128 B RAM Devices

- **Status:** measured; the default is unchanged.
- **Problem:** the default buffer size of 20 bytes for devices with 128 B RAM rested on an estimate of the interrupt path's stack use (about 40–50 bytes, from the compiler's stack usage report) ([slave library](../slave-library.md#supported-devices)).
- **Measured in `simavr`** ([AVR tests](../development.md#avr-tests)): 34 bytes of stack in total on the ATtiny85, of which 32 in the interrupt handler, with short callbacks (one of them writes the event buffer). The application's own callbacks and interrupts add to it.
- **Direction:** decide whether 128 B devices can keep larger buffers; a test firmware for the ATtiny25 would measure it on the device itself.

## Firmware Update

- **Status:** implemented for classic tinyAVR, tested in the bus simulation ([§6.8](../spec/06-operation.md#68-firmware-update)); not yet run on an ATtiny85.
- **Open:** a hardware test (self-programming with `SELFPRGEN`, the hand-over from the application, the write time); an updater for the tinyAVR 0/1/2-series, whose protected boot section at the start of flash changes the layout (application area after `BOOTEND`, no reset vector rule); updates from hubusd, which today only shows the capability bits, so that the service does not have to be stopped.

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

- Dynamic MCID assignment at runtime.
- Security or authentication of bus payloads.
- Physical layer optimization for long distances.
