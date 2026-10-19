# HUBUS

HUBUS is a lightweight bus communication concept for one Raspberry Pi master and multiple ATtiny-based slaves.

This repository starts with protocol-first documentation so the concept can be refined iteratively before firmware work begins.

## Goals

- Run on small 8-bit controllers (target: ATtiny85).
- Keep the library footprint minimal for ATtiny85-class RAM/flash limits.
- Use only the Raspberry Pi and ATtiny controllers as active components on the bus.
- Use a master-generated clock.
- Allow slaves to signal pending communication on a shared data line.
- Use deterministic arbitration based on MCID.
- Keep packets compact by default while allowing multi-frame extension for larger datasets.
- Prefer data integrity and robustness over high throughput.

## Fixed v0 Decisions

- Data line pulled low means logical `1`.
- Neutral/high data line means logical `0`.
- Higher MCID has higher arbitration priority.

## Documentation Map

- `docs/README.md`: documentation index and reading order.
- `docs/GLOSSARY.md`: short definitions for core protocol terms.
- `docs/PROTOCOL.md`: protocol roles, states, and communication lifecycle.
- `docs/ARBITRATION.md`: bit-wise arbitration and collision prevention rules.
- `docs/MESSAGE_FORMAT.md`: compact frame model and message types.
- `docs/HARDWARE_REQUIREMENTS.md`: electrical and timing baseline assumptions.
- `docs/SLAVE_PROPERTIES.md`: mandatory slave capabilities and behavior.
- `docs/ITERATIVE_CONCEPT.md`: roadmap, open points, and next iterations.
- `examples/TYPICAL_SESSION.md`: full flow example.
- `examples/ERROR_SCENARIOS.md`: failure modes and recovery behavior.
- `examples/WIRE_TIMELINE.md`: timing-oriented burst example.

## Current Scope

This project focuses on defining a reusable bus library and protocol that enable common device behaviors.

Device-specific implementations (for example relay, LED, switch, or temperature sensor) are out of core scope and are treated as optional example implementations only.
