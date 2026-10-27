# Development

## Environment

Development runs in WSL2 with Ubuntu 26.04 LTS on Windows. The same Ubuntu setup builds the host tests, the bus simulation, the master, the daemon, the AVR slave firmware, and the documentation.

Create a dedicated distribution (PowerShell):

```powershell
wsl --install Ubuntu-26.04 --name hubus
```

Inside the distribution, from the repository root:

```sh
sudo sh tools/setup-ubuntu.sh     # toolchain and libraries
sh tools/setup-docs.sh            # documentation tools (only for working on the documentation)
```

The repository can stay on the Windows file system and is reachable from WSL at `/mnt/c/...`; builds are small, so the slower cross-file-system access is acceptable. Recommended Git settings inside WSL:

```sh
git config --global core.autocrlf input
git config --global --add safe.directory /mnt/c/Users/<user>/Projekte/hubus
```

Editor: VS Code with the extensions `ms-vscode-remote.remote-wsl`, `ms-vscode.cpptools`, and `ms-vscode.cmake-tools`, opened from inside WSL (`code .`). The CMake presets `host` and `avr` (`CMakePresets.json`) are picked up by CMake Tools.

## Toolchain

Verified versions (2026-10-21):

| Tool | Version | Purpose |
|---|---|---|
| `gcc` | 15.2 | host build (tests, simulation, master, daemon) |
| `cmake` | 4.2 | build system |
| `ninja` | 1.13 | build backend |
| `gcc-avr` | 14.2 | slave firmware |
| `avr-libc` | 2.2.1 | slave firmware |
| `avrdude` | 7.1 | flashing (ISP, UPDI via SerialUPDI) |
| `simavr` | 1.6 | slave simulation (classic tinyAVR only) |
| `libgpiod-dev` | 2.2.1 | master GPIO access (v2 API) |
| `libyaml-dev`, `libmosquitto-dev`, `mosquitto` | | daemon, and its end-to-end test |
| PlantUML | 1.2026.9 | timing diagrams (`tools/setup-docs.sh`) |
| mermaid-cli | 12.0.0 | checks the Mermaid diagrams (`tools/setup-docs.sh`) |

