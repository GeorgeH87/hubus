# Open Issues

Tracking list of known gaps, inconsistencies, and defects found in the project analysis. Decisions are recorded in `ITERATIVE_CONCEPT.md` once resolved; this file tracks what is still open.

All proposals follow the design principles in `README.md`: lightweight slaves, complexity on the master, and rare self-recovering corner cases are accepted rather than prevented at any cost.

Status values: `open`, `proposal`, `resolved`.

## 1. Specification

### S3: Duplicate MCID Is Not Detectable

- **Status:** open
- **Problem:** Two slaves with the same MCID produce identical bit patterns on a wired-AND line. The master cannot detect "unresolved arbitration" as described in `ARBITRATION.md` §5 and `ERROR_SCENARIOS.md` §3. At most, it shows up later as a CRC failure when both respond.
- **Direction:** Treat duplicate MCID as a commissioning fault detected by the master (for example repeated CRC failures from one MCID). A master-side bus scan (`REQUEST_INFO` to each MCID `1..63`) discovers present slaves; duplicates typically show up as CRC failures there. Candidate for an accepted rare case; rewrite §5 and error scenario 3 accordingly.

### S12: Multi-Frame Transfers

- **Status:** open
- **Problem:** Large datasets are split into several `SLAVE_DATA` frames, but there is no sequence number or last-frame flag. Lost or duplicated fragments cannot be detected.
- **Direction:** Keep it application-level as intended, but define a minimal convention. Candidate: a `MORE` flag in the reserved slave header bits (no payload cost), or first payload byte = fragment index with a "last" bit as an optional profile.

### S13: Generation Counter Placement

- **Status:** open
- **Problem:** The spec places the optional generation counter as the first payload byte of the slave buffer; the library keeps it separately (for two buffers) and never transmits it.
- **Direction:** Either keep it as first payload byte, or move a short (2-bit) counter into the reserved slave header bits (no payload cost; sufficient to detect duplicates after a lost STOP). Implement in the library as a compile-time option.

### S15: CRC8 Parameters (D2)

- **Status:** open
- **Problem:** Polynomial, init, and final XOR are not frozen. The library currently uses `0xD5 / 0x00 / 0x00`.
- **Direction:** Freeze after first cross-platform tests, as planned.

## 2. Hardware and Timing

### H1: Voltage Levels

- **Status:** open
- **Problem:** Raspberry Pi GPIO is 3.3 V. An ATtiny85 at 5 V needs about 3.0 V (0.6 × VCC) for a valid high, which is marginal with a 3.3 V pull-up; a 5 V pull-up would damage the Pi.
- **Direction:** Specify 3.3 V operation for slaves (ATtiny85 is rated for it at reduced clock), or require a level-shifting concept.

### H2: Master Clock Jitter

- **Status:** open
- **Problem:** A Linux user-space bit-banged clock has significant jitter and occasional long pauses. Slaves are tolerant (no timeouts), but `T_CLK_LOW` and `T_COND` minimums must still be met.
- **Direction:** Define timing as minimums only (no maximums), so jitter only slows the bus. Evaluate pigpio/DMA or a kernel driver for the master.

### H3: Concrete Timing Values

- **Status:** open
- **Problem:** `T_CLK`, `T_CLK_LOW`, `T_COND`, `T_SAMPLE`, `T_GUARD`, and master timeouts have no values.
- **Direction:** Derive `T_CLK_LOW` from measured slave ISR latency; freeze after first hardware experiments.

## 3. Slave Library (`lib/hubus-slave`)

The library will be reworked after the specification issues above are resolved. Known defects to address:

