# Wire Timeline Example

This example shows the sequence for one burst in which multiple slaves have buffered data.

## Assumptions

- Master MCID mask selects a group of slaves by MCID prefix.
- Higher MCID has higher arbitration priority.
- Slaves without buffered data remain silent.

## Timeline

1. Master sends a status-capture command using a prefix mask.
2. Matching slaves store their current status in local buffers.
3. Master issues `REQUEST_DATA` for the first pending slave.
4. Multiple slaves may contend; arbitration selects the highest-priority MCID.
5. The winner responds with `SLAVE_DATA`.
6. A losing slave may emit the handoff slot marker before releasing the bus.
7. The master keeps its pending MCID mask active and requests the next slave.
8. The master repeats until no further handoff marker appears and the mask is empty.

## Notes

- Slaves without buffered data ignore `REQUEST_DATA` silently.
- Buffered status snapshots may be overwritten before retrieval.
- The example is intentionally timing-oriented and does not define exact cycle counts.
