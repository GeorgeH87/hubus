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
| `00` | `POLL_PENDING` | exact or group | Arbitration with handoff slots; only addressed pending slaves contend (`ARBITRATION.md`). No frame response. |
| `01` | `REQUEST_INFO` | exact only | `SLAVE_INFO` with static device metadata (section 4.1). |
| `10` | `REQUEST_DATA` | exact only | `SLAVE_DATA` with the content of the slave buffer; silent if the buffer is empty. |
| `11` | `SEND_COMMAND` | exact or group | Execute the command. **Never a response**, regardless of addressing. |

`POLL_PENDING`, `REQUEST_INFO`, and `REQUEST_DATA` carry `LEN = 0` in v0; other values are reserved. Slaves still receive all `LEN` payload bytes and the CRC8 so they know where the frame ends and the turnaround slot begins.

All master type codes are in use. Future protocol functions are added as reserved `SEND_COMMAND` opcodes (section 3.0), not as new types.

`SEND_COMMAND` is always fire-and-forget. If the master needs to confirm that a state was established, it reads the state back explicitly (status-capture followed by `REQUEST_DATA`).

## 3.0 Command Opcodes

The first payload byte of `SEND_COMMAND` is the opcode. A `SEND_COMMAND` with `LEN=0` is ignored.

| Opcode | Meaning |
|---|---|
| `0x00` | `STATUS_CAPTURE`: application writes its current status into the slave buffer and the slave sets its pending flag. |
| `0x01..0x0F` | Reserved for protocol use. Slaves ignore unknown reserved opcodes. |
| `0x10..0xFF` | Application-defined (for example relay on/off). |

Remaining payload bytes after the opcode are opcode-specific.

## 3.1 Unified Target Mask Addressing

All master-to-slave commands (`POLL_PENDING`, `REQUEST_INFO`, `REQUEST_DATA`, `SEND_COMMAND`) use the same target-mask rule. For `POLL_PENDING`, the mask selects which pending slaves contend in the arbitration; the master uses this to poll the prefix groups derived from handoff slots.

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

Group and broadcast addressing (`TARGET_LEN < 6`) is strictly one-way: master to n slaves.

- No slave ever sends a response frame to a group or broadcast frame.
- `SEND_COMMAND` to a group is executed by every matching slave without response (as for exact addressing).
- `REQUEST_INFO` and `REQUEST_DATA` expect a response and are therefore only valid with `TARGET_LEN = 6`. Slaves ignore them otherwise.
- If a group command produces something to report (for example a `STATUS_CAPTURE` result), the slave sets its pending flag and uses the regular return path: idle event handshake and burst (`PROTOCOL.md` section 3.2).
- `POLL_PENDING` follows the same rule: it is answered by arbitration with handoff slots, never by a frame.

Response frames are only sent for `REQUEST_INFO` and `REQUEST_DATA` by a slave addressed exactly (`TARGET_LEN = 6`), so at most one slave transmits a frame at any time.

Valid range in v0: `0..6`.

- `0`: broadcast to all slaves.
- `1..5`: prefix group addressing.
- `6`: exact MCID addressing.

## 4. Slave Message Types

| Type | Name | Sent in response to |
|---|---|---|
| `00` | reserved | – |
| `01` | reserved | – |
| `10` | `SLAVE_DATA` | `REQUEST_DATA` |
| `11` | `SLAVE_INFO` | `REQUEST_INFO` |

Slave type codes must start with `1`; codes `00` and `01` are never used by slaves. The master uses this to detect a missing response from the first bit after the turnaround slot (`PROTOCOL.md` section 1.3).

Slaves send no acknowledgements and no error frames. Rationale: commands are fire-and-forget, so there is no request an acknowledgement or error could answer. A slave that needs to report an error (for example an application fault) writes it into its buffer and uses the regular return path; the error format is application-defined.

### 4.1 Slave Info (`SLAVE_INFO`)

Static metadata, typically a constant in flash.

| Byte | Content |
|---|---|
| 0 | HUBUS protocol version |
| 1 | Capability flags: bit 0 = supports `STATUS_CAPTURE`; other bits reserved (0) |
| 2.. | Application-defined (for example device type, firmware version) |

### 4.2 Slave Buffer and `SLAVE_DATA`

Each slave has exactly one buffer for outgoing data.

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
- `SLAVE_INFO` frames carry `GEN = 00`.

Master evaluation (per MCID, master-side only):

| New `GEN` vs. last received | Meaning |
|---|---|
| last + 1 | Normal: next buffer content. |
| equal, payload identical | Duplicate (for example after a lost STOP); discard. |
| equal, payload different | New content; the counter wrapped after four writes. |
| any other step | At least one buffer content was overwritten unread (information, not an error). |
| first frame from this MCID since master start | Taken as baseline. |

The payload comparison prevents new content from being discarded as a duplicate after a counter wrap. The only remaining ambiguity (wrap **and** identical payload) is harmless, since the data is the same.

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

## 7. Integrity Check

- `CRC8` is mandatory in v0.
- Coverage: byte 0, byte 1, and all payload bytes, processed in transmission order. Since every covered field is byte-aligned, the CRC is computed over whole bytes, incrementally as each byte completes.
- Polynomial, initial value, and final XOR are tracked in `docs/ITERATIVE_CONCEPT.md` (D2) until frozen.
- A frame with invalid CRC8 is always discarded, even if this reduces effective throughput.

## 8. Minimal Decoder Requirements

A compliant slave implementation must:

- Restart reception at bit 0 on every START; return to idle on STOP; reset the communication stack on RESET.
- Read the 2-bit TYPE, then `TARGET_LEN`, before evaluating `MCID` match.
- Ignore non-addressed frames as soon as prefix mismatch is known.
- Respect LEN bounds and keep receiving until the end of the frame (header, `LEN` payload bytes, CRC8), even for payload it does not use.
- Validate CRC8 before acting on commands.
- Transmit `MORE` and `GEN` in `SLAVE_DATA` headers; transmit `000` in their place in `SLAVE_INFO`.
