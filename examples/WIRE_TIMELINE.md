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
3. Master starts a burst: empty pending mask stack, `REQUEST_DATA` with `TARGET_LEN=0` (group request).
4. Arbitration with handoff slots (12 slots, because the mask has no prefix bits):

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

5. In the same transaction 42 (the winner) sends its `SLAVE_DATA` frame directly after handoff slot 0. The master ends with STOP; 42 clears its buffer.
6. Master pops `10100` / len 5 and sends `REQUEST_DATA` for group `10100x` (MCIDs 40 and 41): 2 arbitration slots. 40 has only bit 0 left and it is `0`, so both slots stay high; the first response bit is low, so 40 is the contender and the winner `10100` + `0`. 40 sends its `SLAVE_DATA`.
7. Master pops `0` / len 1 and sends `REQUEST_DATA` for group `0xxxxx`: 10 arbitration slots. 25 and 15 contend; 15 loses at bit 4 and drives handoff slot 4. Master pushes `00` / len 2. Winner `0` + `11001` = 25; 25 sends its `SLAVE_DATA`.
8. Master pops `00` / len 2 and sends `REQUEST_DATA` for group `00xxxx`: 8 arbitration slots. 15 wins and sends its `SLAVE_DATA`.
9. Stack empty: burst complete, bus returns to idle.

## Notes

- Four pending slaves are collected with four transactions, in order 42, 40, 25, 15.
- Slaves without buffered data do not contend in a `REQUEST_DATA` and stay silent.
- Buffered status snapshots may be overwritten before retrieval.
- Each group `REQUEST_DATA` is one transaction: START · master frame · turnaround slot · `2·(6−TARGET_LEN)` arbitration slots · `SLAVE_DATA` of the winner · STOP (`docs/PROTOCOL.md` section 1.3). If the winner's frame is invalid, the master ends with RESET and retries that MCID with an exactly addressed `REQUEST_DATA`.
- Within the burst the master keeps `CLK` high after each STOP and starts the next transaction directly (`docs/PROTOCOL.md` section 1.2). It lowers `CLK` only after the final STOP; slaves that are still pending then announce themselves again.
