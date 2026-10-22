# Message Format

## 1. Goals

- Keep parser small for ATtiny85.
- Keep control messages short.
- Keep payloads small by default but extensible across multiple frames.
- Prioritize integrity checks over transfer speed.

## 2. Frame Structure (v0)

A frame consists of a 16-bit header, an optional payload, and a CRC8. The header layout is identical in both directions.

```
Bit:     15 14 | 13 12 11   | 10  9  8  7  6  5 |  4  3  2  1  0
Master:  TYPE  | TARGET_LEN |       MCID        |      LEN
Slave:   TYPE  |MORE| GEN  |       MCID        |      LEN
         ------- Byte 0 --------------|-------- Byte 1 ----------

Frame:   [Byte 0] [Byte 1] [PAYLOAD: LEN bytes] [CRC8]
```

| Field | Bits | Master → slave | Slave → master |
|---|---:|---|---|
| TYPE | 2 | Master message type (section 3) | Slave message type (section 4) |
| TARGET_LEN / MORE + GEN | 3 | Number of significant target prefix bits (`0..6`) | Bit 13: `MORE` (more fragments follow, section 6.1); bits 12–11: `GEN` (2-bit generation counter, section 4.3) |
| MCID | 6 | Target prefix (MSB-aligned) | Source MCID |
| LEN | 5 | Payload length in bytes (`0..31`) | Payload length in bytes (`0..31`) |
| PAYLOAD | `LEN` × 8 | Message data | Message data |
| CRC8 | 8 | Over byte 0, byte 1, and payload | Over byte 0, byte 1, and payload |

Properties:

- The header is exactly two bytes, so payload and CRC8 are byte-aligned without padding.
- One parser and one CRC routine serve both directions.
- Frame length is `3 + LEN` bytes: minimum 24 bits, maximum 34 bytes.
- The frame direction is always known from the transaction sequence (`PROTOCOL.md` section 1.3), so 2-bit type codes suffice per direction.
- There is no start-of-frame field. Every master frame begins after a START line condition, which defines the frame boundary. Protocol versioning is handled by `SLAVE_INFO` (section 4.1).
- In slave frames, the three bits at the `TARGET_LEN` position carry `MORE` and `GEN`, so multi-frame signaling and duplicate detection cost no payload. In `SLAVE_INFO` they are always `000`.

## 3. Master Message Types

| Type | Name | Addressing | Slave reaction |
|---|---|---|---|
| `00` | `REQUEST_STATE` | exact or group | Read the **current state**, at any time, independent of events. Exact: the addressed slave sends its state as `SLAVE_DATA` (section 4.4); silent if it has no state. Group: every matching slave **that has a state** arbitrates and the winner sends its state in the same transaction. Nothing is cleared. |
| `01` | `REQUEST_INFO` | exact or group | Exact: the addressed slave sends `SLAVE_INFO` (static device metadata, section 4.1). Group: the matching slaves arbitrate (`ARBITRATION.md`) and the winner sends its `SLAVE_INFO` in the same transaction. |
| `10` | `REQUEST_DATA` | exact or group | Read the **event buffer** (a state change or a message the slave wants to deliver). Exact: the addressed slave sends `SLAVE_DATA` with the content of its buffer; silent if the buffer is empty. Group: only matching slaves **with pending data** arbitrate; all others drop out at once. The winner sends its `SLAVE_DATA` in the same transaction. |
| `11` | `SEND_COMMAND` | exact or group | Execute the command. **Never a response**, regardless of addressing. |

`REQUEST_STATE`, `REQUEST_INFO`, and `REQUEST_DATA` carry `LEN = 0` in v0; other values are reserved. Slaves still receive all `LEN` payload bytes and the CRC8 so they know where the frame ends and the turnaround slot begins. A v0 slave includes such payload bytes in the CRC check, otherwise ignores them, and answers as if `LEN` were `0`.

All master type codes are in use. Future protocol functions are added as reserved `SEND_COMMAND` opcodes (section 3.0), not as new types.

`REQUEST_STATE` and `REQUEST_DATA` answer two different questions:

| | `REQUEST_STATE` | `REQUEST_DATA` |
|---|---|---|
| Question | What is your state now? | Do you have something to deliver? |
| Source | The live state the application exposes (for example the level of every input of a multi-button slave) | The event buffer, filled by the application on a state change or by `STATUS_CAPTURE` |
| Who contends in a group request | every matching slave that has a state | only matching slaves with pending data |
| Effect | none, repeatable at any time | the STOP after a complete response clears buffer and pending flag |

