# HUBUS

HUBUS is a lightweight two-wire bus for one Raspberry Pi master and up to 63 ATtiny slaves: a master-generated clock, a shared open-drain data line, slaves that announce events on their own, and a deterministic arbitration by address.

The protocol is specified in [`docs/`](docs/README.md) and implemented in C: a hardware-independent core shared by master, slaves, and tests, a slave library for tinyAVR devices, a master for the Raspberry Pi, `hubusd` as an MQTT service with Home Assistant integration, and a bus simulation with fault injection. Hardware bring-up (timing values, runs on a real Raspberry Pi and ATtiny slaves) is still open ([roadmap](TODO.md)).

## Where to Start

- [Overview](docs/overview.md): goals, design principles, and the protocol at a glance
- [Documentation index](docs/README.md): specification, slave library, hubusd, development
- [Slave library](docs/slave-library.md): writing slave firmware
- [hubusd](docs/daemon.md): running a bus from MQTT and Home Assistant
- [Development](docs/development.md): toolchain, build, and tests

## Repository Layout

| Directory | Content |
|---|---|
| `core/` | Protocol code without hardware access: CRC, frames, arbitration, slave state machine |
| `slave/` | Slave library with one HAL per device family |
| `master/` | Master logic on a HAL, and the libgpiod HAL for the Raspberry Pi |
| `daemon/` | `hubusd`, the master as an MQTT service |
| `test/` | Unit tests, bus simulation, fault injection |
| `examples/` | Slave firmware examples and a master command line tool |
| `docs/` | Specification and documentation |

## License

HUBUS is open source under the MIT License, see `LICENSE`. The bundled Unity test framework in `test/unity/` is also MIT-licensed (`test/unity/LICENSE.txt`).
