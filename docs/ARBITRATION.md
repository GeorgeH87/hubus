# Arbitration Specification

## 1. Purpose

Arbitration resolves simultaneous slave communication requests on a shared `DATA` line without active bus arbitration hardware. Combined with handoff slots, it also tells the master where in the MCID space further pending slaves are located, so a burst can serve all of them in priority order.

## 2. Fixed Semantics

- Logical `1`: slave actively pulls `DATA` low.
- Logical `0`: slave leaves `DATA` neutral (released/high).
- Priority: higher MCID wins.
- MCID `0` is reserved and never assigned to a slave. Every valid MCID (`1..63`) therefore contains at least one `1` bit, so any contender pulls `DATA` low at least once during arbitration.

This maps naturally to wired-AND behavior on the shared line because any active low dominates neutral.

## 3. Preconditions

- Master has issued `POLL_PENDING` with valid CRC. Only pending slaves matching its target mask (`TARGET_LEN` + `MCID` prefix) contend.
- Arbitration starts in the slot directly after the turnaround slot that follows the `POLL_PENDING` frame. After handoff slot `0` the master ends the transaction with STOP; all slaves, including the winner, are passive (`PROTOCOL.md` section 1.3).
- All contenders use the same MCID bit width for comparison.

## 4. Arbitration Sequence

Bit positions are numbered `5` (MSB) down to `0` (LSB) for the 6-bit v0 MCID. Each bit position consists of two clock slots:

| Slot | Who drives `DATA` low |
|---|---|
| Arbitration bit `k` | Every active contender whose MCID bit `k` is `1` |
| Handoff slot `k` | Every contender that lost at exactly bit `k` |

The full arbitration phase therefore has 12 slots in v0 (6 bits × 2). The slot layout is fixed and independent of the line state, which keeps slave logic simple.

For each bit position from MSB to LSB:

1. Each active contender reads its MCID bit `k`.
2. If the bit is `1`, it drives `DATA` low for the arbitration slot.
3. If the bit is `0`, it leaves `DATA` released and samples the bus.
4. A contender that sent `0` but samples `DATA` low has lost at bit `k`. It drives `DATA` low for handoff slot `k` and then stays passive for the rest of the arbitration.
5. All other contenders leave handoff slot `k` released and continue with the next bit.

Notes:

- Contenders in a prefix-masked poll share the prefix bits, so nobody loses within the prefix and the corresponding handoff slots stay released.
- A handoff slot after a released arbitration bit always stays released, since nobody can lose at that bit.

## 5. Winner Definition

- The contender still active after bit `0` is the winner. Its MCID equals the bit sequence the master sampled in the arbitration slots.
- If no arbitration slot was low, there was no contender in the polled group.
- Two slaves with identical MCIDs cannot be distinguished during arbitration. Duplicate MCIDs are a configuration fault, detected by the master only through repeated CRC failures on responses (`PROTOCOL.md` section 3.5).

## 6. Master Behavior

### 6.1 Deriving Pending Masks

If handoff slot `k` was low, at least one pending slave lost at bit `k`. Its MCID lies in the group:

- prefix: winner bits `5..k+1`, followed by `0` at position `k`
- `TARGET_LEN = 6 - k`

The master pushes this prefix mask onto its pending mask stack.

### 6.2 Burst Algorithm

1. Start with an empty pending mask stack and send `POLL_PENDING` with `TARGET_LEN=0`.
2. Run the arbitration. If there is no contender, go to step 5.
3. Push one prefix mask for every low handoff slot, in order from bit `5` to bit `0`. Serve the winner with exact addressing (`TARGET_LEN=6`).
4. If the stack is not empty, pop the top mask, send `POLL_PENDING` with that mask, and go to step 2.
5. When the stack is empty, the burst is complete.

Masks from lower bit positions have longer prefixes and cover higher MCIDs. Pushing in order from bit `5` to bit `0` puts the highest-priority group on top, so last-in-first-out processing serves slaves in strictly descending MCID order.

The stack holds at most one mask per prefix length, so at most 6 entries in v0.

A popped mask with `TARGET_LEN=6` identifies exactly one MCID. The master may request data from it directly without a poll.

### 6.3 Retry Policy

- If a request to a winner times out or fails CRC validation, the master may retry up to a configured budget.
- After the budget is exhausted, the master skips this slave for the current burst. The slave keeps its buffered data and pending flag and announces itself again via the idle event handshake.

## 7. Example

Pending slaves: 42 (`101010`), 40 (`101000`), 25 (`011001`), 15 (`001111`).

### Poll 1: `TARGET_LEN=0`

| Bit | 42 | 40 | 25 | 15 | Arbitration slot | Handoff slot |
|---:|:---:|:---:|:---:|:---:|---|---|
| 5 | 1 | 1 | 0 | 0 | low | low (25, 15 lost) → push `0` / len 1 |
| 4 | 0 | 0 | – | – | high | high |
| 3 | 1 | 1 | – | – | low | high |
| 2 | 0 | 0 | – | – | high | high |
| 1 | 1 | 0 | – | – | low | low (40 lost) → push `10100` / len 5 |
| 0 | 0 | – | – | – | high | high |

Winner: `101010` = 42. Stack (top first): `10100`/5, `0`/1. Master serves 42.

### Poll 2: `10100` / len 5

Only 40 contends. Winner 40, no low handoff slot. Master serves 40. Stack: `0`/1.

### Poll 3: `0` / len 1

25 (`011001`) and 15 (`001111`) contend. At bit 4, 25 sends `1` and 15 sends `0`; 15 loses and drives handoff slot 4 → push `00`/2. Winner 25. Master serves 25. Stack: `00`/2.

### Poll 4: `00` / len 2

Only 15 contends. Winner 15. Master serves 15. Stack empty, burst complete.

Result: 4 pending slaves, 4 polls, served in order 42, 40, 25, 15.

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

- A handoff slot falsely read as low (noise) yields a mask without contender; the poll is wasted, nothing else happens.
- A missed handoff slot leaves a slave out of the current burst; it announces itself again via the idle event handshake.
- A slave whose pending state changes between being masked and being polled is simply found or not found in the later poll.