`SEND_COMMAND` is always fire-and-forget. If the master needs to confirm that a state was established, it reads the state back with `REQUEST_STATE`.

## 3.0 Command Opcodes

The first payload byte of `SEND_COMMAND` is the opcode. A `SEND_COMMAND` with `LEN=0` is ignored.

| Opcode | Meaning |
|---|---|
| `0x00` | `STATUS_CAPTURE`: application writes its current status into the slave buffer and the slave sets its pending flag. |
| `0x01..0x0F` | Reserved for protocol use. Slaves ignore unknown reserved opcodes. |
| `0x10..0xFF` | Application-defined (for example relay on/off). |

Remaining payload bytes after the opcode are opcode-specific.

A slave ignores a `SEND_COMMAND` whose payload exceeds its configured maximum (announced in `SLAVE_INFO` byte 2). It still receives the complete frame so it stays synchronized. The master must not send larger commands than a slave accepts.

## 3.1 Unified Target Mask Addressing

All master-to-slave commands (`REQUEST_STATE`, `REQUEST_INFO`, `REQUEST_DATA`, `SEND_COMMAND`) use the same target-mask rule. For a group `REQUEST_STATE`/`REQUEST_INFO`/`REQUEST_DATA`, the mask selects which slaves contend in the arbitration; the master uses this to request the prefix groups derived from handoff slots.

The frame carries:

- `MCID`: target prefix bits (MSB-aligned).
- `TARGET_LEN`: number of significant prefix bits.

Transmission order for addressed master frames is fixed: `TYPE` -> `TARGET_LEN` -> `MCID` -> `LEN` -> `PAYLOAD` -> `CRC8`.

Match rule:

- A slave is addressed when its MCID starts with the first `TARGET_LEN` bits of the command `MCID` field.
- Lower bits beyond `TARGET_LEN` are ignored for matching.

Exact single-slave addressing is just a special case:

- Set `TARGET_LEN = 6` (full v0 MCID width).
- Set `MCID` to the exact destination MCID.

Group addressing is the same mechanism with a shorter prefix:

- Example: `TARGET_LEN = 4`, `MCID = 1101xx` addresses all slaves whose MCID begins with `1101`.

For `STATUS_CAPTURE`, this same unified mask addressing is used.

### Direction Rule

Whenever a response is expected, at most one slave answers:

- `SEND_COMMAND` never produces a response. Sent to a group, it is executed by every matching slave (as for exact addressing).
- `REQUEST_STATE`, `REQUEST_INFO`, and `REQUEST_DATA` with `TARGET_LEN = 6` are answered by the addressed slave alone.
- The same requests with `TARGET_LEN < 6` are answered by the arbitration winner alone. The arbitration is part of the same transaction (`ARBITRATION.md`, `PROTOCOL.md` section 1.3). Slaves that cannot answer do not contend: for `REQUEST_DATA` those are slaves without pending data, for `REQUEST_STATE` slaves without a state.
- If a group command produces something to report (for example a `STATUS_CAPTURE` result), the slave sets its pending flag and uses the regular return path: idle event handshake and burst (`PROTOCOL.md` section 3.2).

A slave therefore sends a response frame only if it was addressed exactly or won the arbitration, so at most one slave transmits a frame at any time (except for a duplicate MCID, which is a configuration fault, `PROTOCOL.md` section 3.5).

Valid range in v0: `0..6`.

- `0`: broadcast to all slaves.
- `1..5`: prefix group addressing.
- `6`: exact MCID addressing.

## 4. Slave Message Types

| Type | Name | Sent in response to |
|---|---|---|
| `00` | reserved | – |
| `01` | reserved | – |
| `10` | `SLAVE_DATA` | `REQUEST_DATA` (event buffer), `REQUEST_STATE` (current state, section 4.4) |
| `11` | `SLAVE_INFO` | `REQUEST_INFO` |

Slave type codes must start with `1`; codes `00` and `01` are never used by slaves. The master uses this to detect a missing response from the first bit after the turnaround slot, or after the last arbitration slot of a group request (`PROTOCOL.md` section 1.3).

Slaves send no acknowledgements and no error frames. Rationale: commands are fire-and-forget, so there is no request an acknowledgement or error could answer. A slave that needs to report an error (for example an application fault) writes it into its buffer and uses the regular return path; the error format is application-defined.

### 4.1 Slave Info (`SLAVE_INFO`)

Static metadata, typically a constant in flash.

