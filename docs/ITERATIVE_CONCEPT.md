# Iterative Concept and Decision Log

## 1. Why Documentation-First

HUBUS starts with protocol specifications to reduce rework during firmware and hardware prototyping.

Core project goal: define a reusable communication library and protocol. Concrete device implementations are optional examples used for validation.

## 2. Confirmed Decisions

- Shared `DATA` line uses dominant-low signaling.
- Pulling `DATA` low equals bit `1`.
- Releasing `DATA` equals bit `0`.
- Higher MCID has higher arbitration priority.
- Arbitration compares MCID from MSB to LSB.
- v0 frame includes CRC8.
- Design principle: lightweight slaves, complexity on the master.
- Design principle: rare, self-recovering corner cases are accepted and documented instead of prevented at any cost.
- Slaves have no time base and implement no timeouts; all timeouts, retries, and recovery are master-side.
- `CLK` idles low; `DATA` changes only while `CLK` is low and is sampled on the rising edge.
- Frames are delimited by START (`DATA` falls while `CLK` high) and STOP (`DATA` rises while `CLK` high).
- RESET = START immediately followed by STOP. It resets the slave communication stack but preserves buffered data, the `GEN` counter, and the pending flag.
- Master sends RESET at startup (mandatory) and before shutdown (optional), and after detected faults.
- MCID `0` is reserved; valid slave MCIDs are `1..63` in v0.
- Every arbitration bit is followed by a handoff slot, driven by slaves that lost at exactly that bit.
- The master derives prefix masks from low handoff slots and processes them with a last-in-first-out pending mask stack. A burst serves all slaves pending at its start in descending MCID order, with one arbitration per slave.
- `REQUEST_STATE`, `REQUEST_INFO` and `REQUEST_DATA` use unified target-mask addressing. A group request is answered by the arbitration winner in the same transaction (no `POLL_PENDING`); the arbitration covers only the MCID bits below the prefix. Group `SEND_COMMAND` stays one-way: slaves report results of group commands via the pending flag and burst.
- Commands are fire-and-forget: no `SLAVE_ACK`, no `SLAVE_ERROR`. The master verifies states by reading them back.
- One outgoing buffer per slave, read via `REQUEST_DATA` / `SLAVE_DATA`. `STATUS_CAPTURE` writes into the same buffer.
- `REQUEST_INFO` returns static metadata as `SLAVE_INFO`: protocol version, capability flags, application data.
- Implicit read confirmation: the slave clears buffer and pending flag on the STOP following its complete `SLAVE_DATA` response. On a missing, incomplete, or invalid response the master ends with RESET, which preserves both for a retry.
- `DATA` is open-drain for all nodes including the master; `CLK` is push-pull from the master.
- One turnaround slot separates master transmission from slave transmission (response or arbitration). Slaves compute CRC8 incrementally so the turnaround slot suffices.
- The arbitration winner is served in a separate `REQUEST_DATA` transaction; slaves only transmit frames after exactly addressed `REQUEST_INFO`/`REQUEST_DATA`.
- Missing response detection: slave type codes start with `1`, so a released first bit after the turnaround slot means "no response". No response timeout needed.
- Idle event handshake: slave releases `DATA` after the first complete master clock pulse; master clocks until `DATA` is released.
- `SEND_COMMAND` opcodes: `0x00` = `STATUS_CAPTURE`, `0x01..0x0F` reserved for protocol use, `0x10..0xFF` application-defined.
- Frame format: 16-bit header, identical layout in both directions: `TYPE`(2) · `TARGET_LEN`/reserved(3) · `MCID`(6) · `LEN`(5), followed by payload and CRC8. No SOF field. CRC8 covers header bytes and payload in transmission order.
- 2-bit type codes per direction: master `00` REQUEST_STATE, `01` REQUEST_INFO, `10` REQUEST_DATA, `11` SEND_COMMAND; slave `10` SLAVE_DATA, `11` SLAVE_INFO (`00`/`01` reserved). A state frame uses the `SLAVE_DATA` type code.
- Two kinds of data: the live state (`REQUEST_STATE`, any time, every slave with a state contends, nothing cleared) and the event buffer (`REQUEST_DATA`, only slaves with pending data contend, cleared after delivery). The state is read from the application at transmit time without a copy.
- Slave header uses the `TARGET_LEN` bit positions for `MORE` (1 bit, more fragments follow) and `GEN` (2-bit mandatory generation counter, incremented on every buffer write). Duplicates are detected master-side by equal `GEN` plus identical payload; fragments need no sequence number.
- Duplicate MCIDs are a configuration fault detected master-side only: commissioning scan (group `REQUEST_INFO`) and a runtime heuristic (repeated CRC failures for one MCID). `SLAVE_INFO` SHOULD contain a device-unique value. Slave-side collision detection was rejected because it would hide the fault and add slave logic.
- CRC8 = CRC-8/AUTOSAR (poly `0x2F`, init `0xFF`, xorout `0xFF`). Hamming distance 4 up to 14 covered bytes; maximum payload stays 31 bytes, with a recommendation to fragment into ≤ 12-byte payloads where strongest integrity is needed.
- 3.3 V bus: all slaves supplied with 3.3 V, `DATA` pull-up to 3.3 V, no level shifters.
- Slave library supports classic tinyAVR and tinyAVR 0/1/2-series via a portable core plus one HAL per family. ATtiny85 is the reference target and always fully supported (31-byte buffers). Minimum device: 2 KB flash, 128 B RAM; buffer sizes are compile-time options, and `SLAVE_INFO` byte 2 announces the accepted command payload size. The master configures timing for the slowest slave.
- Master in C, Raspberry Pi 3 and newer (including Pi 5), GPIO via `libgpiod` with chip selection by label; `DATA` as open-drain line. Master logic runs on a HAL so it can be tested against a simulated bus.
- START/STOP detection stays interrupt-based on both `CLK` and `DATA` (a `DATA` change while `CLK` is high is a condition). Rejected alternative: detecting conditions by comparing `DATA` at the rising and falling `CLK` edges. It would need only one interrupt pin, but with slaves of different ISR latency a fast transmitter changes `DATA` after the falling edge before a slow slave samples it, which the slow slave would misread as START/STOP (for example dropping out of arbitration).
- Transactions of a burst are generated back-to-back with `CLK` held high after each STOP, and a slave announces itself only on a buffer write and once at the first falling `CLK` edge after a STOP/RESET (found during implementation). Without the first rule every pending slave would pull `DATA` low right after each STOP of a burst and make the next START impossible; without the second, pending slaves in different phases would keep toggling `DATA` during the idle handshake. See `PROTOCOL.md` sections 1.2 and 3.1.
- The master checks `DATA` before raising `CLK` for a START/RESET and again after raising it, which detects an event signal that races with the condition (`PROTOCOL.md` section 7.1) and repeats the transaction.
- MCID width stays 6 bits (`1..63`), also for v1 (D1). Electrical limits of a single open-drain line keep the practical node count well below 63; 8 bits would cost one header byte and 4 arbitration slots per frame.

