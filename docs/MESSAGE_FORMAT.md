# Message Format

## 1. Goals

- Keep parser small for ATtiny85.
- Keep control messages short.
- Keep payloads small by default but extensible across multiple frames.
- Prioritize integrity checks over transfer speed.

## 2. Frame Structure (v0)

A compact fixed-header frame is used.

| Field | Size (bits) | Description |
|---|---:|---|
| SOF | 4 | Start marker |
| TYPE | 3 | Message class |
| TARGET_LEN | 3 | Number of significant target prefix bits (master-to-slave only) |
| MCID | 6 | Target prefix or source MCID (v0 default width) |
| LEN | 5 | Payload length in bytes (0..31) |
| PAYLOAD | variable | Message data |
| CRC8 | 8 | Integrity check over TYPE..PAYLOAD |

This base frame intentionally favors low overhead and simple parsing on ATtiny85.

Every master frame begins after a START line condition (see `PROTOCOL.md` section 1.2). START, not SOF, defines the frame boundary; the receiver starts at bit 0 after each START. The SOF field therefore no longer carries delimiting meaning. Whether SOF is removed or repurposed (for example as a version marker) is decided together with the header alignment rework (open decision D4 in `ITERATIVE_CONCEPT.md`).

## 3. Master Message Types

| Type | Name | Addressing | Slave reaction |
|---|---|---|---|
| `000` | `POLL_PENDING` | exact or group | Arbitration with handoff slots; only addressed pending slaves contend (`ARBITRATION.md`). No frame response. |
| `001` | `REQUEST_INFO` | exact only | `SLAVE_INFO` with static device metadata (section 4.1). |
| `010` | `REQUEST_DATA` | exact only | `SLAVE_DATA` with the content of the slave buffer; silent if the buffer is empty. |
| `011` | `SEND_COMMAND` | exact or group | Execute the command. **Never a response**, regardless of addressing. |

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
| `100` | reserved | – (formerly `SLAVE_ACK`; commands are never acknowledged) |
| `101` | `SLAVE_DATA` | `REQUEST_DATA` |
| `110` | `SLAVE_INFO` | `REQUEST_INFO` |
| `111` | reserved | – (formerly `SLAVE_ERROR`; see below) |

Both slave type codes start with `1`. The master uses this to detect a missing response from the first bit after the turnaround slot (`PROTOCOL.md` section 1.3). Any future header layout must keep this property.

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

An optional one-byte generation counter MAY be placed at the start of the buffer payload. If used, the counter increments whenever the buffer is overwritten, which helps the master detect that a newer value replaced an older unread one.

## 5. Addressing Rules

- `MCID` identifies slave nodes.
- During master-to-slave frames, `TARGET_LEN` is interpreted first, then `MCID` is evaluated as target prefix.
- During slave-to-master frames, `MCID` is the source.

- MCID `0` is reserved and never assigned to a slave. Valid slave MCIDs are `1..63` in v0. (Reason: a slave with MCID `0` would send only released bits in arbitration and be indistinguishable from "no contender".) A frame addressed with `TARGET_LEN=6` and `MCID=0` matches no slave.

The master-side pending mask stack is internal scheduling state and is not transmitted as a separate frame field in v0.

Handoff slots are part of the arbitration phase only; they do not add a frame field or change the frame layout.

START, STOP, and RESET are line conditions, not frame content. RESET in particular is never encoded as a frame, so it works even when a slave has lost bit synchronization.

## 6. Encoding Rules

- Bit order in frame fields: MSB first.
- Arbitration MCID order: MSB first.
- `LEN=0` is valid.
- Maximum payload in v0 is 31 bytes.

## 6.1 Payload Extension Strategy

- v0 keeps each frame small (`LEN <= 31`) to remain lightweight.
- Larger logical datasets (for example weather records) are transferred as multiple ordered `SLAVE_DATA` frames.
- Reassembly policy is handled at application/library level, not by adding heavy transport state to tiny slaves.

## 7. Integrity Check

- `CRC8` is mandatory in v0.
- Polynomial selection and initial value are tracked in `docs/ITERATIVE_CONCEPT.md` until frozen.
- A frame with invalid CRC8 is always discarded, even if this reduces effective throughput.

## 8. Minimal Decoder Requirements

A compliant slave implementation must:

- Restart reception at bit 0 on every START; return to idle on STOP; reset the communication stack on RESET.
- Validate TYPE (and SOF while the field exists).
- Read `TARGET_LEN` before evaluating `MCID` match.
- Ignore non-addressed frames as soon as prefix mismatch is known.
- Respect LEN bounds.
- Validate CRC8 before acting on commands.
