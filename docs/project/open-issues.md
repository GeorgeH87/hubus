# Open Issues

What is still open. The specification is complete except for values that can only be measured on hardware; the implementation status is tracked in the [roadmap](../../TODO.md).

## H2: Master Clock Implementation

- **Status:** open. The specification part is resolved: all timing values are minimums, so jitter only slows the bus down ([§1.4](../spec/01-physical-layer.md#14-timing-parameters)).
- **Problem:** a bit-banged clock in Linux user space has jitter and occasional long pauses; the minimums `T_CLK_LOW`, `T_COND`, and `T_TURNAROUND` must still be met reliably.
- **Direction:** `libgpiod` in user space ([§1.5](../spec/01-physical-layer.md#15-nodes)). Measure the achievable clock rate and jitter on Pi 3, Pi 4, and Pi 5; use real-time priority if needed. `master/hubus_master_gpiod.c` uses the libgpiod v2 API (compile-tested against 2.2.1, not yet run on a Raspberry Pi). Whether a v1 variant is needed depends on the Raspberry Pi OS version of the target.

## H3: Concrete Timing Values

- **Status:** open.
- **Problem:** `T_CLK`, `T_CLK_LOW`, `T_COND`, `T_TURNAROUND`, `T_SAMPLE`, the line-fault threshold, and the pull-up value have no measured values; the defaults of the master are conservative placeholders ([§1.4](../spec/01-physical-layer.md#14-timing-parameters)).
- **Direction:** derive `T_CLK_LOW` from the measured interrupt latency of the slaves (for example in `simavr`), and freeze all values after the first hardware experiments.

## Stack Use on 128 B RAM Devices

- **Status:** open.
- **Problem:** the default buffer size of 20 bytes for devices with 128 B RAM rests on an estimate of the interrupt path's stack use (about 40–50 bytes, from the compiler's stack usage report), not on a measurement ([slave library](../slave-library.md#supported-devices)).
- **Direction:** measure the maximum stack depth in `simavr`.

## Quiet Time After a Command

- **Status:** open.
- **Problem:** a slave that has to disable interrupts for longer than a clock phase, like the LED strip slave during `SHOW` ([example](../example-ledstrip.md#timing)), misses edges if the master keeps clocking. hubusd sends queued commands, pings, and bursts back to back and has no way to pause the bus after a given command.
- **Direction:** a per-slave setting in hubusd, for example `quiet: {opcode: 0x42, us: 2500}`: after that command the bus thread sends nothing, answers no idle event, and pings no slave for the given time. The master library needs no change; the waiting belongs to the application that schedules transactions. Raw frames over MQTT would also benefit from a topic that takes a whole frame and splits it into `WRITE` and `SHOW` in one queue entry, so that a full queue cannot drop part of a frame.

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
