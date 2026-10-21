# Arbitration Specification

## 1. Purpose

Arbitration decides which of several slaves answers a group `REQUEST_INFO` or `REQUEST_DATA`, on a shared `DATA` line without active bus arbitration hardware. The winner sends its response frame in the same transaction. Combined with handoff slots, the arbitration also tells the master where in the MCID space further matching slaves are located, so a burst (pending slaves) or a commissioning scan (all slaves) can serve all of them in priority order.

## 2. Fixed Semantics

- Logical `1`: slave actively pulls `DATA` low.
- Logical `0`: slave leaves `DATA` neutral (released/high).
- Priority: higher MCID wins.
- MCID `0` is reserved and never assigned to a slave.

This maps naturally to wired-AND behavior on the shared line because any active low dominates neutral.

## 3. Preconditions

- Master has issued `REQUEST_STATE`, `REQUEST_INFO` or `REQUEST_DATA` with valid CRC and `TARGET_LEN < 6`. Contenders are the slaves matching the target mask (`TARGET_LEN` + `MCID` prefix) that can answer: all of them for `REQUEST_INFO`, those with a state for `REQUEST_STATE`, only those with pending data for `REQUEST_DATA`. The others take no part and stay passive.
- Arbitration starts in the slot directly after the turnaround slot that follows the request frame. The winner starts its response frame in the slot directly after handoff slot `0`; the losers stay passive until the next START, STOP, or RESET (`PROTOCOL.md` section 1.3).
- With `TARGET_LEN = 6` there are no arbitration slots: the addressed slave answers directly after the turnaround slot.
- All contenders use the same MCID bit width for comparison.

## 4. Arbitration Sequence

Bit positions are numbered `5` (MSB) down to `0` (LSB) for the 6-bit v0 MCID. The first `TARGET_LEN` = `L` bits are the shared prefix of all contenders and are not arbitrated. Arbitration covers bit positions `5−L` down to `0`. Each bit position consists of two clock slots:

| Slot | Who drives `DATA` low |
|---|---|
| Arbitration bit `k` | Every active contender whose MCID bit `k` is `1` |
| Handoff slot `k` | Every contender that lost at exactly bit `k` |

The arbitration phase therefore has `2·(6−L)` slots in v0: 12 for `L = 0`, 2 for `L = 5`, none for `L = 6`. The slot layout depends only on `TARGET_LEN` of the request frame, never on the line state, which keeps slave logic simple: a slave starts at slot `2·L` of the 12-slot pattern.

For each bit position from `5−L` to LSB:

1. Each active contender reads its MCID bit `k`.
2. If the bit is `1`, it drives `DATA` low for the arbitration slot.
3. If the bit is `0`, it leaves `DATA` released and samples the bus.
4. A contender that sent `0` but samples `DATA` low has lost at bit `k`. It drives `DATA` low for handoff slot `k` and then stays passive for the rest of the arbitration.
5. All other contenders leave handoff slot `k` released and continue with the next bit.

Notes:

- Contenders of a masked request share the prefix bits, so nobody can lose within the prefix; those slots are omitted.
- A handoff slot after a released arbitration bit always stays released, since nobody can lose at that bit.

## 5. Winner Definition

- The contender still active after bit `0` is the winner. It sends its response frame directly after the arbitration. Its MCID is the request prefix followed by the bit sequence the master sampled in the arbitration slots.
- Whether a contender exists is shown by the first bit after the arbitration slots: every winner starts a frame (type codes start with `1`), so a released first bit means no contender. Low arbitration slots alone are not sufficient, because the open bits of the only contender may all be `0` (for example MCID `101000` answering a request with prefix `10100`).
- Two slaves with identical MCIDs cannot be distinguished during arbitration. Both win and both send their frame; the overlapping responses fail the CRC unless they are identical. Duplicate MCIDs are a configuration fault, detected by the master only through repeated CRC failures on responses (`PROTOCOL.md` section 3.5).

## 6. Master Behavior

### 6.1 Deriving Pending Masks

If handoff slot `k` was low, at least one matching slave lost at bit `k`. Its MCID lies in the group:

- prefix: winner bits `5..k+1`, followed by `0` at position `k`
- `TARGET_LEN = 6 - k`

The master pushes this prefix mask onto its pending mask stack.

### 6.2 Burst Algorithm

1. Start with an empty pending mask stack and send `REQUEST_DATA` with `TARGET_LEN=0`.
2. Run the arbitration. If the first bit after the arbitration slots is released, there is no contender: go to step 4.
3. Push one prefix mask for every low handoff slot, in order from bit `5` to bit `0`. Receive the winner's `SLAVE_DATA` frame, which follows in the same transaction.
4. If the stack is not empty, pop the top mask, send `REQUEST_DATA` with that mask, and go to step 2.
5. When the stack is empty, the burst is complete.

