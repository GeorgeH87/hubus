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

- Master triggers line-fault timeout.
- Master enters `RECOVERY` and pauses normal transfers.
- Slaves must release `DATA` and reset local communication state.

## 3. Duplicate MCID

### Condition

Two slaves share same MCID and remain identical through arbitration.

### Expected Behavior

- Master detects unresolved arbitration ambiguity.
- Master raises configuration fault.
- Deployment must correct MCID assignment before normal operation.

## 4. Missing Slave Response

### Condition

Master sends `REQUEST_DATA` to winner MCID, but no valid frame arrives before timeout.

### Expected Behavior

- Master aborts request for this cycle.
- Master records timeout event.
- Master may retry based on application policy.

## 5. CRC Failure on Received Frame

### Condition

Slave or master receives frame with invalid CRC8.

### Expected Behavior

- Receiver discards frame.
- Receiver may emit error counter update.
- Communication resumes from next master-driven cycle.

## 6. Clock Loss Mid-Transfer

### Condition

Master clock halts while slave is in transfer state.

### Expected Behavior

- Slave times out, releases `DATA`, and returns to passive listen.
- Master reinitializes protocol state after clock restoration.