| ID | Location | Problem |
|---|---|---|
| L1 | `hubus_gpio_hal.c` | ISR only sets a flag; the state machine is never called. The advertised ISR-only architecture is not implemented. |
| L2 | `hubus_slave_config.h` | `HUBUS_CLK_PIN`/`HUBUS_DATA_PIN` are bit numbers (`PB2`/`PB3`) instead of `PINB`; line reads return constants. |
| L3 | `hubus_gpio_hal.c` | Missing `<avr/interrupt.h>` (`ISR`, `sei`); does not compile. |
| L4 | `hubus_state.c` | Bit polarity inverted (spec: low = `1`). |
| L5 | `hubus_state.c` | Slaves without a pending flag ignore `CLK` in idle and never receive master frames. |
| L6 | whole library | `hubus_data_pull_low()` is never called: no event signaling, no arbitration, no transmission. |
| L7 | `hubus_state.c` | Payload and CRC read from bit 24 although they start at bit 21. |
| L8 | `hubus_state.c` | CRC computed over C struct fields instead of the received bit stream, without the CRC byte. |
| L9 | PCINT | Fires on both edges; each bit sampled twice. To be replaced by edge discrimination including START/STOP detection. |
| L10 | `hubus_state.c` | Every received frame leads to `TRANSFER`, even when not addressed; data buffer cleared without sending. |
| L11 | `hubus_frame.c` | TX CRC header packing differs from RX validation. |
| L12 | `hubus_state.c` | Transmission, arbitration, and START/STOP/RESET handling are missing. |
| L13 | `hubus_buffer.c` | `memcpy` in setters is not atomic against the ISR. |
| L14 | `README.md` | Describes target features (ISR-only, timings) as implemented; written in German. |
| L15 | `hubus_state.c`, `README.md` | Accepts MCID `0` (documented range `0..63`); must reject or flag MCID `0` (valid range `1..63`). |
| L16 | `hubus_state.c` | Arbitration must implement handoff slots and only contend when addressed by the `POLL_PENDING` mask. |
| L17 | `hubus_buffer.*`, `hubus_slave.h` | Two buffers (data + status) must become one outgoing buffer; add static `SLAVE_INFO`; implement `STATUS_CAPTURE` opcode (application callback provides status). |
| L18 | `hubus_frame.h`, `hubus_state.h` | Remove `SLAVE_ACK`/`SLAVE_ERROR`/`SLAVE_STATUS` types and related events; add `SLAVE_INFO`. Callback events (`HUBUS_EVENT_REQUEST_DATA` for all request types) no longer match the spec. |
| L19 | `hubus_state.c` | Implement buffer clearing per `PROTOCOL.md` §3.4 (clear on STOP after complete response, keep on RESET); today the buffer is cleared in `TRANSFER` without sending anything. Guard against application writes during transmission. |
| L20 | `hubus_crc.*`, `hubus_state.c` | CRC8 must be computed incrementally while receiving/transmitting (today computed in bulk at frame end); implement turnaround slot and response start per `PROTOCOL.md` §1.3. |
| L21 | `hubus_frame.*`, `hubus_state.c`, `hubus_bitbuffer.*` | Implement the 16-bit header (no SOF, 2-bit TYPE, reserved bits in slave frames) and CRC8 over header + payload bytes; current code assumes SOF and the old 21-bit layout. |

## 4. Examples

| ID | Location | Problem |
|---|---|---|
| E1 | all slave examples | Use non-existent API (`HUBUS_EVENT_REQUEST_STATUS`, `hubus_slave_process()`, `hubus_slave_process_clk_edge()`); none compile. |
| E2 | `switch_example.c` | `(BUTTON_PIN >> BUTTON_PIN)` reads `PB1` instead of `PINB`. |
| E3 | `sensor_example.c` | ADC3 is on PB3, the `DATA` line; `ADCH` without `ADLAR` returns only 2 bits. |
| E4 | `slave-relay/` | `relay_example.c` and `relay_isr_only.c` are near duplicates. |

## 5. Project Infrastructure

| ID | Problem |
|---|---|
| P1 | No master (Raspberry Pi) implementation, although `v0-master` precedes `v0-slave` in the milestones. |
| P2 | No build system (Makefile / avr-gcc), no `.gitignore`. |
| P3 | No host-side unit tests (frame codec, CRC, arbitration simulation on a virtual wired-AND bus). |
| P4 | No CI. |
| P5 | No license. |

## 6. Resolved

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

### R6: Fairness (D3, largely)

- **Status:** resolved for v0 (2026-10-20)
- **Resolution:** A burst serves every slave pending at its start, so a busy high-priority slave cannot starve lower-priority ones. See `ARBITRATION.md` §9.
