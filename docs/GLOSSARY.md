# Glossary

## Terms

- **MCID**: Master Communication ID. The unique identifier used for arbitration and addressing.
- **Priority**: The rule that higher MCID values win arbitration in v0.
- **Pending MCID Mask**: Internal master-side state that tracks which slaves are expected to be served in the current burst.
- **Handoff Slot**: A short on-bus continuation marker used by a losing slave to tell the master that more slaves still have pending data.
- **Buffered Data**: Status or payload data stored locally by a slave until the master retrieves it.
- **Status-Capture Command**: A master intent that asks one or more slaves to store their current status in a local buffer.
- **Overwrite Policy**: The rule that a newer buffered status value may replace an unread older one.
- **Target Mask Addressing**: Unified addressing mode where master commands provide an `MCID` prefix plus `TARGET_LEN` and all matching slaves are addressed.
- **Prefix Match**: A slave matches when its most-significant `TARGET_LEN` MCID bits equal the command prefix.
- **Generation Counter**: Optional byte that can be incremented when a buffer is overwritten so the master can detect a newer snapshot.