Masks from lower bit positions have longer prefixes and cover higher MCIDs. Pushing in order from bit `5` to bit `0` puts the highest-priority group on top, so last-in-first-out processing serves slaves in strictly descending MCID order.

The stack holds at most one mask per prefix length, so at most 6 entries in v0.

A popped mask with `TARGET_LEN=6` identifies exactly one MCID. The request has no arbitration slots and is a plain exactly addressed `REQUEST_DATA`.

The same algorithm with `REQUEST_INFO` enumerates all slaves on the bus (commissioning scan, `PROTOCOL.md` section 3.5): every slave contends, and the winner's `SLAVE_INFO` arrives in the same transaction. With `REQUEST_STATE` it reads the current state of every slave that has one.

### 6.3 Retry Policy

- If the response of a winner is missing or fails CRC validation, the master ends the transaction with RESET and may retry with an exactly addressed request up to a configured budget. The masks derived from the handoff slots stay valid.
- After the budget is exhausted, the master skips this slave for the current burst. The slave keeps its buffered data and pending flag and announces itself again via the idle event handshake.

## 7. Example

Pending slaves: 42 (`101010`), 40 (`101000`), 25 (`011001`), 15 (`001111`). The master runs a burst with group `REQUEST_DATA`.

### Request 1: `TARGET_LEN=0` (12 arbitration slots)

| Bit | 42 | 40 | 25 | 15 | Arbitration slot | Handoff slot |
|---:|:---:|:---:|:---:|:---:|---|---|
| 5 | 1 | 1 | 0 | 0 | low | low (25, 15 lost) → push `0` / len 1 |
| 4 | 0 | 0 | – | – | high | high |
| 3 | 1 | 1 | – | – | low | high |
| 2 | 0 | 0 | – | – | high | high |
| 1 | 1 | 0 | – | – | low | low (40 lost) → push `10100` / len 5 |
| 0 | 0 | – | – | – | high | high |

Winner: `101010` = 42. Stack (top first): `10100`/5, `0`/1. Slave 42 sends its `SLAVE_DATA` frame directly after handoff slot 0.

### Request 2: `10100` / len 5 (2 arbitration slots)

Only 40 contends. It has only bit 0 left to send, and that bit is `0`: arbitration slot and handoff slot stay high. The first bit after the arbitration slots is low, so there is a contender; its MCID is `10100` followed by `0` = 40. No low handoff slot. Slave 40 sends its frame. Stack: `0`/1.

### Request 3: `0` / len 1 (10 arbitration slots)

25 (`011001`) and 15 (`001111`) contend with bits 4 to 0. At bit 4, 25 sends `1` and 15 sends `0`; 15 loses and drives handoff slot 4 → push `00`/2. Winner `0` + `11001` = 25. Slave 25 sends its frame. Stack: `00`/2.

### Request 4: `00` / len 2 (8 arbitration slots)

Only 15 contends (bits 3 to 0 of `001111`). Winner `00` + `1111` = 15. Slave 15 sends its frame. Stack empty, burst complete.

Result: 4 pending slaves, 4 transactions, served in order 42, 40, 25, 15. The earlier scheme (poll, then `REQUEST_DATA`) needed 8 transactions, each with its own START, frame, turnaround and STOP. The arbitration slots drop from 4 × 12 = 48 to 12 + 2 + 10 + 8 = 32.

## 8. Timing Requirements (Symbolic)

- `T_ARB_SLOT`: one arbitration or handoff slot (one clock period).
- `T_DRIVE_SETUP`: minimum time before sample where contenders must drive.
- `T_SAMPLE`: master and slave sample point inside slot (rising `CLK` edge).
- `T_RELEASE`: release time before next slot.

All timing constants are defined by master and must fit ATtiny85 ISR and GPIO timing limits.

## 9. Fairness

Priority determines the order within a burst only. Every slave that is pending when a burst starts is served in that burst, because the master works through all derived masks before the burst ends.

A higher-priority slave that becomes pending again during a burst lies outside the remaining masks and is served in the next burst. Lower-priority slaves therefore cannot be starved by a single busy high-priority slave.

## 10. Accepted Rare Cases

- A handoff slot falsely read as low (noise) yields a mask without contender; the request is wasted, nothing else happens.
- A missed handoff slot leaves a slave out of the current burst; it announces itself again via the idle event handshake.
- A slave whose pending state changes between being masked and being requested is simply found or not found in the later request.