| Byte | Content |
|---|---|
| 0 | HUBUS protocol version |
| 1 | Capability flags: bit 0 = supports `STATUS_CAPTURE`; bit 1 = has a state and answers `REQUEST_STATE`; bit 2 = has an event buffer (announces events, answers `REQUEST_DATA`); other bits reserved (0). Bit 0 requires bit 2. |
| 2 | Maximum command payload the slave accepts, in bytes (`1..31`; ATtiny85 reference build: 31) |
| 3.. | Application-defined (for example device type, firmware version). SHOULD include a device-unique value (for example a serial number) so the master's commissioning scan reliably detects duplicate MCIDs (`PROTOCOL.md` section 3.5). |

### 4.2 Slave Buffer and `SLAVE_DATA`

A slave with an event buffer has exactly one buffer for outgoing data. The event buffer is optional (`SLAVE_PROPERTIES.md` section 4.2): a slave without it never announces anything, never answers `REQUEST_DATA` (like a slave whose buffer is empty), and does not support `STATUS_CAPTURE`. It clears capability bits 0 and 2.

- The application writes event data into it (for example a switch change), or the slave fills it on `STATUS_CAPTURE` with the current status provided by the application.
- Writing the buffer sets the pending flag.
- The buffer is overwriteable by design and has no queue semantics. A status capture may overwrite unread event data and vice versa; this is accepted.
- `REQUEST_DATA` returns the buffer content as `SLAVE_DATA`. If the buffer is empty, the slave stays silent; no negative acknowledgement is sent.
- Buffer and pending flag are cleared on the STOP following a complete `SLAVE_DATA` response. If the master ends the transaction with RESET instead (missing, incomplete, or invalid response), both are preserved for a retry (`PROTOCOL.md` section 3.4).

When writing the buffer, the application also states whether more fragments follow (`MORE` bit, section 6.1).

### 4.3 Generation Counter (`GEN`)

`GEN` is a mandatory 2-bit counter in the slave header of every `SLAVE_DATA` frame.

- The slave library increments `GEN` (modulo 4) on every buffer write: event data or `STATUS_CAPTURE`.
- `GEN` is preserved across RESET.
- `SLAVE_INFO` frames and state frames (section 4.4) carry `GEN = 00`.

Master evaluation (per MCID, master-side only):

| New `GEN` vs. last received | Meaning |
|---|---|
| last + 1 | Normal: next buffer content. |
| equal, payload identical | Duplicate (for example after a lost STOP); discard. |
| equal, payload different | New content; the counter wrapped after four writes. |
| any other step | At least one buffer content was overwritten unread (information, not an error). |
| first frame from this MCID since master start | Taken as baseline. |

The payload comparison prevents new content from being discarded as a duplicate after a counter wrap. The only remaining ambiguity (wrap **and** identical payload) is harmless, since the data is the same.

### 4.4 Slave State (`REQUEST_STATE`)

A slave may expose a **state**: a block of 1 to 31 bytes that the application keeps up to date (for example one bit per button, or a measured value). The slave library reads the bytes directly from the application when it transmits; no copy is made and the buffer of section 4.2 is not touched.

- `REQUEST_STATE` is answered with a frame of type `SLAVE_DATA` (`10`), `MORE = 0`, `GEN = 00`, and the state bytes as payload. The master knows from its request that this is a state frame, not event data.
- It can be sent at any time and as often as needed. It neither clears nor sets the pending flag and does not change `GEN`.
- A slave without a state ignores `REQUEST_STATE`. It announces the capability in `SLAVE_INFO` (byte 1, bit 1), so the master knows which slaves to ask.
- The state is not latched. If the application changes a byte while it is being transmitted, the frame may mix old and new bytes of the application's state; the CRC is computed over what is sent, so the frame stays valid. An application that needs a consistent snapshot over several bytes keeps the state within one byte or updates it atomically.
- State and events complement each other. A multi-button slave exposes the current level of all buttons as state and writes a button press as an event into the buffer: the master collects the event in a burst and may read the state at any time, for example to verify it or after a start.
- A group `REQUEST_STATE` (for example `TARGET_LEN = 0`) enumerates the states of all slaves, one transaction per slave, with the same mask stack as a burst (`ARBITRATION.md` section 6.2).

## 5. Addressing Rules

- `MCID` identifies slave nodes.
- During master-to-slave frames, `TARGET_LEN` is interpreted first, then `MCID` is evaluated as target prefix.
- During slave-to-master frames, `MCID` is the source.

