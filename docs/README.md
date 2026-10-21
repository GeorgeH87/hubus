# Documentation Index

This directory contains the normative documentation for HUBUS.

## Core Specs

- `PROTOCOL.md`: system model, communication lifecycle, and master/slave behavior.
- `ARBITRATION.md`: MCID arbitration, pending masks, and handoff behavior.
- `MESSAGE_FORMAT.md`: frame layout, message types, unified target-mask addressing, and payload rules.
- `HARDWARE_REQUIREMENTS.md`: electrical and timing assumptions.
- `SLAVE_PROPERTIES.md`: minimal slave capabilities and example device categories.
- `ITERATIVE_CONCEPT.md`: open decisions, roadmap, and deferred topics.
- `OPEN_ISSUES.md`: tracking list of known gaps, inconsistencies, and defects.
- `DAEMON_REQUIREMENTS.md`: requirements for the MQTT master daemon.
- `DAEMON.md`: operating the bus from MQTT / Home Assistant (hubusd).

## Development

- `DEVELOPMENT.md`: WSL/Ubuntu development environment and toolchain.

## Examples

- `TYPICAL_SESSION.md`: end-to-end reference session.
- `ERROR_SCENARIOS.md`: fault handling and recovery examples.
- `WIRE_TIMELINE.md`: compact timing-oriented example of a complete burst.

## Reading Order

1. Start with `PROTOCOL.md`.
2. Read `ARBITRATION.md` and `MESSAGE_FORMAT.md` together.
3. Use `SLAVE_PROPERTIES.md` and `HARDWARE_REQUIREMENTS.md` for implementation constraints.
4. Consult `ITERATIVE_CONCEPT.md` for decisions that are intentionally left open.
