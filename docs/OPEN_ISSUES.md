# Open Issues

Tracking list of known gaps, inconsistencies, and defects found in the project analysis. Decisions are recorded in `ITERATIVE_CONCEPT.md` once resolved; this file tracks what is still open.

All proposals follow the design principles in `README.md`: lightweight slaves, complexity on the master, and rare self-recovering corner cases are accepted rather than prevented at any cost.

Status values: `open`, `proposal`, `resolved`.

## 1. Specification

No open specification issues. Remaining items are hardware measurements (section 2) and implementation work (sections 3–5).

## 2. Hardware and Timing

### H2: Master Clock Implementation

- **Status:** open (spec part resolved: all timing values are minimums, so jitter only slows the bus)
- **Problem:** A Linux user-space bit-banged clock has significant jitter and occasional long pauses. `T_CLK_LOW`, `T_COND`, and `T_TURNAROUND` minimums must still be met reliably.
- **Direction:** `libgpiod` in user space is the baseline (`HARDWARE_REQUIREMENTS.md` §4.2). Measure achievable clock rate and jitter on Pi 3, Pi 4, and Pi 5; use real-time priority if needed. Note: `libgpiod` v1 and v2 have different APIs; check which version the target OS ships and keep the HAL thin enough to support both if necessary. Status of the implementation: `master/hubus_master_gpiod.c` uses the v2 API (compile-tested against libgpiod 2.2.1, not yet run on a Raspberry Pi); a v1 variant is not written.

### H3: Concrete Timing Values

- **Status:** open
- **Problem:** `T_CLK`, `T_CLK_LOW`, `T_COND`, `T_TURNAROUND`, `T_SAMPLE`, `T_GUARD`, the line-fault threshold, and the pull-up value have no concrete values.
- **Direction:** Derive `T_CLK_LOW` from measured slave ISR latency; freeze after first hardware experiments.

## 3. Slave Library (`slave/`, `core/`)

Rewritten against the specification (R18); the old implementation has been removed.

## 6. Resolved

### R18: Slave Library Rewrite, Master, Simulation, Build, and CI (L1–L25, E1–E4, P1–P5)

- **Status:** resolved (2026-10-21)
- **Resolution:** Protocol code split into `core/` (CRC-8/AUTOSAR, 16-bit header codec, arbitration helpers, `GEN` evaluation, slave state machine) shared by `slave/`, `master/`, and the tests; one HAL per device family (classic tinyAVR, tinyAVR 0/1/2-series) selected from the device macro; incremental CRC, turnaround slot, handoff slots, buffer clearing on STOP only, single outgoing buffer with `MORE`/`GEN`, static `SLAVE_INFO`, `STATUS_CAPTURE`, compile-time buffer sizes, write protection during transmission. The master (C, HAL-based, libgpiod v2 HAL for the Raspberry Pi) implements startup, recovery, bursts with the pending mask stack, commissioning, and duplicate-MCID heuristics. CMake presets, Unity tests, a bus simulation with fault injection, and a GitHub Actions workflow with the AVR device matrix and the ATtiny85 flash/RAM budget exist. See `docs/DEVELOPMENT.md` section 5.
- **Found during implementation (specification clarified):** pending slaves announcing themselves after every STOP would block the next START inside a burst, and pending slaves in different phases could keep each other's idle signals alive. Resolved by back-to-back transactions with `CLK` held high inside a burst and by the "announce once per STOP" rule (`PROTOCOL.md` sections 1.2 and 3.1).
- **Note:** the library alone needs about 1.9 KB of flash, so on 2 KB devices (ATtiny25 class) only about 70 bytes remain for the application (`slave/README.md`).


### R1: Frame Delimiting, Reset, and Slave Timeouts

- **Status:** resolved (2026-10-20)
- **Resolution:** START/STOP line conditions delimit frames, RESET (START + STOP) resets the slave communication stack while preserving buffered data, and slaves implement no timeouts. See `PROTOCOL.md` §1.1, §1.2, §3.3, §7.

### R2: MCID 0 Cannot Arbitrate (was S2)

- **Status:** resolved (2026-10-20)
- **Resolution:** MCID `0` is reserved; valid slave MCIDs are `1..63`. Every contender drives at least one arbitration bit low. See `ARBITRATION.md` §2, `MESSAGE_FORMAT.md` §5.

### R3: Pending Mask and Handoff Slot (was S4, S5)

- **Status:** resolved (2026-10-20)
- **Resolution:** Each arbitration bit is followed by a handoff slot, driven by slaves that lost at exactly that bit. The master derives prefix masks from low handoff slots and processes them via a last-in-first-out pending mask stack, serving all pending slaves in descending MCID order with one arbitration each. See `ARBITRATION.md` §4–§7, `PROTOCOL.md` §3.2.

### R4: POLL_PENDING Addressing (was S7)

- **Status:** resolved (2026-10-20)
- **Resolution:** `POLL_PENDING` uses unified target-mask addressing; only addressed pending slaves contend. See `MESSAGE_FORMAT.md` §3.1.

### R5: Arbitration Trigger in Wire Timeline (part of S6)

- **Status:** resolved (2026-10-20)
- **Resolution:** Only `POLL_PENDING` triggers arbitration. `WIRE_TIMELINE.md` rewritten accordingly.

### R7: Group Responses (was S6)

- **Status:** resolved (2026-10-20)
- **Resolution:** Group/broadcast addressing is strictly one-way (master to n slaves). No response frames to group frames; `REQUEST_INFO`/`REQUEST_DATA` require exact addressing. Slaves report results via the pending flag and the regular burst. See `MESSAGE_FORMAT.md` §3.1 "Direction Rule".

