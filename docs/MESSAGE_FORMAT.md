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

## 3. Master Message Types

- `POLL_PENDING` (`TYPE=000`): Ask whether slaves have pending data. Can trigger arbitration.
- `REQUEST_INFO` (`TYPE=001`): Request status or metadata from selected slave.
- `REQUEST_DATA` (`TYPE=010`): Request buffered data from selected slave.
- `SEND_COMMAND` (`TYPE=011`): Send command payload to selected slave.

The master may also issue a status-capture trigger to one or more slaves; any resulting buffered status snapshot may overwrite a previous unread snapshot in that slave.

## 3.1 Unified Target Mask Addressing

All addressed master-to-slave commands (`REQUEST_INFO`, `REQUEST_DATA`, `SEND_COMMAND`) use the same target-mask rule.

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

For status-capture, this same unified mask addressing is used.

Valid range in v0: `0..6`.

- `0`: broadcast to all slaves.
- `1..5`: prefix group addressing.
- `6`: exact MCID addressing.

The command payload SHOULD carry a small opcode so slaves can distinguish status-capture from other command payloads without adding extra frame types.

Other type values are reserved.

## 4. Slave Message Types

- `SLAVE_ACK` (`TYPE=100`): Positive acknowledgement.
- `SLAVE_DATA` (`TYPE=101`): Data payload from slave.
- `SLAVE_STATUS` (`TYPE=110`): Status/diagnostics payload.
- `SLAVE_ERROR` (`TYPE=111`): Error indication.

If a slave has no buffered data for `REQUEST_DATA`, it ignores the request completely and stays silent.

This request is intentionally one-way: no negative acknowledgement is required for the empty-buffer case.

Buffered status data is overwriteable by design and does not imply queue semantics.

An optional one-byte generation counter MAY be placed at the start of a buffered status payload. If used, the counter increments whenever the buffer is overwritten, which helps the master detect that a newer snapshot replaced an older unread one.

## 5. Addressing Rules

- `MCID` identifies slave nodes.
- During master-to-slave frames, `TARGET_LEN` is interpreted first, then `MCID` is evaluated as target prefix.
- During slave-to-master frames, `MCID` is the source.

The master-side pending MCID mask is internal scheduling state and is not transmitted as a separate frame field in v0.

The handoff slot also remains an on-bus timing marker only; it does not add a new frame field or change the frame layout.

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

- Validate SOF and TYPE.
- Read `TARGET_LEN` before evaluating `MCID` match.
- Ignore non-addressed frames as soon as prefix mismatch is known.
- Respect LEN bounds.
- Validate CRC8 before acting on commands.
