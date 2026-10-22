# Development Environment

## 1. Overview

Development runs in WSL2 with Ubuntu 26.04 LTS on Windows. The same Ubuntu setup builds the host tests, the bus simulation, the master, and the AVR slave firmware.

## 2. WSL Distribution

Create a dedicated distribution (PowerShell):

```powershell
wsl --install Ubuntu-26.04 --name hubus
```

Then, inside the distribution, install the toolchain:

```sh
sudo sh tools/setup-ubuntu.sh
```

The repository can stay on the Windows file system and is reachable from WSL at `/mnt/c/...`. Builds are small, so the slower cross-file-system access is acceptable. Recommended Git settings inside WSL:

```sh
git config --global core.autocrlf input
git config --global --add safe.directory /mnt/c/Users/<user>/Projekte/hubus
```

## 3. Toolchain

Verified versions (2026-10-21):

| Tool | Version | Purpose |
|---|---|---|
| `gcc` | 15.2 | host build (tests, simulation, master) |
| `cmake` | 4.2 | build system |
| `ninja` | 1.13 | build backend |
| `gcc-avr` | 14.2 | slave firmware |
| `avr-libc` | 2.2.1 | slave firmware |
| `avrdude` | 7.1 | flashing (ISP, UPDI via SerialUPDI) |
| `simavr` | 1.6 | slave simulation (classic tinyAVR only) |
| `libgpiod-dev` | 2.2.1 | master GPIO access (v2 API) |

Notes:

- `gcc-avr` 14.2 with `avr-libc` 2.2.1 supports classic tinyAVR and tinyAVR 0/1/2-series without Microchip device packs. Verified for ATtiny25, 85, 84, 88, 841, 202, 1614, 1624, and 3216.
- `simavr` only simulates classic devices (for example ATtiny84/85). The 0/1/2-series HAL can be compiled but not simulated.
- The development host ships `libgpiod` v2. The Raspberry Pi OS version on the target decides whether the master also needs the v1 API (`OPEN_ISSUES.md` H2).

## 4. Editor

VS Code with the extensions `ms-vscode-remote.remote-wsl`, `ms-vscode.cpptools`, and `ms-vscode.cmake-tools`. Open the repository from inside WSL:

```sh
code .
```

## 5. Build and Test

All commands run inside the WSL distribution from the repository root (from Windows: `wsl -d hubus --cd /mnt/c/Users/<user>/Projekte/hubus -- sh tools/build-host.sh`).

| Task | Command |
|---|---|
| Host build, unit tests, bus simulation (sanitizers, `-Werror`) | `sh tools/build-host.sh` |
| Slave firmware for the ATtiny85 incl. flash/RAM budget check | `sh tools/build-avr.sh` |
| Slave firmware for another device | `sh tools/build-avr.sh attiny841` |
| Size reference firmware for the whole device matrix | `sh tools/build-avr-matrix.sh [mcu ...]` |
| Largest symbols of the reference firmware | `sh tools/sizes.sh [mcu]` |
| Sanity check of the test suite (deliberate bugs must be caught) | `sh tools/mutation-check.sh` |
| One test with the bus trace on stderr | `HUBUS_SIM_TRACE=1 ./build/host/test/test_master` |

The same build is available through CMake presets (`host`, `avr`; see `CMakePresets.json`), so VS Code with the CMake Tools extension picks them up.

### Repository Layout

| Directory | Content |
|---|---|
| `core/` | Hardware-independent protocol code: CRC, frame codec, arbitration helpers, `GEN` evaluation, slave state machine |
| `slave/` | Slave library (public API, HAL for classic tinyAVR, tinyAVR 0/1/2-series, and the host) |
| `master/` | Master logic on a HAL, and the libgpiod HAL for the Raspberry Pi |
| `test/` | Unity tests, bus simulation (`test/sim/`), test support (`test/support/`) |
| `examples/` | Slave firmware examples (ATtiny85) and the master command line example |
| `cmake/` | AVR toolchain file, device defaults, size check |
| `tools/` | Setup and helper scripts |

### Tests

- `test_crc`, `test_frame`, `test_arb`, `test_gen`: unit tests of the core modules (check value `0xDF`, header layout, exhaustive prefix matching, Hamming distance 4 for 12-byte payloads, arbitration against the spec example and random bursts, `GEN` evaluation).
- `test_slave_core`: the slave state machine with exact wire traffic from a bit-level master (`test/support/rawmaster.h`).
- `test_master`: the real master with real slave cores on the simulated wired-AND bus (startup, bursts, commands, commissioning with group requests, fragments).
- `test_faults`: fault injection (bit errors, lost and extra clock edges, lost STOP/START, stuck line, noise in arbitration, event racing a START, duplicate MCID).
- `test_slave_lib`: the public slave API on the host HAL.
- `test_small_buffers`: the core compiled with small compile-time buffers.
- `test_slave_passive`, `test_slave_command_only`: the core compiled without the event buffer, with and without the state (`SLAVE_PROPERTIES.md` section 4.2).

The simulation checks two invariants in every master/fault test: a slave never changes `DATA` while `CLK` is high, and the master never undercuts its configured timing minimums.

### Daemon Tests

- `test_daemon_config`: YAML tree, configuration (defaults, every error class with line numbers, unknown keys, secrets), custom and built-in profiles, JSON writer.
- `test_daemon_core`: `hubusd` against the bus simulation and a fake MQTT client: startup, discovery, events, commands and their validation, read-back, availability, statistics, line faults, reconnect, reload, shutdown, messages from a second thread.
- `hubusd_example_config`: `hubusd --check` on `daemon/hubusd.example.yaml`.
- `hubusd_e2e` (`test/tools/e2e.sh`): a real mosquitto broker, `hubusd_sim` (the daemon on a simulated relay, button, and temperature sensor), and `mosquitto_pub`/`mosquitto_sub` as clients. Skipped if mosquitto is not installed. `build/host/test/hubusd_sim` can also be run by hand for experiments (`docs/DAEMON.md`).
