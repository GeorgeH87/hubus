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
- **Open:** a hardware test (self-programming with `SELFPRGEN`, the hand-over from the application, the write time); an updater for the tinyAVR 0/1/2-series, whose protected boot section at the start of flash changes the layout (application area after `BOOTEND`, no reset vector rule); updates from hubusd, so that the service does not have to be stopped.

## Master Modules

- **Status:** open. The interface, the selection, the loading of plugins, and the entities of a module are implemented ([§6.10](../spec/06-operation.md#610-master-modules), [master library](../master-library.md#master-modules)); the functions of a module are not called yet.
- **Problem:** a module function (`on_set`, `on_frame`) is plugin code that runs in the master process. It must not block the bus thread, which keeps the timing of the bus, and a function that hangs or crashes must not take the bus down with it more than it has to. A crash in a plugin ends the whole service today; systemd restarts it and the startup sequence recovers the bus ([§8.9](../spec/08-faults.md#89-master-stops-mid-transaction)).
- **Direction:** the functions run on threads of their own, never on the bus thread; see the proposal below. Running a plugin in a process of its own would isolate crashes too, at the price of a second interface; decide once the first module with functions (the LED strip) exists.

### Proposed Threading Model

*Proposal of 2026-11-02, to be confirmed before it is implemented.*

hubusd has two threads today: the MQTT thread only queues requests, and the bus thread owns the bus, the slave table, and everything published. The proposal keeps it that way.

1. **The bus thread never calls a module function.** It puts jobs into the queue of the module: `create` and `destroy` of an instance per configured slave, `on_frame` for each frame of such a slave (a copy of at most 31 bytes), and `on_set` for a `set` message of an entity that has no fixed command. The queue is bounded (32 jobs). If it is full, the job is dropped, counted in the statistics, and the next `on_frame` of that slave says that frames were lost.
2. **One worker thread per module that has functions**, started when its first instance is created. The calls of one module run one after the other in the order of the jobs; modules do not wait for each other, so a slow or hanging module costs only itself. Modules without functions, all built-in ones, cost no thread.
3. **A function reaches hubusd only through `hubus_module_host_t`.** `host->send()` and the new `host->set_state()` touch neither the bus nor MQTT: they append to an output queue, which the bus thread empties in `hubusd_step()`. There a command is checked like a raw command (module present, `max_command`) and sent with `send_exact()`, so blind times, statistics, and readback apply; a state is cached and published like any other value. The bus thread stays the only owner of the bus, the slave table, and the MQTT state.
4. **Time limit per call** (`modules.call_timeout_ms`, default 1000 ms). The bus thread compares the start time of the running call with the clock in every step. When it is exceeded the module is marked failed: logged, `"module_failed": true` in the info topic of its slaves, no more jobs. Everything that is data keeps working (decoding the entities, fixed commands, blind times); what needs a function does not. A thread cannot be stopped safely, so it is left alone and the plugin is never closed; only a restart brings the module back.
5. **Instances** are created when a slave is configured and its module is known, and destroyed when the slave is removed, its module changes, or hubusd stops. `hubusd_shutdown()` queues the `destroy` calls and waits at most 2 s per module.
6. **A crash in a plugin** still ends the service; systemd restarts it ([§8.9](../spec/08-faults.md#89-master-stops-mid-transaction)).

What the interface needs for this (it has no users outside the repository, so it stays version 1):

- `host->set_state(host, key, text)`: a function publishes the value of one of its entities, for example the state of a light that no frame reports. Without it a light would stay empty.
- `on_frame(..., bool gap)`: frames were lost before this one, by a `GEN` gap or by a full queue. A module that reassembles fragments needs to know.
- `on_set` gets a `hubus_value_t` that hubusd parses from the text: a switch takes `ON` or `OFF`, a light `ON`, `OFF`, or `r,g,b`.
- A light in Home Assistant uses the default schema: `<key>/set` takes `ON` or `OFF`, `<key>/rgb/set` takes `r,g,b`, `<key>/state` reports `ON` or `OFF`. That needs no JSON parser in hubusd.

Alternatives and why they are not proposed:

| Alternative | Why not |
|---|---|
| Functions on the bus thread | Simple, no queues. A slave has no timeouts, so a long function only delays the bus, but a function that hangs stops everything. |
| One worker thread for all modules | A module that hangs would stop the functions of all others. |
| `pthread_cancel` for a late call | Cancelling a thread in the middle of plugin code can leave locks and memory in any state. |
| A process per plugin | Isolates crashes, but needs a second interface and a copy of the data; kept for later. |

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
