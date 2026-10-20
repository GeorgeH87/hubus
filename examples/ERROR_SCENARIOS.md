# Error Scenarios and Recovery

## 1. No Slave Responds to Poll

### Condition

Master sends `POLL_PENDING` and sees no arbitration activity.

### Expected Behavior

- Master marks cycle as "no pending data".
- Master continues with next scheduled poll.

## 2. Data Line Stuck Low

### Condition

`DATA` remains low across idle windows.

### Expected Behavior

- Master first treats low `DATA` as a possible idle event signal and sends confirmation ticks.
- Master keeps clocking for up to its line-fault threshold (at least one maximum frame length in clock ticks). Any slave still transmitting or arbitrating releases `DATA` within that time, since slaves only advance on master clock edges.
- If `DATA` is released: master generates RESET and resumes normal operation.
- If `DATA` stays low: master enters `RECOVERY`, reports a hardware line fault, and pauses normal transfers.
- Slaves need no timeout for this; they are released by the clock ticks or by START/STOP/RESET.

## 3. Duplicate MCID

### Condition

Two slaves share the same MCID (configuration fault).

### What Happens on the Bus

- Arbitration: both send identical bits and never lose against each other; the master sees one winner and no handoff. Not detectable.
- `REQUEST_DATA` while only one of them has data: works normally. Not detectable.
- `REQUEST_DATA` while both have data: both frames overlap on the wired-AND line; the payloads differ, so the CRC8 is practically always invalid. Every retry fails the same way.
- `REQUEST_INFO`: identical responses (same device, same firmware) overlap into a valid frame; different responses fail CRC.
- `SEND_COMMAND`: both slaves execute it.

### Expected Behavior

- Master raises "suspected duplicate MCID" when the same MCID fails CRC repeatedly while other MCIDs respond cleanly (runtime heuristic, `docs/PROTOCOL.md` section 3.5).
- The commissioning scan (`REQUEST_INFO` to every MCID) reports CRC failures for that MCID. With a device-unique value in `SLAVE_INFO`, detection at the scan is reliable.
- Deployment must correct the MCID assignment. Slaves implement no detection logic.

## 4. Missing Slave Response

### Condition

Master sends `REQUEST_DATA` to winner MCID, but no valid frame arrives before timeout.

### Expected Behavior

- Master ends the transaction with RESET (not STOP), so a slave that may have sent a partial response keeps its buffer and pending flag.
- Master records the timeout event.
- Master may retry `REQUEST_DATA` within its retry budget; afterwards it skips the slave for the current burst.

## 5. CRC Failure on Received Frame

### Condition

Slave or master receives frame with invalid CRC8.

### Expected Behavior

- Receiver discards frame.
- A slave ignores the rest of the frame and waits for the next START.
- If the master receives an invalid `SLAVE_DATA` frame, it ends the transaction with RESET instead of STOP. The slave keeps its buffer and pending flag, and the master retries within its budget.
- Communication resumes from next master-driven cycle.

## 5.1 STOP Lost After a Valid Response

### Condition

The master received a valid `SLAVE_DATA` frame and sent STOP, but the slave did not detect the STOP (noise).

### Expected Behavior

- Accepted rare case (see `docs/PROTOCOL.md` section 3.4).
- The slave keeps its data and announces it again; the master receives it a second time.
- The `GEN` counter in the slave header is unchanged and the payload is identical, so the master detects and discards the duplicate.

## 6. Clock Loss Mid-Transfer

### Condition

Master clock halts while slave is in transfer state (for example master process crash or restart).

### Expected Behavior

- Slave has no timeout and stays in its current state, possibly holding `DATA` low.
- On restart, the master runs its startup sequence: clock until `DATA` is released, generate RESET, then run a full `POLL_PENDING` burst.
- Buffered slave data survives the RESET and is collected in that burst.

## 7. Slave Loses Bit Synchronization

### Condition

A slave misses or double-counts a `CLK` edge (for example due to noise or ISR latency) and its bit position no longer matches the frame.

### Expected Behavior

- The affected frame fails address matching or CRC in that slave and is ignored.
- The next START resynchronizes the slave; no explicit recovery action is needed.
- If the slave was transmitting, the master sees a CRC failure or missing response and retries per its policy.

## 8. Event Signal Races With START

### Condition

A slave starts idle event signaling at the same moment the master raises `CLK` for a START.

### Expected Behavior

- Accepted rare case (see `docs/PROTOCOL.md` section 7.1).
- The frame is corrupted and discarded; master retries and the next START resynchronizes all slaves.
