# TODO

Implementation roadmap for HUBUS v0. The specification is complete except for values that must be measured on hardware (see `docs/OPEN_ISSUES.md`). IDs in brackets refer to entries in `docs/OPEN_ISSUES.md`.

## 1. Development Environment

- [x] Set up WSL2 with Ubuntu on the development PC (see `docs/DEVELOPMENT.md`).
- [x] Install host toolchain: `gcc`, `cmake`, `make`/`ninja`.
- [x] Install AVR toolchain: `gcc-avr`, `avr-libc`, `avrdude`, `simavr`.
- [x] Verify tinyAVR 0/1/2-series support of the AVR toolchain [R17, was P6].
- [x] VS Code with the WSL extension.

## 2. Project Skeleton

- [x] Create directory structure:
  - `core/`: hardware-independent protocol code (shared by master, slave, and tests)
  - `slave/`: slave library with HALs for classic tinyAVR and tinyAVR 0/1/2-series
  - `master/`: Raspberry Pi master with `libgpiod` HAL
  - `test/`: unit tests and bus simulation
- [x] CMake build with presets `host` (tests, simulation, master) and `avr` (slave firmware via toolchain file) [P2].
- [x] Add `.gitignore` [P2].
- [x] Integrate Unity as test framework [P3].
- [x] Choose and add a license [P5]. MIT.

## 3. Protocol Core (`core/`)

- [x] CRC-8/AUTOSAR, incremental byte-wise API; test against check value `0xDF` [L20, L23].
- [x] Frame codec: 16-bit header encode/decode (`TYPE`, `TARGET_LEN`/`MORE`+`GEN`, `MCID`, `LEN`) [L21].
- [x] Target-mask prefix match, MCID `0` reserved [L15].
- [x] Arbitration helpers: pending mask derivation from handoff slots, last-in-first-out mask stack.
- [x] `GEN` evaluation for the master (normal, duplicate, wrap, gap, baseline).
- [x] Slave state machine as pure logic: input = line event (`CLK` edge, START, STOP, RESET) and `DATA` level, output = `DATA` drive [L1–L12, L16–L19].
- [x] Unit tests for every module, built with sanitizers (`-fsanitize=address,undefined`).

## 4. Bus Simulation (`test/`)

- [x] Virtual open-drain wired-AND `DATA` line and master-driven `CLK`.
- [x] Simulated master and N simulated slaves using the real core code.
- [x] Scenarios: startup + RESET, idle event handshake, full burst with handoff slots, `REQUEST_INFO`, `REQUEST_DATA`, `SEND_COMMAND`, `STATUS_CAPTURE`, fragmented transfer with `MORE`.
- [x] Fault injection: bit flips, lost/extra clock edges, lost STOP, stuck line, duplicate MCID, event signal racing START.
- [x] Verify all accepted rare cases recover as specified.

## 5. Slave Library (`slave/`)

- [x] HAL interface: pin read, `DATA` pull-low/release, interrupt setup for `CLK` and `DATA` [L24].
- [x] HAL for classic tinyAVR (pin change interrupts, register names per device) [L24].
- [x] HAL for tinyAVR 0/1/2-series (`PORTx`/`VPORTx`, `PINnCTRL`) [L24].
- [x] Automatic HAL selection from the compiler device macro.
- [x] Public API: init, single outgoing buffer write with `MORE`, `STATUS_CAPTURE` callback, command callback, static `SLAVE_INFO` [L17, L18, L22].
- [x] Compile-time buffer sizes; `SLAVE_INFO` byte 2; ignore oversized commands [L25].
- [x] Protect buffer writes against the ISR (atomic access, defer writes during transmission) [L13, L19].
- [x] Flash/RAM budget for the ATtiny85 reference build; check with `avr-size`. About 1910 bytes flash and 97 bytes RAM for the reference build (budget 2050 / 112); 1810 / 94 without `REQUEST_STATE`, which the 2 KB devices ATtiny24/25 build without.
- [ ] Optional: run in simavr and measure ISR cycles (input for `T_CLK_LOW`, `T_COND`, `T_TURNAROUND`).
- [x] Rewrite library README in English; describe only implemented features [L14].
- [x] Remove the old ATtiny85-only implementation.

