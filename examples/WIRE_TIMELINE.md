# Wire Timeline Example

This example shows a status-capture command followed by a burst that collects the captured data from four slaves.

## Assumptions

- Slaves with MCIDs 42 (`101010`), 40 (`101000`), 25 (`011001`), and 15 (`001111`) support status capture.
- Higher MCID has higher arbitration priority.
- Slaves without buffered data remain silent.
- Every master frame starts with START and ends with STOP.

## Timeline

1. Master sends `SEND_COMMAND` with opcode `STATUS_CAPTURE` (`0x00`) and `TARGET_LEN=0` (broadcast). No slave responds.
2. All four slaves write their current status into their slave buffer and set their pending flag.
3. Master starts a burst: empty pending mask stack, `POLL_PENDING` with `TARGET_LEN=0`.
4. Arbitration with handoff slots (12 slots):

   | Slot | `DATA` | Driven by | Master action |
   |---|---|---|---|
   | Bit 5 | low | 42, 40 | – |
   | Handoff 5 | low | 25, 15 (lost) | push `0` / len 1 |
   | Bit 4 | high | – | – |
   | Handoff 4 | high | – | – |
   | Bit 3 | low | 42, 40 | – |
   | Handoff 3 | high | – | – |
   | Bit 2 | high | – | – |
   | Handoff 2 | high | – | – |
   | Bit 1 | low | 42 | – |
   | Handoff 1 | low | 40 (lost) | push `10100` / len 5 |
   | Bit 0 | high | – | – |
   | Handoff 0 | high | – | winner = `101010` (42) |

5. Master sends `REQUEST_DATA` to 42; 42 responds with `SLAVE_DATA`.
6. Master pops `10100` / len 5 and polls group `10100x` (MCIDs 40 and 41). 40 wins without handoff. Master requests data from 40.
7. Master pops `0` / len 1 and polls group `0xxxxx`. 25 and 15 contend; 15 loses at bit 4 and drives handoff slot 4. Master pushes `00` / len 2. Winner 25; master requests data from 25.
8. Master pops `00` / len 2 and polls group `00xxxx`. 15 wins. Master requests data from 15.
9. Stack empty: burst complete, bus returns to idle.

## Notes

- Four pending slaves are collected with four polls, in order 42, 40, 25, 15.
- Slaves without buffered data ignore `POLL_PENDING` and `REQUEST_DATA` silently.
- Buffered status snapshots may be overwritten before retrieval.
- Each `POLL_PENDING` transaction is: START · master frame · turnaround slot · 12 arbitration slots · STOP. Each `REQUEST_DATA` is a separate transaction: START · master frame · turnaround slot · `SLAVE_DATA` · STOP (`docs/PROTOCOL.md` section 1.3).
