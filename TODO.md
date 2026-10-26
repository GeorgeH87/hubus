# TODO

Implementation roadmap for HUBUS v0. The specification is complete except for values that must be measured on hardware ([open issues](docs/project/open-issues.md)).

## 1. Development Environment

- [x] Set up WSL2 with Ubuntu on the development PC ([development](docs/development.md)).
- [x] Install host toolchain: `gcc`, `cmake`, `make`/`ninja`.
- [x] Install AVR toolchain: `gcc-avr`, `avr-libc`, `avrdude`, `simavr`.
- [x] Verify tinyAVR 0/1/2-series support of the AVR toolchain.
- [x] VS Code with the WSL extension.
- [x] Documentation tools: PlantUML for timing diagrams, mermaid-cli and a link check (`tools/build-docs.sh`).

## 2. Project Skeleton

- [x] Create directory structure:
  - `core/`: hardware-independent protocol code (shared by master, slave, and tests)
  - `slave/`: slave library with HALs for classic tinyAVR and tinyAVR 0/1/2-series
  - `master/`: Raspberry Pi master with `libgpiod` HAL
  - `test/`: unit tests and bus simulation
- [x] CMake build with presets `host` (tests, simulation, master) and `avr` (slave firmware via toolchain file).
- [x] Add `.gitignore`.
- [x] Integrate Unity as test framework.
- [x] Choose and add a license. MIT.

## 3. Protocol Core (`core/`)

- [x] CRC-8/AUTOSAR, incremental byte-wise API; test against check value `0xDF`.
- [x] Frame codec: 16-bit header encode/decode (`TYPE`, `TARGET_LEN`/`MORE`+`GEN`, `MCID`, `LEN`).
- [x] Target-mask prefix match, MCID `0` reserved.
- [x] Arbitration helpers: pending mask derivation from handoff slots, last-in-first-out mask stack.
- [x] `GEN` evaluation for the master (normal, duplicate, wrap, gap, baseline).
- [x] Slave state machine as pure logic: input = line event (`CLK` edge, START, STOP, RESET) and `DATA` level, output = `DATA` drive.
- [x] Unit tests for every module, built with sanitizers (`-fsanitize=address,undefined`).

## 4. Bus Simulation (`test/`)

- [x] Virtual open-drain wired-AND `DATA` line and master-driven `CLK`.
- [x] Simulated master and N simulated slaves using the real core code.
- [x] Scenarios: startup + RESET, idle event handshake, full burst with handoff slots, `REQUEST_INFO`, `REQUEST_DATA`, `REQUEST_STATE`, `SEND_COMMAND`, `STATUS_CAPTURE`, fragmented transfer with `MORE`.
- [x] Fault injection: bit flips, lost/extra clock edges, lost STOP, stuck line, duplicate MCID, event signal racing START.
- [x] Verify all accepted rare cases recover as specified.

## 5. Slave Library (`slave/`)