- `gcc-avr` 14.2 with `avr-libc` 2.2.1 supports classic tinyAVR and tinyAVR 0/1/2-series without Microchip device packs.
- `simavr` only simulates classic devices (for example ATtiny84/85); the 0/1/2-series HAL can be compiled but not simulated.
- The development host ships `libgpiod` v2. Whether the master also needs the v1 API depends on the Raspberry Pi OS version of the target ([open issues, H2](project/open-issues.md#h2-master-clock-implementation)).

## Build and Test

All commands run inside WSL from the repository root (from Windows: `wsl -d hubus --cd /mnt/c/Users/<user>/Projekte/hubus -- sh tools/build-host.sh`).

| Task | Command |
|---|---|
| Host build, unit tests, bus simulation (sanitizers, `-Werror`) | `sh tools/build-host.sh` |
| Slave firmware for the ATtiny85, with the flash/RAM budget check | `sh tools/build-avr.sh` |
| Slave firmware for another device | `sh tools/build-avr.sh attiny841` |
| Size reference firmware for the whole device matrix | `sh tools/build-avr-matrix.sh [mcu ...]` |
| Largest symbols of the reference firmware | `sh tools/sizes.sh [mcu]` |
| Sanity check of the test suite (deliberate bugs must be caught) | `sh tools/mutation-check.sh` |
| Only some mutants of the sanity check | `sh tools/run-mutants.sh "<name pattern>" ...` |
| One test with the bus trace on stderr | `HUBUS_SIM_TRACE=1 ./build/host/test/test_master` |
| The same, rebuilt, showing only the context of the first matches | `sh tools/run-trace.sh test_master [pattern] [lines]` |
| Render the timing diagrams and check the documentation | `sh tools/build-docs.sh` |

### Repository Layout

| Directory | Content |
|---|---|
| `core/` | Hardware-independent protocol code: CRC, frame codec, arbitration helpers, `GEN` evaluation, slave state machine |
| `slave/` | Slave library: public API and the HALs for classic tinyAVR, tinyAVR 0/1/2-series, and the host |
| `master/` | Master logic on a HAL, and the libgpiod HAL for the Raspberry Pi |
| `daemon/` | `hubusd` |
| `test/` | Unity tests, bus simulation (`test/sim/`), test support (`test/support/`), daemon tools (`test/tools/`) |
| `examples/` | Slave firmware examples (ATtiny85) and the master command line example |
| `docs/` | This documentation; `docs/diagrams/` holds the PlantUML sources and their SVGs |
| `cmake/` | AVR toolchain file, device defaults, size check |
| `tools/` | Setup, build, and check scripts |

### Tests

- `test_crc`, `test_frame`, `test_arb`, `test_gen`: the core modules (check value `0xDF`, header layout, exhaustive prefix matching, Hamming distance 4 for 12-byte payloads, arbitration against the example of [§5.6](spec/05-arbitration.md#56-example-four-pending-slaves) and random bursts, `GEN` evaluation).
- `test_slave_core`: the slave state machine with exact wire traffic from a bit-level master (`test/support/rawmaster.h`).
- `test_master`: the real master with real slave cores on the simulated wired-AND bus (startup, bursts, commands, commissioning, fragments, states).
- `test_faults`: fault injection (bit errors, lost and extra clock edges, lost STOP and START, stuck line, noise in the arbitration, an event racing a START, duplicate MCID), against [§8](spec/08-faults.md).
- `test_blind`: blind sections after a command ([§7.8](spec/07-slave.md#78-blind-sections)): a slave that ends its section at every possible point of foreign traffic (bursts with arbitration and handoff slots, state reads, commands) stays passive until the next START and disturbs nobody; the stale-level case the resynchronization prevents.
- `test_slave_lib`: the public slave API on the host HAL.
- `test_small_buffers`: the core with small compile-time buffers and without the state.
- `test_slave_passive`, `test_slave_command_only`: the core without the event buffer, with and without the state ([§7.4](spec/07-slave.md#74-optional-parts)).

In every master and fault test the simulation checks two invariants: a slave never changes `DATA` while `CLK` is high, and the master never undercuts its configured timing minimums.

### Daemon Tests

- `test_daemon_config`: YAML tree, configuration (defaults, every error class with line numbers, unknown keys, secrets), custom and built-in profiles, JSON writer.
- `test_daemon_core`: `hubusd` against the bus simulation and a fake MQTT client: startup, discovery, events, commands and their validation, readback, polling, availability, statistics, line faults, reconnect, reload, shutdown, messages from a second thread.
- `hubusd_example_config`: `hubusd --check` on `daemon/hubusd.example.yaml`.
- `hubusd_e2e` (`test/tools/e2e.sh`): a real mosquitto broker, `hubusd_sim` (the daemon on a simulated relay, button, and temperature sensor), and `mosquitto_pub`/`mosquitto_sub` as clients. Skipped if mosquitto is not installed.
- Still open: an end-to-end test on hardware with Home Assistant ([roadmap](../TODO.md)).

## Documentation

The documentation lives in `docs/` ([index](README.md)) and follows the conventions described there: every fact in one place, links instead of repetition, the specification numbered by section.

- **Mermaid** diagrams are written directly into the Markdown (` ```mermaid ` blocks) and rendered by GitHub. Stick to flowcharts, sequence diagrams, and state diagrams, which every renderer supports.
- **Timing diagrams** use PlantUML, because Mermaid has no timing diagram. Each one is a source file `docs/diagrams/<name>.puml`; the rendered `docs/diagrams/<name>.svg` is committed next to it and embedded in the Markdown as an image.
- `sh tools/build-docs.sh` renders every PlantUML diagram to its SVG, renders every Mermaid diagram (to check it), and checks every relative link and `#anchor` in all Markdown files (`tools/check-docs-links.py`). Run it after every documentation change and commit the updated SVGs.
- The CI runs `sh tools/build-docs.sh --check`, which also fails if a committed SVG does not match its source.
- The tools are installed in fixed versions by `sh tools/setup-docs.sh` into `~/.local/share/hubus-docs` (`HUBUS_DOCS_TOOLS` overrides it): the PlantUML jar and mermaid-cli with its headless browser. Java, Node.js, and the browser libraries come from `tools/setup-ubuntu.sh`.
