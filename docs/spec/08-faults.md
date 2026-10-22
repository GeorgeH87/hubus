# 8. Faults and Accepted Rare Cases

## 8.1 Principle

- The master detects every fault and recovers from it ([§6.6](06-operation.md#66-startup-shutdown-and-recovery)); slaves have no timeouts and no recovery state ([§7.6](07-slave.md#76-no-timeouts)).
- Every START resynchronizes all slaves ([§2.5](02-line-conditions.md#25-resynchronization)), and a frame with an invalid CRC is always discarded ([§4.12](04-frames.md#412-integrity-check-crc8)).
- Corner cases that can only occur rarely and recover at the next master cycle are **accepted and documented** rather than prevented with additional slave logic. Sections 8.3 to 8.9 list them.

## 8.2 Fault Scenarios

| Condition | What happens on the bus | Master reaction |
|---|---|---|
| No slave answers a request | The first bit after the turnaround or arbitration slots stays high ([§3.4](03-transactions.md#34-response-detection)). | Ends the transaction. For a group request it means the group has nobody who can answer; the master continues with the next mask. |
| The response of a slave is missing, incomplete, or has an invalid CRC | The master cannot accept the frame; the slave may have sent part of it. | Ends with RESET, so the slave keeps its buffer and pending flag ([§3.6](03-transactions.md#36-transaction-end-and-buffer-clearing)), and retries with an exact request within its retry budget; then skips the slave for this burst. The slave announces itself again later. |
| A slave receives a frame with an invalid CRC | – | Nothing: the slave ignores the frame. If it was a request, the master sees no response (above). |
| A slave misses or double-counts a `CLK` edge | Its bit position no longer matches; the frame fails the address match or the CRC in that slave. | Nothing for a received frame. A response becomes invalid or missing (above). The next START resynchronizes the slave. |
| `DATA` stays low while the bus should be idle | First indistinguishable from event signaling ([§6.1](06-operation.md#61-idle-event-handshake)). | Clocks for at most the line-fault threshold: a slave that is still transmitting or arbitrating finishes within one frame, a signaling slave releases on the first pulse. If `DATA` is released: RESET and continue. If not: hardware line fault, normal transfers pause, startup is retried later ([§6.6](06-operation.md#66-startup-shutdown-and-recovery)). |
| The master stops clocking mid-transaction | [§8.9](#89-master-stops-mid-transaction) | Startup sequence on restart ([§6.6](06-operation.md#66-startup-shutdown-and-recovery)). |
| Two slaves share an MCID | Both execute the same commands; in an arbitration both win and both send. Differing frames overlap and fail the CRC every time. | Commissioning scan and runtime heuristic report a suspected duplicate MCID ([§6.4](06-operation.md#64-commissioning-scan-and-duplicate-mcids)); the deployment must fix the assignment. Blind spots: [§8.7](#87-duplicate-mcid-blind-spots). |

## 8.3 Event Signal Races With START

A slave decides to signal an event while `CLK` is low, but the master raises `CLK` for a START before the slave's pull takes effect. The other slaves then see `DATA` fall while `CLK` is high and take it as a START.

- Effect: the signaling slave takes the first clock pulse of the frame as the confirmation and releases `DATA`; the frame is corrupted and fails the address match or the CRC in every slave.
- Recovery: the master sees `DATA` low right after raising `CLK` for the START, or detects the corruption by reading back `DATA` while it transmits, or finds the response missing. It runs the recovery sequence ([§6.6](06-operation.md#66-startup-shutdown-and-recovery)) and repeats the transaction ([§6.1](06-operation.md#61-idle-event-handshake)). The next START resynchronizes everybody.

## 8.4 Lost STOP

The master received a valid `SLAVE_DATA` frame and sent STOP, but the slave did not see the STOP (noise).

- The slave keeps its data and announces it again, so the master receives it a second time.
- `GEN` and payload are unchanged, so the master recognizes the duplicate and discards it ([§4.9](04-frames.md#49-generation-counter-gen)).

## 8.5 Handoff Slot Errors

- A handoff slot falsely read as low (noise) creates a mask without a contender: one request is wasted.
- A missed handoff slot leaves a slave out of the current burst. Its pending flag stays set, and it announces itself again after the burst ([§6.1](06-operation.md#61-idle-event-handshake)).
- A slave whose pending state changes between being masked and being requested is simply found or not found by the later request.

## 8.6 Mixed State Bytes

The state is read from the application while it is transmitted ([§4.10](04-frames.md#410-state)). If the application changes a byte during the transmission, the frame may combine old and new bytes; the CRC covers what was sent, so the frame is valid. A multi-byte value can be wrong once (for example when its high byte changes); the next read is correct. Applications that cannot accept this keep the state within one byte.

## 8.7 Duplicate MCID Blind Spots

Without a device-unique value in `SLAVE_INFO`, a duplicate MCID stays unnoticed as long as only one of the two slaves has data to deliver, or their frames are identical. Identical frames overlap into one valid frame. Commands addressed to that MCID always reach both slaves.

## 8.8 Commands Without Readback

A slave with neither a state nor an event buffer ([§7.4](07-slave.md#74-optional-parts)) cannot be read back. The master reports the state it commanded; a command that did not arrive or was not executed stays unnoticed. This is accepted for such devices; where it matters, the slave exposes its state.

## 8.9 Master Stops Mid-Transaction

If the master stops clocking in the middle of a transaction, for example because its process crashes, the slaves stay in their current state, possibly holding `DATA` low. The bus is blocked until the master restarts; its startup sequence releases the line, generates RESET, and collects the buffered data, which survives the reset ([§6.6](06-operation.md#66-startup-shutdown-and-recovery)).
