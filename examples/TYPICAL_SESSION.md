# Typical Session Example

This example shows one poll cycle with three slaves.

## Setup

- Slave A: MCID `0b011001` (25)
- Slave B: MCID `0b101010` (42)
- Slave C: MCID `0b001111` (15)

Slave A and Slave B have pending data. Slave C has no data.

## Sequence

1. Slave B gets a local event while bus is idle and pulls `DATA` low.
2. Master detects low `DATA` in idle and sends confirmation ticks on `CLK`.
3. Slave B releases `DATA` after confirmation, then waits for protocol intent.
4. Master sends `POLL_PENDING`.
5. A and B enter arbitration. C stays passive.
6. Arbitration bit 1 (MSB):
- A sends `0` (release).
- B sends `1` (pull low).
- Line is low -> A detects dominant `1` while sending `0` and drops out.
7. B remains sole contender and wins arbitration.
8. Slave A emits the handoff slot marker before fully releasing the bus, indicating that another pending slave still exists.
9. Master keeps the burst open and continues polling after the current transfer.
10. Master sends `REQUEST_DATA` to MCID 42.
11. Slave B replies with `SLAVE_DATA` frame and valid CRC8.
12. Master acknowledges and keeps polling until no further handoff marker appears.

## Why B Wins

Higher MCID has higher priority, and bit `1` is dominant because it pulls the line low.
