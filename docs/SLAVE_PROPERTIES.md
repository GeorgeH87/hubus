# Slave Base Properties

## 1. Mandatory Capabilities

A compliant HUBUS slave must provide:

- Unique MCID within the bus domain.
- Clock synchronization to master `CLK`.
- `DATA` line sensing at defined sample points (rising `CLK` edge).
- `DATA` line drive-low and release behavior; `DATA` is only changed while `CLK` is low.
- Detection of START, STOP, and RESET line conditions (`DATA` change while `CLK` is high).
- Idle event announce behavior by pulling `DATA` low until the first complete master clock pulse.
- Arbitration participation logic, including driving the handoff slot directly after losing.
- Target-mask match logic (`MCID` prefix + `TARGET_LEN`) for all master commands.
- Early target filtering based on `TARGET_LEN` before full command processing.
- Send a response frame only if addressed exactly (`TARGET_LEN = 6`) or after winning the arbitration of a group `REQUEST_STATE`/`REQUEST_INFO`/`REQUEST_DATA`. Never answer a group `SEND_COMMAND`; results of group commands are reported via the pending flag.
- Answer `REQUEST_STATE` only if the application provides a state; otherwise ignore it.
- Frame RX/TX handling with incremental CRC8 computation (byte by byte while receiving and transmitting).
- Transaction sequencing per `PROTOCOL.md` section 1.3 (turnaround slot, arbitration slots, response).

## 2. Slave State Machine

- `IDLE_LISTEN`: Wait for START. The pending flag indicates unread outgoing data.
- `IDLE_EVENT_SIGNAL`: Hold `DATA` low to announce a pending event.
- `RX_FRAME`: Receive the master frame after START; stop evaluating once the address does not match.
- `TURNAROUND`: Release `DATA` for one slot; evaluate CRC, address, and buffer state.
- `ARBITRATION_ACTIVE`: Contend with the open MCID bits (only if addressed by the group mask and able to answer: any slave for `REQUEST_INFO`, a slave with a state for `REQUEST_STATE`, a pending slave for `REQUEST_DATA`). The first arbitration slot is slot `2·TARGET_LEN` of the 12-slot pattern.
- `ARBITRATION_HANDOFF`: Lost at the current bit; drive `DATA` low for the following handoff slot.
- `ARBITRATION_DROPPED`: Passive until the transaction ends.
- `SELECTED_TRANSFER`: Transmit `SLAVE_INFO` or `SLAVE_DATA` after an exactly addressed request, or directly after the last arbitration slot as the winner of a group request.
- `IGNORE`: Not addressed, invalid frame, or nothing to send; passive until the next START, STOP, or RESET.

### Arbitration Rules

- For each bit position: drive `DATA` low in the arbitration slot if the own MCID bit is `1`; otherwise release and sample.
- Released but sampled low: lost at this bit. Drive `DATA` low in the immediately following handoff slot, then stay passive.
- Not lost: leave the handoff slot released and continue.
- After the last handoff slot the losers are passive. A slave that is still active (the winner) starts its response frame in the next slot, in the same transaction. Arbitration covers only the bits below the request prefix: `2·(6−TARGET_LEN)` slots; with `TARGET_LEN = 6` there are none and the slave answers directly after the turnaround slot.
- The slave neither knows nor tracks other slaves; deriving masks and scheduling is entirely master-side.

### Response Rules

- Only `REQUEST_INFO` and `REQUEST_DATA`, exactly addressed and with valid CRC, lead to `SELECTED_TRANSFER`.
- The response starts in the slot directly after the turnaround slot.
- With an empty buffer, a `REQUEST_DATA` leads to `IGNORE` (the line stays released, which the master reads as "no response").

There is no slave recovery state. On a CRC failure or non-matching frame the slave simply ignores the rest of the frame and waits for the next START.

### Transitions Driven by Line Conditions

| Condition | Slave action (from any state) |
|---|---|
| START | Release `DATA`, discard partial frame/arbitration/transmit state, start receiving at bit 0. |
| STOP | Release `DATA`, go to `IDLE_LISTEN`. If a complete `SLAVE_DATA` response was transmitted in this transaction, clear the buffer and the pending flag (`PROTOCOL.md` section 3.4). |
| RESET | Release `DATA`, reset communication stack, go to `IDLE_LISTEN`. Buffer, `GEN` counter, and pending flag are preserved (see `PROTOCOL.md` section 3.3). |

### Idle Event Signal Rules

