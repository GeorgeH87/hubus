# 4. Frames

A frame is the unit of data in both directions: a 16-bit header, an optional payload, and a CRC8. One parser and one CRC routine serve master and slave frames.

## 4.1 Frame Structure

```
Bit:     15 14 | 13 12 11   | 10  9  8  7  6  5 |  4  3  2  1  0
Master:  TYPE  | TARGET_LEN |       MCID        |      LEN
Slave:   TYPE  |MORE| GEN  |       MCID        |      LEN
         -------------- Byte 0 --------|-------- Byte 1 ---------

Frame:   [Byte 0] [Byte 1] [PAYLOAD: LEN bytes] [CRC8]
```

| Field | Bits | Master frame | Slave frame |
|---|---:|---|---|
| `TYPE` | 2 | master message type ([§4.3](#43-master-message-types)) | slave message type ([§4.6](#46-slave-message-types)) |
| `TARGET_LEN` / `MORE` + `GEN` | 3 | number of significant prefix bits, `0..6` ([§4.5](#45-addressing)) | bit 13 `MORE` ([§4.11](#411-fragments)), bits 12–11 `GEN` ([§4.9](#49-generation-counter-gen)); `000` in `SLAVE_INFO` |
| `MCID` | 6 | target prefix, MSB-aligned ([§4.5](#45-addressing)) | source MCID |
| `LEN` | 5 | payload length in bytes, `0..31` | payload length in bytes, `0..31` |
| payload | `LEN` × 8 | message data | message data |
| `CRC8` | 8 | over byte 0, byte 1, and the payload ([§4.12](#412-integrity-check-crc8)) | same |

- The header is exactly two bytes, so payload and CRC8 are byte-aligned without padding. A frame is `3 + LEN` bytes long: at least 24 bits, at most 34 bytes.
- There is no start-of-frame field: a master frame begins at its START ([§2.1](02-line-conditions.md#21-conditions)), and a slave frame directly after the turnaround or arbitration slots ([§3.1](03-transactions.md#31-transaction-types)). The transaction sequence always tells the direction, so 2-bit type codes suffice per direction.
- Protocol versions are announced in `SLAVE_INFO` ([§4.7](#47-slave-info-slave_info)), not in the header.

## 4.2 Encoding

- Bytes are sent in the order byte 0, byte 1, payload, CRC8; within a byte, and therefore within every field, the most significant bit first. The first bit after START is header bit 15.
- `LEN = 0` is valid; the maximum payload is 31 bytes.

## 4.3 Master Message Types

| `TYPE` | Name | Addressing | Slave reaction |
|---|---|---|---|
| `00` | `REQUEST_STATE` | exact or group | Send the live **state** ([§4.10](#410-state)) as `SLAVE_DATA`. A slave without a state stays silent and does not contend. Nothing is cleared. |
| `01` | `REQUEST_INFO` | exact or group | Send `SLAVE_INFO` ([§4.7](#47-slave-info-slave_info)). Every matching slave can answer. |
| `10` | `REQUEST_DATA` | exact or group | Send the content of the **event buffer** ([§4.8](#48-event-buffer)) as `SLAVE_DATA`. A slave without pending data stays silent and does not contend. The STOP after a complete response clears the buffer ([§3.6](03-transactions.md#36-transaction-end-and-buffer-clearing)). |
| `11` | `SEND_COMMAND` | exact or group | Execute the command ([§4.4](#44-command-opcodes)). **Never** a response, whatever the addressing. |

- With exact addressing the addressed slave answers; with group addressing the matching slaves that can answer arbitrate, and the winner answers in the same transaction ([§5](05-arbitration.md)).
- The three requests carry `LEN = 0` in v0; other values are reserved. A slave still receives all `LEN` payload bytes and the CRC8, so it knows where the frame ends; it includes them in the CRC check, otherwise ignores them, and answers as if `LEN` were `0`.
- All four type codes are in use. Future protocol functions become reserved `SEND_COMMAND` opcodes ([§4.4](#44-command-opcodes)), not new types.

`REQUEST_STATE` and `REQUEST_DATA` answer different questions:

| | `REQUEST_STATE` | `REQUEST_DATA` |
|---|---|---|
| Question | What is your state now? | Do you have something to deliver? |
| Source | the live state the application exposes, for example the level of every input | the event buffer, written by the application on a change or by `STATUS_CAPTURE` |
| Contenders in a group request | every matching slave with a state | only matching slaves with pending data |
| Effect | none; repeatable at any time | the STOP after a complete response clears buffer and pending flag |

## 4.4 Command Opcodes

The first payload byte of `SEND_COMMAND` is the opcode; the remaining bytes are opcode-specific. A `SEND_COMMAND` with `LEN = 0` is ignored.

| Opcode | Meaning |
|---|---|
| `0x00` | `STATUS_CAPTURE`: the application writes its current status into the event buffer, which sets the pending flag ([§4.8](#48-event-buffer)). Ignored by a slave without event buffer. |
| `0x01..0x04` | Firmware update: `UPDATE_ENTER` for an application with update support; `UPDATE_LOAD`, `UPDATE_WRITE`, and `UPDATE_LEAVE` for the updater ([§6.8](06-operation.md#68-firmware-update)). |
| `0x05..0x0F` | Reserved for protocol use. |
| `0x10..0xFF` | Application-defined, for example relay on/off. |

Slaves ignore unknown reserved opcodes, including the update opcodes they do not take.

A slave ignores a command whose payload exceeds the maximum it announces in `SLAVE_INFO` byte 2 ([§4.7](#47-slave-info-slave_info)). It still receives the complete frame and stays synchronized. The master must not send larger commands than a slave accepts.

Commands are fire-and-forget. A master that needs to know whether a command took effect reads the state back with `REQUEST_STATE`.

## 4.5 Addressing

Every master frame addresses its slaves with a target mask: `TARGET_LEN` significant prefix bits of the `MCID` field.

- A slave is addressed when its MCID begins with the first `TARGET_LEN` bits of the `MCID` field; the remaining bits are ignored.
- `TARGET_LEN = 0` addresses all slaves (broadcast), `1..5` a prefix group, `6` exactly one MCID. `TARGET_LEN = 7` matches no slave.
- Example: `TARGET_LEN = 4`, `MCID = 1101xx` addresses every slave whose MCID begins with `1101`.
- A slave reads `TYPE` and `TARGET_LEN` first and rejects the frame as soon as the arriving prefix bits prove a mismatch.
- Valid slave MCIDs are `1..63`. MCID `0` is reserved and never assigned: such a slave would send only released bits in an arbitration and be indistinguishable from "no contender" ([§5.4](05-arbitration.md#54-winner)). `TARGET_LEN = 6` with `MCID = 0` matches no slave.

**Direction rule.** Whenever a response is expected, at most one slave answers:

- `SEND_COMMAND` never produces a response. Sent to a group, every matching slave executes it.
- A request with `TARGET_LEN = 6` is answered by the addressed slave alone.
- A request with `TARGET_LEN < 6` is answered by the arbitration winner alone, in the same transaction ([§5](05-arbitration.md)). Slaves that cannot answer do not contend.
- A slave that has something to report after a group command (for example a `STATUS_CAPTURE` result) uses the regular return path: pending flag, idle event handshake, and burst ([§6.1](06-operation.md#61-idle-event-handshake), [§6.2](06-operation.md#62-burst)).

So at most one slave transmits a frame at a time. The only exception is a duplicate MCID, which is a configuration fault ([§6.4](06-operation.md#64-commissioning-scan-and-duplicate-mcids)).

## 4.6 Slave Message Types

| `TYPE` | Name | Answer to |
|---|---|---|
| `00`, `01` | reserved | never used by slaves |
| `10` | `SLAVE_DATA` | `REQUEST_DATA` (event buffer) and `REQUEST_STATE` (state) |
| `11` | `SLAVE_INFO` | `REQUEST_INFO` |

Slave type codes start with `1`, which makes every response visible in its first bit ([§3.4](03-transactions.md#34-response-detection)).

Slaves send no acknowledgements and no error frames: commands are fire-and-forget, so there is no request such a frame could answer. A slave that wants to report an error, for example an application fault, writes it into its event buffer; the format is application-defined.

## 4.7 Slave Info (`SLAVE_INFO`)

Static metadata, typically a constant in flash.

| Byte | Content |
|---|---|
| 0 | HUBUS protocol version (`0` in v0) |
| 1 | Capabilities: bit 0 = supports `STATUS_CAPTURE`; bit 1 = has a state and answers `REQUEST_STATE`; bit 2 = has an event buffer (announces events, answers `REQUEST_DATA`); bit 3 = application with firmware update, takes `UPDATE_ENTER`; bit 4 = the updater is running ([§6.8](06-operation.md#68-firmware-update)); bits 7–5 are reserved (`0`). Bit 0 requires bit 2; bits 3 and 4 exclude each other. |
| 2 | Maximum command payload the slave accepts, in bytes (`1..31`; including the opcode) |
| 3 | Bits 3–0: speed class `0..15`, `0` = not declared ([§1.5](01-physical-layer.md#15-speed-classes)). Bits 7–4: blind class `0..15`, `0` = never blind after a command ([§7.8](07-slave.md#78-blind-sections)). |
| 4.. | Application-defined, for example device type, firmware version. SHOULD include a device-unique value, such as a serial number, so that the commissioning scan reliably detects duplicate MCIDs ([§6.4](06-operation.md#64-commissioning-scan-and-duplicate-mcids)). |

The reserved bits are a reserve for later protocol functions: slaves send `0`, and masters ignore them.

## 4.8 Event Buffer

A slave with an event buffer has exactly **one** outgoing buffer. The event buffer is optional ([§7.4](07-slave.md#74-optional-parts)); a slave without it never announces anything, never answers `REQUEST_DATA`, and does not support `STATUS_CAPTURE`.

- The application writes event data into it (for example a button press), or its current status on `STATUS_CAPTURE`. Every write sets the pending flag and increments `GEN` ([§4.9](#49-generation-counter-gen)), and states whether further fragments follow (`MORE`, [§4.11](#411-fragments)).
- New content overwrites unread content; the buffer is not a queue. A status capture may overwrite an unread event and vice versa; this is accepted, and the master sees it as a gap ([§4.9](#49-generation-counter-gen)).
- `REQUEST_DATA` returns the content as `SLAVE_DATA`. With an empty buffer (no pending data) the slave stays silent; there is no negative acknowledgement.
- The STOP after a complete response clears buffer and pending flag; a RESET keeps both for a retry ([§3.6](03-transactions.md#36-transaction-end-and-buffer-clearing)).
- A slave whose pending flag is set announces itself on the bus ([§6.1](06-operation.md#61-idle-event-handshake)).

## 4.9 Generation Counter (`GEN`)

`GEN` is a mandatory 2-bit counter in the header of every `SLAVE_DATA` frame with event data.

- The slave increments `GEN` (modulo 4) on every write of the event buffer, by the application or by `STATUS_CAPTURE`. It survives a RESET.
- `SLAVE_INFO` and state frames carry `GEN = 00`.

The master evaluates it per MCID:

| New `GEN` compared with the last one received | Meaning |
|---|---|
| last + 1 | normal: the next buffer content |
| equal, payload identical | duplicate, for example after a lost STOP ([§8.4](08-faults.md#84-lost-stop)); discarded |
| equal, payload different | new content; the counter wrapped after four writes |
| any other step | at least one buffer content was overwritten unread; information, not an error |
| first frame from this MCID since the master started | taken as the baseline |

The payload comparison keeps new content after a wrap from being discarded as a duplicate. The remaining ambiguity (wrap **and** identical payload) is harmless, because the data is the same.

## 4.10 State

A slave may expose a **state**: 1 to 31 bytes that the application keeps up to date, for example one bit per button or a measured value. The state is optional ([§7.4](07-slave.md#74-optional-parts)).

- `REQUEST_STATE` is answered with a `SLAVE_DATA` frame with `MORE = 0`, `GEN = 00`, and the state bytes as payload. The master knows from its request that it is a state frame.
- It can be requested at any time and as often as needed. It neither sets nor clears the pending flag and does not change `GEN`.
- A slave without a state ignores `REQUEST_STATE`; `SLAVE_INFO` capability bit 1 tells the master which slaves have one.
- The slave reads the state bytes directly from the application while it transmits; there is no copy. If the application changes a byte during the transmission, the frame may mix old and new bytes; the CRC covers what was sent, so the frame stays valid ([§8.6](08-faults.md#86-mixed-state-bytes)). An application that needs a consistent multi-byte snapshot keeps the state within one byte or updates it so that a mix is harmless.
- State and events complement each other. A multi-button slave exposes the level of all buttons as its state and writes each press into the event buffer: the master collects the presses in bursts and can read the state at any time.

## 4.11 Fragments

Larger data, for example a set of records, is sent as a sequence of `SLAVE_DATA` fragments through the single event buffer. Reassembly is the master application's task; slaves keep no transport state.

1. The slave application writes the first fragment with `MORE = 1`.
2. The master reads it; the STOP empties the buffer.
3. The slave application sees the empty buffer and writes the next fragment, which sets the pending flag again.
4. This repeats until a fragment with `MORE = 0`.

- The slave application writes the next fragment only when the buffer is empty. `MORE` is set by the application; the library only transmits it.
- After a valid fragment with `MORE = 1` the master MAY ask the same slave again at once. If the next fragment is not written yet, the slave stays silent, and it announces itself later through the pending flag.
- No sequence number is needed: the single buffer keeps the order, a duplicate shows as an equal `GEN`, and an overwritten fragment as a `GEN` gap ([§4.9](#49-generation-counter-gen)). What to do about a gap, for example restarting the transfer, is up to the master application.
- Applications that need the strongest integrity SHOULD use fragments of at most 12 payload bytes, which keeps every frame in the range where the CRC8 detects all 1-, 2-, and 3-bit errors ([§4.12](#412-integrity-check-crc8)).

## 4.12 Integrity Check (CRC8)

Every frame carries a CRC8 over byte 0, byte 1, and all payload bytes, in transmission order. Since every covered field is byte-aligned, it is computed over whole bytes, incrementally as each byte completes.

| Parameter (CRC-8/AUTOSAR) | Value |
|---|---|
| Polynomial | `0x2F` (x⁸ + x⁵ + x³ + x² + x + 1) |
| Initial value | `0xFF` |
| Final XOR | `0xFF` |
| Reflected input/output | no (MSB first) |
| Check value for ASCII `"123456789"` | `0xDF` |

- Master and slave implementations must reproduce the check value `0xDF`.
- Error detection for HUBUS frame lengths, verified by exhaustive computation: up to 14 covered bytes (header plus at most 12 payload bytes) every 1-, 2-, and 3-bit error is detected (Hamming distance 4). At any length every error with an odd number of flipped bits and every burst of up to 8 bits is detected. In longer frames some 2-bit patterns are undetectable, as for any 8-bit CRC; a clock slip is detected with a probability of about 255/256.
- Because of the non-zero initial value and final XOR, neither an all-released frame (all `0`, for example a stuck driver) nor an all-low frame (all `1`, a stuck line) is ever valid.
- A frame with an invalid CRC8 is always discarded, even at the cost of throughput.
