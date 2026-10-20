# HUBUS

HUBUS is a lightweight bus communication concept for one Raspberry Pi master and multiple ATtiny-based slaves.

This repository starts with protocol-first documentation so the concept can be refined iteratively before firmware work begins.

## Goals

- Run on small 8-bit controllers: the slave library supports classic tinyAVR and tinyAVR 0/1/2-series devices. The ATtiny85 is the reference target and is always fully supported.
- Keep the library footprint minimal for ATtiny85-class RAM/flash limits.
- Use only the Raspberry Pi and ATtiny controllers as active components on the bus.
- Master runs on Raspberry Pi 3 and newer (including Pi 5), written in C and sharing the protocol core with the slaves.
- Use a master-generated clock.
- Allow slaves to signal pending communication on a shared data line.
- Use deterministic arbitration based on MCID.
- Keep packets compact by default while allowing multi-frame extension for larger datasets.
- Prefer data integrity and robustness over high throughput.

## Design Principles

- **Lightweight slaves, smart master.** Complex logic (timeouts, retries, scheduling, fault detection, recovery) lives on the master. The slave library only implements what cannot be moved to the master.
- **The master clock is the slave's only time base.** Slaves have no timers and no timeouts. Every slave state transition is triggered by a master clock edge or a master line condition.
- **Pragmatic robustness.** Failures that can only occur in very rare corner cases do not have to be excluded at any cost. If such a case is rare and recovers at the next master cycle, it is documented as an accepted known case instead of adding slave complexity.

## Fixed v0 Decisions

- Data line pulled low means logical `1`.
- Neutral/high data line means logical `0`.
- Higher MCID has higher arbitration priority.
- `CLK` idles low.
- 3.3 V bus: all slaves run at 3.3 V; no level shifters.
- Frames are delimited by START/STOP line conditions; START immediately followed by STOP is a communication stack RESET (see `docs/PROTOCOL.md`).
- Slaves implement no timeouts; all timeouts are master-side.
- Commands are fire-and-forget (no acknowledgements); the master verifies states by reading them back.
- Group addressing is one-way (master to n slaves); slaves report via the pending flag and bursts.

## Documentation Map

- `docs/README.md`: documentation index and reading order.
- `docs/GLOSSARY.md`: short definitions for core protocol terms.
- `docs/PROTOCOL.md`: protocol roles, states, and communication lifecycle.
- `docs/ARBITRATION.md`: bit-wise arbitration and collision prevention rules.
- `docs/MESSAGE_FORMAT.md`: compact frame model and message types.
- `docs/HARDWARE_REQUIREMENTS.md`: electrical and timing baseline assumptions.
- `docs/SLAVE_PROPERTIES.md`: mandatory slave capabilities and behavior.
- `docs/ITERATIVE_CONCEPT.md`: roadmap, open points, and next iterations.
- `docs/OPEN_ISSUES.md`: tracking list of known gaps, inconsistencies, and defects.
- `examples/TYPICAL_SESSION.md`: full flow example.
- `examples/ERROR_SCENARIOS.md`: failure modes and recovery behavior.
- `examples/WIRE_TIMELINE.md`: timing-oriented burst example.

## Current Scope

This project focuses on defining a reusable bus library and protocol that enable common device behaviors.

Device-specific implementations (for example relay, LED, switch, or temperature sensor) are out of core scope and are treated as optional example implementations only.