### R8: Command Opcodes, Request Semantics, Acknowledgements (was S8)

- **Status:** resolved (2026-10-20)
- **Resolution:**
  - Commands are fire-and-forget; no `SLAVE_ACK` and no `SLAVE_ERROR`. The master verifies states by reading them back.
  - One outgoing buffer per slave: `REQUEST_DATA` → `SLAVE_DATA`. `STATUS_CAPTURE` writes into the same buffer.
  - `REQUEST_INFO` → `SLAVE_INFO` with static metadata.
  - Opcodes: `0x00` `STATUS_CAPTURE`, `0x01..0x0F` reserved, `0x10..0xFF` application.
  - See `MESSAGE_FORMAT.md` §3, §3.0, §4.

### R9: Transaction Completion and Buffer Clearing (was S9)

- **Status:** resolved (2026-10-20)
- **Resolution:** The slave clears buffer and pending flag on the STOP following its complete `SLAVE_DATA` response. On a missing, incomplete, or invalid response the master ends the transaction with RESET, which preserves both for a retry. A lost STOP leads to duplicate delivery (accepted rare case, detectable via generation counter). See `PROTOCOL.md` §3.4.

### R10: Bus Direction per Phase (was S1)

- **Status:** resolved (2026-10-20)
- **Resolution:** `DATA` is open-drain for all nodes including the master. Transactions are fixed sequences (START · master frame · turnaround slot · response/arbitration · STOP/RESET). One turnaround slot plus a master-side minimum `T_TURNAROUND` separates master and slave transmission; slaves compute CRC8 incrementally. The arbitration winner is served in a separate `REQUEST_DATA` transaction. A released first bit after the turnaround slot means "no response". See `PROTOCOL.md` §1.3.

### R11: Idle Confirmation Tick Count (was S10)

- **Status:** resolved (2026-10-20)
- **Resolution:** The slave releases `DATA` after the first complete master clock pulse (after the falling edge, so the release is not mistaken for a STOP). The master clocks until `DATA` is released, bounded by its line-fault threshold. See `PROTOCOL.md` §3.1.

### R12: Header Layout, SOF, and MCID Width (was S11, S14; D1, D4)

- **Status:** resolved (2026-10-20)
- **Resolution:** 16-bit header with identical layout in both directions: `TYPE`(2) · `TARGET_LEN`/reserved(3) · `MCID`(6) · `LEN`(5). No SOF. 2-bit type codes per direction; slave codes `10`/`11` start with `1`. CRC8 over header bytes and payload in transmission order, byte-aligned. MCID width stays 6 bits. See `MESSAGE_FORMAT.md` §2–§4, §6, §7.

### R13: Multi-Frame Transfers and Generation Counter (was S12, S13)

- **Status:** resolved (2026-10-21)
- **Resolution:** The slave header carries `MORE` (bit 13) and a mandatory 2-bit `GEN` counter (bits 12–11) at the `TARGET_LEN` position. `GEN` increments on every buffer write; the master detects duplicates (equal `GEN` and identical payload) and overwrites (`GEN` gaps). Fragments pass sequentially through the single buffer, so no sequence number is needed. See `MESSAGE_FORMAT.md` §2, §4.3, §6.1.

### R14: Duplicate MCID Detection (was S3)

- **Status:** resolved (2026-10-21)
- **Resolution:** Duplicate MCIDs are a configuration fault detected by the master only: commissioning scan (`REQUEST_INFO` to every MCID) and a runtime heuristic (repeated CRC failures for one MCID). `SLAVE_INFO` SHOULD contain a device-unique value for reliable detection. Remaining blind spots are documented as accepted limitations. See `PROTOCOL.md` §3.5, `ERROR_SCENARIOS.md` §3.

### R15: CRC8 Parameters (was S15, D2)

- **Status:** resolved (2026-10-21)
- **Resolution:** CRC-8/AUTOSAR: poly `0x2F`, init `0xFF`, xorout `0xFF`, not reflected, check value `0xDF`. Chosen by exhaustive evaluation of all 8-bit polynomials for HUBUS frame lengths (Hamming distance 4 up to 14 covered bytes, odd-weight errors always detected) and because the non-zero init prevents all-zero/all-one frames from being valid. Maximum payload stays 31 bytes; fragments ≤ 12 bytes recommended for strongest integrity. See `MESSAGE_FORMAT.md` §7.

### R16: Voltage Levels (was H1)

- **Status:** resolved (2026-10-21)
- **Resolution:** HUBUS v0 is a 3.3 V bus: all slaves supplied with 3.3 V, `DATA` pull-up to 3.3 V, no level shifters. See `HARDWARE_REQUIREMENTS.md` §4.

### R17: AVR Toolchain for tinyAVR 0/1/2-series (was P6)

- **Status:** resolved (2026-10-21)
- **Resolution:** Ubuntu 26.04 ships `gcc-avr` 14.2 and `avr-libc` 2.2.1, which support classic tinyAVR and tinyAVR 0/1/2-series without Microchip device packs. Verified by compiling for ATtiny25, 85, 84, 88, 841, 202, 1614, 1624, and 3216. Limitation: `simavr` 1.6 only simulates classic devices (for example ATtiny84/85), not the 0/1/2-series. See `docs/DEVELOPMENT.md`.

### R6: Fairness (D3, largely)

- **Status:** resolved for v0 (2026-10-20)
- **Resolution:** A burst serves every slave pending at its start, so a busy high-priority slave cannot starve lower-priority ones. See `ARBITRATION.md` §9.