- [x] HAL interface: pin read, `DATA` pull-low/release, interrupt setup for `CLK` and `DATA`.
- [x] HAL for classic tinyAVR (pin change interrupts, register names per device).
- [x] HAL for tinyAVR 0/1/2-series (`PORTx`/`VPORTx`, `PINnCTRL`).
- [x] Automatic HAL selection from the compiler device macro.
- [x] Public API: init, event buffer write with `MORE`, `STATUS_CAPTURE` callback, command callback, static `SLAVE_INFO`, live state.
- [x] Compile-time buffer sizes; `SLAVE_INFO` byte 2; ignore oversized commands.
- [x] Event buffer and state as independent build options.
- [x] Protect buffer writes against the ISR (atomic access, refuse writes during transmission).
- [x] Speed class in `SLAVE_INFO` byte 3 (`speed_class` in the configuration, [§1.5](docs/spec/01-physical-layer.md#15-speed-classes)).
- [x] Flash/RAM budget for the ATtiny85 reference build, checked with `avr-size` ([slave library](docs/slave-library.md#optional-parts)).
- [ ] Optional: run in simavr and measure ISR cycles and stack depth (input for `T_CLK_LOW`, `T_COND`, `T_TURNAROUND`, and the buffer sizes of 128 B RAM devices; [open issues](docs/project/open-issues.md)).
- [x] Remove the old ATtiny85-only implementation.

## 6. Master (`master/`)

- [x] HAL interface: set `CLK`, pull/release/read `DATA`, wait.
- [x] `libgpiod` HAL: chip selection by label, `DATA` as open-drain line. Implemented for the v2 API and compile-tested on Ubuntu 26.04 (libgpiod 2.2.1); not run on a Raspberry Pi yet, v1 support is still open ([H2](docs/project/open-issues.md#h2-master-clock-implementation)).
- [x] Line conditions: START, STOP, RESET; startup sequence; error recovery.
- [x] Transactions: `REQUEST_STATE`/`REQUEST_INFO`/`REQUEST_DATA` (exact, or group with arbitration and handoff slots), `SEND_COMMAND`.
- [x] Burst scheduling with pending mask stack and retry budget.
- [x] Idle event handshake detection.
- [x] `GEN` duplicate/gap handling, fragment reassembly hooks for applications.
- [x] Commissioning scan (group `REQUEST_INFO`) and duplicate MCID heuristic.
- [x] Configurable timing minimums (for the slowest slave on the bus).
- [x] Real-time priority option for the clock thread.
- [ ] Derive the timing minimums from the speed classes of the known slaves; later clock the rest of a frame faster once slower slaves have rejected its prefix ([open issues](docs/project/open-issues.md#speed-classes-on-the-master)).

## 7. Examples

- [x] Slave examples for every combination of event buffer and state: switch, relay, sensor, passive sensor, relay without feedback. Compile-tested for the ATtiny85, not run on hardware.
- [x] Master example application (scan, burst, state, info, commands). Compiles; not run on a Raspberry Pi yet.

## 8. Continuous Integration

- [x] GitHub Actions: host build, unit tests, bus simulation, mutation check.
- [x] AVR build matrix, also without the event buffer and without both optional parts.
- [x] Fail the build if the ATtiny85 reference build exceeds its flash/RAM budget.
- [x] Documentation check: diagrams render, SVGs up to date, links resolve.

## 9. Hardware Bring-Up

- [ ] Get an ISP programmer (USBasp or Arduino as ISP) and a UPDI programmer for 0/1/2-series devices.
- [ ] Get a logic analyzer (8 channels) and install PulseView/sigrok.
- [ ] Breadboard setup: Raspberry Pi + ATtiny85 slaves at 3.3 V, `DATA` pull-up to 3.3 V.
- [ ] Verify ATtiny input thresholds at 3.3 V against the datasheet.
- [ ] Measure clock rate and jitter on Pi 3, Pi 4, and Pi 5 ([H2](docs/project/open-issues.md#h2-master-clock-implementation)).
- [ ] Determine concrete timing values and the pull-up value; update [§1.4](docs/spec/01-physical-layer.md#14-timing-parameters) ([H3](docs/project/open-issues.md#h3-concrete-timing-values)).
- [ ] Multi-node test with mixed device families (`v0-multi` milestone).
- [ ] Firmware update on an ATtiny85: install the updater, load an application over the bus, interrupt an update ([open issues](docs/project/open-issues.md#firmware-update)).
- [ ] Optional: sigrok protocol decoder for HUBUS.

## 10. Master Daemon (`hubusd`, MQTT)

Documentation: [hubusd](docs/daemon.md).

- [x] Configuration in YAML; unknown slaves are ignored and reported in the master state.
- [x] Daemon core: bus thread, command queue, statistics, availability tracking, tested against the bus simulation with a fake MQTT client.
- [x] Device profiles (relay, switch, temperature, custom) and Home Assistant discovery.
- [x] Configuration file parser with validation (line numbers, unknown keys, secrets).
- [x] `libmosquitto` binding, reconnect, last will, retained state; end-to-end test with a real broker on the simulated bus.
- [x] Readback from the cache for slaves that cannot be read back; periodic state reads (`poll_interval_s`).
- [x] systemd unit, installation notes.
- [ ] End-to-end test on hardware (Raspberry Pi, relay ATtiny, button ATtiny) and Home Assistant.

## 11. Firmware Update

- [x] Specification: capability bits 3 and 4, update opcodes, updater information, info page ([§6.8](docs/spec/06-operation.md#68-firmware-update)).
- [x] Slave library option `HUBUS_ENABLE_UPDATE`: hand-over to the updater.
- [x] Updater for classic tinyAVR (ATtiny85, 2 KB at the end of flash).
- [x] Master: `hubus_master_update()`, Intel HEX reader, `update` command of the master example.
- [x] Bus simulation test with bit errors and an interrupted update.
- [ ] Updater for the tinyAVR 0/1/2-series (protected boot section).
- [ ] Updates from hubusd over MQTT.
