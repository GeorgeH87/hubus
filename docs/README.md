# HUBUS Documentation

## Contents

| Document | Content |
|---|---|
| [Overview](overview.md) | Goals, design principles, system model, the protocol at a glance |
| [Glossary](glossary.md) | Short definitions of the terms used everywhere |
| **Specification** | normative, one chapter per layer |
| [1. Physical layer](spec/01-physical-layer.md) | Lines, logic levels, bit transfer, timing parameters, nodes |
| [2. Line conditions](spec/02-line-conditions.md) | START, STOP, RESET, idle bus, back-to-back transactions |
| [3. Transactions](spec/03-transactions.md) | Transaction types, turnaround slot, response detection, transaction end |
| [4. Frames](spec/04-frames.md) | Header, message types, opcodes, addressing, `SLAVE_INFO`, event buffer, `GEN`, state, fragments, CRC8 |
| [5. Arbitration](spec/05-arbitration.md) | Arbitration and handoff slots, winner, pending masks, worked example |
| [6. Operation](spec/06-operation.md) | Idle event handshake, burst, state reads, commissioning scan, reset, startup and recovery, firmware update |
| [7. Slave requirements](spec/07-slave.md) | Responsibilities, state machine, optional parts, no timeouts |
| [8. Faults](spec/08-faults.md) | Fault scenarios and accepted rare cases |
| **Implementation and operation** | |
| [Slave library](slave-library.md) | API, optional parts, configuration, supported devices, footprint, examples |
| [Master library](master-library.md) | API of the master: HAL, startup, bursts, transactions, commissioning, diagnosis, protocol helpers |
| [hubusd](daemon.md) | The master as an MQTT service: installation, configuration, topics, Home Assistant |
| [Development](development.md) | Toolchain, build and test, documentation tools |
| **Project** | |
| [Decisions](project/decisions.md) | Why the protocol is the way it is |
| [Open issues](project/open-issues.md) | What is still open, milestones, deferred topics |
| [Roadmap](../TODO.md) | Implementation checklist |

## Reading Paths

- **Getting to know HUBUS:** [overview](overview.md), then [§3](spec/03-transactions.md) and [§6](spec/06-operation.md).
- **Writing slave firmware:** [§7](spec/07-slave.md) and the [slave library](slave-library.md); the other chapters as referenced.
- **Implementing the protocol:** the specification in chapter order.
- **Writing a master application:** the [master library](master-library.md).
- **Running a bus:** [hubusd](daemon.md).
- **Working on the code:** [development](development.md).

## Conventions

- The specification (`spec/`) is normative. MUST, SHOULD, and MAY in capitals have their usual meaning (RFC 2119); everything else describes the implementation or how to use it.
- Every fact is specified in exactly one place; other places link to it. `§4.9` means chapter 4, section 9 of the specification.
- `1` and `0` are logical bit values: `1` is `DATA` pulled low ([§1.2](spec/01-physical-layer.md#12-logic-levels-and-voltage)). MCIDs are written as six bits, most significant first, often with their decimal value: `101010` (42).
- Diagrams: Mermaid diagrams are embedded in the Markdown and rendered by GitHub. Timing diagrams use PlantUML; their sources are in [`diagrams/`](diagrams/), next to the rendered SVG files. `sh tools/build-docs.sh` renders and checks all diagrams and links ([development](development.md#documentation)).
