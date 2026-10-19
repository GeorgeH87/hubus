# Arbitration Specification

## 1. Purpose

Arbitration resolves simultaneous slave communication requests on a shared `DATA` line without active bus arbitration hardware.

## 2. Fixed Semantics

- Logical `1`: slave actively pulls `DATA` low.
- Logical `0`: slave leaves `DATA` neutral (released/high).
- Priority: higher MCID wins.

This maps naturally to wired-AND behavior on the shared line because any active low dominates neutral.

## 3. Preconditions

- Master has issued a poll-like intent that allows slaves with pending data to contend.
- All contenders know arbitration start timing from the master clock.
- All contenders use the same MCID bit width for comparison.

## 4. Bit-by-Bit Arbitration Algorithm

For each arbitration bit from MSB to LSB:

1. Each active contender reads its current MCID bit.
2. If bit is `1`, it drives `DATA` low for the bit window.
3. If bit is `0`, it leaves `DATA` released and samples the bus.
4. A contender that sent `0` but samples `DATA` low must drop out immediately.
5. Contenders that drove `1` remain active.
6. Continue with remaining contenders until one remains or all bits are consumed.

## 5. Winner Definition

- The last remaining active contender is the winner.
- If multiple contenders remain equal through all bits, MCID uniqueness is violated and master must enter recovery.

## 6. Master Behavior

- Master samples each arbitration bit window.
- Master determines winner MCID from sampled bit stream.
- Master issues follow-up request to winner.
- If a losing slave emits the handoff slot marker, the master keeps the current burst open and continues polling for the next highest-priority pending slave.
- The master maintains an internal pending MCID mask and clears entries as slaves are served or time out.
- The next follow-up request is selected from that mask in MCID priority order.

### Pending MCID Mask Rules

- The master sets a mask bit when it observes a slave event announcement, a successful arbitration contender, or any other valid pending indication for that MCID.
- The master clears a mask bit after that MCID has been successfully served.
- If a request to an MCID times out or fails CRC validation repeatedly, the master clears the bit after the configured retry budget is exhausted.
- The mask is only a scheduling aid; it does not replace arbitration or payload validation.

If no valid contender remains, master shall treat cycle as "no pending data" or "arbitration failure" based on timeout policy.

## 6.1 Handoff Marker

The handoff marker is a single predefined slot used by a losing slave to indicate that at least one other slave still has pending data.

It does not change the winner of the current arbitration round. It only tells the master to keep the current burst open when more data can be served immediately.

The handoff marker is interpreted by the master as a continuation marker for its internal pending MCID mask.

### Handoff Slot Semantics

- The handoff slot is emitted by a slave only when it is about to release `DATA` after losing arbitration or after completing a response.
- The slot means: "I am done, but the bus should stay in active polling mode because another MCID is still pending."
- The slot does not identify the next MCID; it only tells the master not to fall back to idle yet.
- The master still selects the next MCID from its internal pending mask in priority order.

## 7. Timing Requirements (Symbolic)

- `T_ARB_SLOT`: one arbitration bit slot.
- `T_DRIVE_SETUP`: minimum time before sample where contenders must drive.
- `T_SAMPLE`: master and slave sample point inside slot.
- `T_RELEASE`: release time before next slot.

All timing constants are defined by master and must fit ATtiny85 ISR and GPIO timing limits.

## 8. Fairness Note

Strict priority can starve low MCID nodes in heavy traffic. That is accepted in v0.

The master may therefore observe the same higher-priority MCID repeatedly and continue serving it before lower-priority nodes.