- The slave enters `IDLE_EVENT_SIGNAL` and pulls `DATA` low in exactly two cases: (1) the buffer is written while no data was pending and the bus is idle with `CLK` low; (2) at the first falling `CLK` edge after a STOP or RESET, if data is still pending (the slave is "armed" by the STOP/RESET and disarmed by signaling or by a START). A slave therefore signals at most once per STOP (`PROTOCOL.md` section 3.1).
- After the first falling `CLK` edge (one complete clock pulse), the slave releases `DATA` and returns to `IDLE_LISTEN`. Releasing while `CLK` is high is not allowed, since it would look like a STOP.
- There is no local timeout: the slave holds `DATA` low until that clock pulse arrives.

## 3. MCID Requirements

- Valid MCIDs are `1..63` in v0. MCID `0` is reserved and must not be assigned.
- MCID must be immutable during normal runtime.
- MCID uniqueness is required for deterministic arbitration.
- Duplicate MCID is a configuration fault. It is detected by the master (commissioning scan, CRC failure heuristic); the slave implements no detection logic.
- The application bytes of `SLAVE_INFO` SHOULD contain a device-unique value (for example a serial number) to make duplicates reliably detectable.

## 4. Buffer and Payload Constraints

- Design for low RAM footprint.
- Exactly one outgoing buffer (`MESSAGE_FORMAT.md` section 4.2) and a receive buffer for command payloads.
- Both buffer sizes are compile-time options (maximum 31 bytes each). The ATtiny85 reference build uses the full 31 bytes; smaller devices may use shorter buffers (`HARDWARE_REQUIREMENTS.md` section 4.1).
- The accepted command payload size is announced in `SLAVE_INFO` byte 2. Longer commands are received to the end (to stay synchronized) and then ignored.
- Writing the outgoing buffer (event data or `STATUS_CAPTURE`) sets the pending flag.
- New content may overwrite unread content; the buffer is not a queue.
- Every buffer write increments the 2-bit `GEN` counter, which is transmitted in the `SLAVE_DATA` header (`MESSAGE_FORMAT.md` section 4.3).
- The application passes the `MORE` flag with each buffer write (fragmented transfers) and writes the next fragment only when the buffer is empty (`MESSAGE_FORMAT.md` section 6.1).
- Static device info for `REQUEST_INFO` (`MESSAGE_FORMAT.md` section 4.1) can be a constant in flash and needs no RAM.
- An optional state for `REQUEST_STATE` (`MESSAGE_FORMAT.md` section 4.4) is not copied: the application hands the library a pointer and a length, and the library reads the bytes while it transmits. It costs no buffer RAM.

## 4.1 Responses

- A slave only sends frames in response to `REQUEST_INFO` (`SLAVE_INFO`), `REQUEST_STATE` (`SLAVE_DATA` with the live state, only if the application provides one) and `REQUEST_DATA` (`SLAVE_DATA` with the buffer, only if the buffer is not empty), either exactly addressed or after winning the arbitration of a group request.
- Commands are executed without acknowledgement. No error frames exist; application errors are reported through the buffer.
- Unknown reserved opcodes (`0x01..0x0F`) are ignored.
- After transmitting a complete `SLAVE_DATA` response to `REQUEST_DATA`, the slave remembers this (one flag) until the transaction ends: STOP clears buffer and pending flag, RESET keeps both. A state frame (`REQUEST_STATE`) clears nothing.

## 5. No Timeouts on the Slave

A slave has no time base of its own and implements no timeouts, timers, or retry counters. Every state transition is caused by a `CLK` edge or a master line condition.

A slave must:

- Release `DATA` and resynchronize on every START.
- Release `DATA` on STOP and RESET.
- Finish any transmission within the bit count defined by the frame (it only advances on master clock edges, so it can never block the bus for longer than one frame of clock ticks).

If the master clock stops mid-transfer, the slave stays in its current state. The master resolves this with its startup/recovery sequence (`PROTOCOL.md` section 3.3). All timeout and fault handling is master-side.

## 6. Implementation Notes (All Devices)

- Avoid dynamic allocation.
- Keep ISR work minimal and deterministic.
- Protocol logic lives in a hardware-independent core shared by all devices (and by host-side tests). Each device family has a small HAL for pin access and interrupt setup (`HARDWARE_REQUIREMENTS.md` section 4.1).
- Both `CLK` and `DATA` raise pin change/edge interrupts. The handler evaluates the pin state: a `CLK` edge is a bit event; a `DATA` change while `CLK` is high is a START/STOP; a `DATA` change while `CLK` is low (including the slave's own writes) is ignored.
- On the ATtiny85 both pins share one pin change vector; the handler compares the pin state with the previous one to see which line changed. Other devices may use one vector per port or per pin; the HAL hides this.
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