## 6. Master (`master/`)

- [x] HAL interface: set `CLK`, pull/release/read `DATA`, wait.
- [x] `libgpiod` HAL: chip selection by label, `DATA` as open-drain line; check `libgpiod` v1 vs v2 on the target OS [H2]. Implemented for the v2 API and compile-tested on Ubuntu 26.04 (libgpiod 2.2.1); not run on a Raspberry Pi yet, v1 support is still open.
- [x] Line conditions: START, STOP, RESET; startup sequence; error recovery.
- [x] Transactions: `REQUEST_INFO`/`REQUEST_DATA` (exact, or group with arbitration and handoff slots), `SEND_COMMAND`.
- [x] Burst scheduling with pending mask stack and retry budget.
- [x] Idle event handshake detection.
- [x] `GEN` duplicate/gap handling, fragment reassembly hooks for applications.
- [x] Commissioning scan (group `REQUEST_INFO`) and duplicate MCID heuristic.
- [x] Configurable timing minimums (for the slowest slave on the bus).
- [x] Real-time priority option for the clock thread.

## 7. Examples

- [x] Rewrite slave examples against the new API: relay/LED, switch, temperature sensor [E1–E3]. Compile-tested for the ATtiny85, not run on hardware.
- [x] Remove the superseded example files `slave-relay/relay_isr_only.c` and `relay_pcint_example.c` [E4].
- [x] Master example application (scan, burst, state, info, commands). Compiles; not run on a Raspberry Pi yet.
- [x] Update `examples/*.md` if behavior details change during implementation.

## 8. Continuous Integration

- [x] GitHub Actions: host build, unit tests, bus simulation [P4]. Workflow added; first run on GitHub pending.
- [x] AVR build matrix (for example ATtiny25, 85, 84, 88, 841, 1614, 3216) [P4].
- [x] Fail the build if the ATtiny85 reference build exceeds its flash/RAM budget [P4].

## 9. Hardware Bring-Up

- [ ] Get an ISP programmer (USBasp or Arduino as ISP) and a UPDI programmer for 0/1/2-series devices.
- [ ] Get a logic analyzer (8 channels) and install PulseView/sigrok.
- [ ] Breadboard setup: Raspberry Pi + ATtiny85 slaves at 3.3 V, `DATA` pull-up to 3.3 V.
- [ ] Verify ATtiny input thresholds at 3.3 V against the datasheet.
- [ ] Measure clock rate and jitter on Pi 3, Pi 4, and Pi 5 [H2].
- [ ] Determine concrete timing values and pull-up value; update `HARDWARE_REQUIREMENTS.md` [H3].
- [ ] Multi-node test with mixed device families (`v0-multi` milestone).
- [ ] Optional: sigrok protocol decoder for HUBUS.

## 10. Master Daemon (`hubusd`, MQTT)

Requirements: `docs/DAEMON_REQUIREMENTS.md`, operation: `docs/DAEMON.md`.

- [x] Confirm the open decisions (YAML, unknown slaves are ignored and reported in the master state).
- [x] Daemon core: bus thread, command queue, statistics, availability tracking, tested against the bus simulation with a fake MQTT client.
- [x] Device profiles (relay, switch, temperature, custom) and Home Assistant discovery.
- [x] Configuration file parser with validation (line numbers, unknown keys, secrets).
- [x] `libmosquitto` binding, reconnect, last will, retained state; end-to-end test with a real broker on the simulated bus.
- [x] systemd unit, installation notes.
- [ ] End-to-end test on hardware (Raspberry Pi, relay ATtiny, button ATtiny) and Home Assistant.