- MCID `0` is reserved and never assigned to a slave. Valid slave MCIDs are `1..63` in v0. (Reason: a slave with MCID `0` would send only released bits in arbitration and be indistinguishable from "no contender".) A frame addressed with `TARGET_LEN=6` and `MCID=0` matches no slave.

The master-side pending mask stack is internal scheduling state and is not transmitted as a separate frame field in v0.

Handoff slots are part of the arbitration phase only; they do not add a frame field or change the frame layout.

START, STOP, and RESET are line conditions, not frame content. RESET in particular is never encoded as a frame, so it works even when a slave has lost bit synchronization.

## 6. Encoding Rules

- Bytes are transmitted in order: byte 0, byte 1, payload, CRC8.
- Within each byte, bits are transmitted MSB first. The first bit after START is bit 15 of the header (MSB of byte 0).
- Multi-bit fields are therefore also transmitted MSB first.
- Arbitration MCID order: MSB first.
- `LEN=0` is valid.
- Maximum payload in v0 is 31 bytes.

## 6.1 Payload Extension Strategy

- v0 keeps each frame small (`LEN <= 31`) to remain lightweight.
- Larger logical datasets (for example weather records) are transferred as a sequence of `SLAVE_DATA` fragments through the single slave buffer.
- Reassembly policy is handled at application level on the master, not by adding transport state to tiny slaves.

Fragment sequence:

1. The slave application writes fragment 1 with `MORE = 1`.
2. The master reads it; the STOP clears the buffer.
3. The slave application sees the empty buffer and writes the next fragment (setting the pending flag again).
4. Repeat until a fragment with `MORE = 0` (the last one).

Rules:

- The slave application writes the next fragment only when the buffer is empty.
- After a valid `SLAVE_DATA` with `MORE = 1`, the master MAY immediately send `REQUEST_DATA` to the same slave. If the next fragment is not ready yet, the slave stays silent; the master continues, and the slave announces itself via the pending flag once the fragment is written.
- No sequence number is needed: the single buffer enforces order, a duplicate (lost STOP) shows up as equal `GEN`, and an overwritten fragment shows up as a `GEN` gap. Handling a gap (for example restarting the transfer) is up to the master application.
- `MORE` is set by the application; the library only transmits it.
- Applications that need the strongest integrity guarantee SHOULD use fragments of at most 12 payload bytes, which keeps every frame within the CRC8 Hamming-distance-4 range (section 7).

## 7. Integrity Check

- `CRC8` is mandatory in v0.
- Coverage: byte 0, byte 1, and all payload bytes, processed in transmission order. Since every covered field is byte-aligned, the CRC is computed over whole bytes, incrementally as each byte completes.
- Parameters (CRC-8/AUTOSAR):

  | Parameter | Value |
  |---|---|
  | Polynomial | `0x2F` (x⁸ + x⁵ + x³ + x² + x + 1) |
  | Initial value | `0xFF` |
  | Final XOR | `0xFF` |
  | Input/output reflected | no (MSB first) |
  | Check value for ASCII `"123456789"` | `0xDF` |

- Both master and slave implementations must reproduce the check value `0xDF`.
- Error detection for HUBUS frame lengths (verified by exhaustive computation):
  - Covered length up to 14 bytes (header + payload ≤ 12 bytes): all 1-, 2-, and 3-bit errors are detected (Hamming distance 4).
  - Any length: all errors with an odd number of flipped bits and all burst errors up to 8 bits are detected.
  - Longer frames: some 2-bit error patterns are undetectable (limit of any 8-bit CRC at these lengths). A clock slip (lost or extra clock edge) is detected with a probability of about 255/256.
- The non-zero initial value and final XOR ensure that neither an all-released frame (all `0`, for example a stuck master driver) nor an all-low frame (all `1`, stuck line) is ever a valid frame.
- A frame with invalid CRC8 is always discarded, even if this reduces effective throughput.

## 8. Minimal Decoder Requirements

A compliant slave implementation must:

- Restart reception at bit 0 on every START; return to idle on STOP; reset the communication stack on RESET.
- Read the 2-bit TYPE, then `TARGET_LEN`, before evaluating `MCID` match.
- Ignore non-addressed frames as soon as prefix mismatch is known.
- Respect LEN bounds and keep receiving until the end of the frame (header, `LEN` payload bytes, CRC8), even for payload it does not use.
- Validate CRC8 before acting on commands.
- Transmit `MORE` and `GEN` in `SLAVE_DATA` headers; transmit `000` in their place in `SLAVE_INFO`.
