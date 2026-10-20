# TODO

Implementation roadmap for HUBUS v0. The specification is complete except for values that must be measured on hardware (see `docs/OPEN_ISSUES.md`). IDs in brackets refer to entries in `docs/OPEN_ISSUES.md`.

## 1. Development Environment

- [ ] Set up WSL2 with Ubuntu on the development PC.
- [ ] Install host toolchain: `gcc`, `cmake`, `make`/`ninja`.
- [ ] Install AVR toolchain: `gcc-avr`, `avr-libc`, `avrdude`, `simavr`.
- [ ] Verify tinyAVR 0/1/2-series support of the AVR toolchain; install a newer avr-gcc or Microchip device packs if needed [P6].
- [ ] VS Code with the WSL extension.

## 2. Project Skeleton

- [ ] Create directory structure:
  - `core/`: hardware-independent protocol code (shared by master, slave, and tests)
  - `slave/`: slave library with HALs for classic tinyAVR and tinyAVR 0/1/2-series
  - `master/`: Raspberry Pi master with `libgpiod` HAL
  - `test/`: unit tests and bus simulation
- [ ] CMake build with presets `host` (tests, simulation, master) and `avr` (slave firmware via toolchain file) [P2].
- [ ] Add `.gitignore` [P2].
- [ ] Integrate Unity as test framework [P3].
- [ ] Choose and add a license [P5].

## 3. Protocol Core (`core/`)

- [ ] CRC-8/AUTOSAR, incremental byte-wise API; test against check value `0xDF` [L20, L23].
- [ ] Frame codec: 16-bit header encode/decode (`TYPE`, `TARGET_LEN`/`MORE`+`GEN`, `MCID`, `LEN`) [L21].
- [ ] Target-mask prefix match, MCID `0` reserved [L15].
- [ ] Arbitration helpers: pending mask derivation from handoff slots, last-in-first-out mask stack.
- [ ] `GEN` evaluation for the master (normal, duplicate, wrap, gap, baseline).
- [ ] Slave state machine as pure logic: input = line event (`CLK` edge, START, STOP, RESET) and `DATA` level, output = `DATA` drive [L1–L12, L16–L19].
- [ ] Unit tests for every module, built with sanitizers (`-fsanitize=address,undefined`).

## 4. Bus Simulation (`test/`)

- [ ] Virtual open-drain wired-AND `DATA` line and master-driven `CLK`.
- [ ] Simulated master and N simulated slaves using the real core code.
- [ ] Scenarios: startup + RESET, idle event handshake, full burst with handoff slots, `REQUEST_INFO`, `REQUEST_DATA`, `SEND_COMMAND`, `STATUS_CAPTURE`, fragmented transfer with `MORE`.
- [ ] Fault injection: bit flips, lost/extra clock edges, lost STOP, stuck line, duplicate MCID, event signal racing START.
- [ ] Verify all accepted rare cases recover as specified.

## 5. Slave Library (`slave/`)

- [ ] HAL interface: pin read, `DATA` pull-low/release, interrupt setup for `CLK` and `DATA` [L24].
- [ ] HAL for classic tinyAVR (pin change interrupts, register names per device) [L24].
- [ ] HAL for tinyAVR 0/1/2-series (`PORTx`/`VPORTx`, `PINnCTRL`) [L24].
- [ ] Automatic HAL selection from the compiler device macro.
- [ ] Public API: init, single outgoing buffer write with `MORE`, `STATUS_CAPTURE` callback, command callback, static `SLAVE_INFO` [L17, L18, L22].
- [ ] Compile-time buffer sizes; `SLAVE_INFO` byte 2; ignore oversized commands [L25].
- [ ] Protect buffer writes against the ISR (atomic access, defer writes during transmission) [L13, L19].
- [ ] Flash/RAM budget for the ATtiny85 reference build; check with `avr-size`.
- [ ] Optional: run in simavr and measure ISR cycles (input for `T_CLK_LOW`, `T_COND`, `T_TURNAROUND`).
- [ ] Rewrite library README in English; describe only implemented features [L14].
- [ ] Remove the old ATtiny85-only implementation.

## 6. Master (`master/`)

- [ ] HAL interface: set `CLK`, pull/release/read `DATA`, wait.
- [ ] `libgpiod` HAL: chip selection by label, `DATA` as open-drain line; check `libgpiod` v1 vs v2 on the target OS [H2].
- [ ] Line conditions: START, STOP, RESET; startup sequence; error recovery.
- [ ] Transactions: `POLL_PENDING` with arbitration and handoff slots, `REQUEST_INFO`, `REQUEST_DATA`, `SEND_COMMAND`.
- [ ] Burst scheduling with pending mask stack and retry budget.
- [ ] Idle event handshake detection.
- [ ] `GEN` duplicate/gap handling, fragment reassembly hooks for applications.
- [ ] Commissioning scan and duplicate MCID heuristic.
- [ ] Configurable timing minimums (for the slowest slave on the bus).
- [ ] Real-time priority option for the clock thread.

## 7. Examples

- [ ] Rewrite slave examples against the new API: relay/LED, switch, temperature sensor [E1–E4].
- [ ] Master example application (scan, burst, commands).
- [ ] Update `examples/*.md` if behavior details change during implementation.

## 8. Continuous Integration

- [ ] GitHub Actions: host build, unit tests, bus simulation [P4].
- [ ] AVR build matrix (for example ATtiny25, 85, 84, 88, 841, 1614, 3216) [P4].
- [ ] Fail the build if the ATtiny85 reference build exceeds its flash/RAM budget [P4].

## 9. Hardware Bring-Up

- [ ] Get an ISP programmer (USBasp or Arduino as ISP) and a UPDI programmer for 0/1/2-series devices.
- [ ] Get a logic analyzer (8 channels) and install PulseView/sigrok.
- [ ] Breadboard setup: Raspberry Pi + ATtiny85 slaves at 3.3 V, `DATA` pull-up to 3.3 V.
- [ ] Verify ATtiny input thresholds at 3.3 V against the datasheet.
- [ ] Measure clock rate and jitter on Pi 3, Pi 4, and Pi 5 [H2].
- [ ] Determine concrete timing values and pull-up value; update `HARDWARE_REQUIREMENTS.md` [H3].
- [ ] Multi-node test with mixed device families (`v0-multi` milestone).
- [ ] Optional: sigrok protocol decoder for HUBUS.
