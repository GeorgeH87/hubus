# Slave Base Properties

## 1. Mandatory Capabilities

A compliant HUBUS slave must provide:

- Unique MCID within the bus domain.
- Clock synchronization to master `CLK`.
- `DATA` line sensing at defined sample points.
- `DATA` line drive-low and release behavior.
- Idle event announce behavior by pulling `DATA` low until master confirmation ticks are observed.
- Arbitration participation logic.
- Target-mask match logic (`MCID` prefix + `TARGET_LEN`) for addressed master commands.
- Early target filtering based on `TARGET_LEN` before full command processing.
- Frame RX/TX handling with CRC8 validation.

## 2. Slave State Machine

- `IDLE_LISTEN`: Wait for master frame start.
- `IDLE_EVENT_SIGNAL`: Hold `DATA` low to announce a pending event.
- `INTENT_READY`: Internal flag indicates pending outbound data.
- `ARBITRATION_ACTIVE`: Contend with MCID bits.
- `ARBITRATION_DROPPED`: Lost arbitration and return to passive listen.
- `SELECTED_TRANSFER`: Respond to master request.
- `RECOVERY_WAIT`: Enter after timeout, CRC failure, or protocol violation.

### Idle Event Signal Rules

- When an event is pending during idle, slave enters `IDLE_EVENT_SIGNAL` and pulls `DATA` low.
- Slave monitors `CLK` and counts master confirmation ticks.
- After receiving confirmation ticks, slave releases `DATA` and returns to `IDLE_LISTEN`.
- If confirmation does not arrive before a local timeout, slave releases `DATA` and retries in a later idle window.

## 3. MCID Requirements

- MCID must be immutable during normal runtime.
- MCID uniqueness is required for deterministic arbitration.
- Duplicate MCID is a configuration fault.

## 4. Buffer and Payload Constraints

- Design for low RAM footprint.
- Support at least one complete incoming frame and one outgoing frame buffer.
- Enforce maximum payload length from `docs/MESSAGE_FORMAT.md`.
- If a new status value arrives before the previous buffered value was read, the slave may overwrite the older value.
- The buffer is not required to behave like a queue.
- An optional generation counter MAY be used to make overwritten snapshots observable to the master.

## 5. Timeout and Recovery Behavior

A slave must:

- Abort transfer if master clock disappears beyond configured threshold.
- Release `DATA` immediately on dropout or fault.
- Release `DATA` if idle event signal exceeds local timeout without confirmation.
- Return to `IDLE_LISTEN` after recovery timeout.

## 6. ATtiny85 Practical Notes

- Avoid dynamic allocation.
- Keep ISR work minimal and deterministic.
- Precompute MCID bit masks for arbitration speed.

## 7. Example Slave Categories

The following categories are reference examples for protocol usage:

- Relay or LED actuator slave:
	- Accepts master command frames to switch outputs on or off.
	- Exposes current output state via readable status/data response.
- Switch input slave:
	- Signals local actuation events through idle event signaling (`DATA` low until master confirmation ticks).
	- Returns current switch state when master requests it.
	- Master may read switch state at any time, not only after events.
- Temperature sensor slave:
	- Responds to master polling and returns current measurement data.

These categories are non-mandatory examples and not fixed implementation targets.
