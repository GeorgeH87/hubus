# Typical Session Example

This example shows one burst with three slaves.

## Setup

- Slave A: MCID `0b011001` (25)
- Slave B: MCID `0b101010` (42)
- Slave C: MCID `0b001111` (15)

Slave A and Slave B have pending data. Slave C has no data.

Precondition: the master has completed its startup sequence (clock until `DATA` released, RESET). Every master frame below begins with a START condition and ends with a STOP condition.

## Sequence

1. Slave B gets a local event while the bus is idle and pulls `DATA` low.
2. Master detects low `DATA` in idle and sends confirmation ticks on `CLK`.
3. Slave B releases `DATA` after confirmation. The master starts a burst with an empty pending mask stack.
4. Master sends `REQUEST_DATA` with `TARGET_LEN=0` (all slaves).
5. A and B enter arbitration. C is not pending and stays passive.
6. Arbitration bit 5 (MSB):
   - A sends `0` (release), B sends `1` (pull low).
   - Line is low, so A has lost at bit 5.
7. Handoff slot 5: A pulls `DATA` low. The master pushes the mask `0` / `TARGET_LEN=1` onto its stack.
8. Bits 4..0: B is the only contender; all remaining handoff slots stay released. The master reads the winner `101010` = 42.
9. Slave B sends its `SLAVE_DATA` frame with valid CRC8 directly after the arbitration, in the same transaction. The master ends it with STOP.
10. Master pops mask `0` / `TARGET_LEN=1` and sends `REQUEST_DATA` for that group (10 arbitration slots).
11. Only A is pending in group `0xxxxx` and wins. No handoff slot is low. A sends its `SLAVE_DATA` in the same transaction.
12. The stack is empty. The burst is complete and the bus returns to idle.

## Why B Wins First

Higher MCID has higher priority, and bit `1` is dominant because it pulls the line low.

## Why A Is Not Forgotten

A lost at bit 5 and announced this in handoff slot 5. The master therefore knows that a pending slave exists in group `0xxxxx` and requests it before ending the burst.