## 3. Open Decisions

### D3: Fairness Strategy

Largely resolved by the burst mechanism: every slave pending at burst start is served within that burst, so a busy high-priority slave cannot starve lower-priority ones. Starvation across bursts is still possible only if bursts are aborted before completion.

Further fairness extensions (priority aging, deferral windows) are not planned unless tests show a need. Any such policy would be master-side.

### Resolved

- D1 (MCID width): 6 bits kept, see section 2.
- D2 (CRC8 parameters): CRC-8/AUTOSAR (poly `0x2F`, init `0xFF`, xorout `0xFF`, check `0xDF`), frozen by analysis; cross-platform tests only verify the check value. See `MESSAGE_FORMAT.md` section 7.
- D4 (header layout and SOF): 16-bit header without SOF, see section 2 and `MESSAGE_FORMAT.md` section 2.

## 4. Milestones

1. v0-docs: Stable core spec and examples.
2. v0-master: Raspberry Pi prototype for clock, framing, arbitration observation.
3. v0-slave: ATtiny85 prototype with arbitration and basic responses.
4. v0-multi: Multi-node stress tests and timeout tuning.
5. v1-spec: Fairness and commissioning improvements.

## 5. Deferred Topics

- Dynamic MCID assignment at runtime.
- Security/authentication on bus payloads.
- Long-distance physical layer optimization.
