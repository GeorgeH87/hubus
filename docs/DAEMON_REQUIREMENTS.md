# Master Daemon (`hubusd`) Requirements

Status: implemented (`daemon/`), see `DAEMON.md` for operation. Not yet run on real hardware. Decisions taken: configuration in YAML (`libyaml`), slaves that are on the bus but not configured are ignored and listed in `hubus/master/state`. Implementation notes: slave availability is tracked with periodic `REQUEST_INFO` (`bus.ping_interval_s`), because slaves only answer when asked; discovery entries are removed on reload (and optionally at start with `mqtt.discovery_cleanup`).

## 1. Purpose

`hubusd` is a long-running Linux service on the Raspberry Pi that operates the HUBUS bus and exposes it through MQTT. It makes slaves usable from Home Assistant and other MQTT clients: events from slaves (for example a button press), commands to slaves (for example switching a relay), slave details, and technical statistics of the master and the bus.

Example setup: one Raspberry Pi, one ATtiny as relay controller, one ATtiny as push button.

## 2. Constraints

- Written in C, built on the existing master library (`master/`) and the libgpiod HAL. No new language runtime.
- MQTT client: `libmosquitto`.
- The configuration format must be natural for Home Assistant users (section 8).
- The MQTT layer sits behind a thin interface so that the daemon logic can be tested against the bus simulation and a fake MQTT client, without broker and hardware.
- Home Assistant discovery follows the MQTT discovery conventions of Home Assistant, so slaves appear without manual entity configuration.
- The bus is slow and serves one transaction at a time. Latency is in the range of milliseconds to a few hundred milliseconds; the daemon makes no real-time promises towards MQTT clients.

## 3. Architecture

- **Bus thread:** the only thread that touches GPIO and the master library. It runs the service loop (idle event handshake, burst), executes queued commands, and collects statistics.
- **MQTT thread:** `libmosquitto` network loop. It translates incoming messages into queued commands and publishes events and statistics produced by the bus thread.
- Commands travel through a bounded queue. A full queue rejects the command and reports it on the log topic; it never blocks the bus thread.
- Between bursts the bus thread executes pending commands. A burst is never interrupted.
- Real-time priority for the bus thread is configurable.

## 4. Functional Requirements

### 4.1 Bus operation

- F1: At start the daemon runs the master startup sequence (RESET) and a full burst, then serves idle events continuously.
- F2: Every received `SLAVE_DATA` frame is published (section 5). Duplicates detected through `GEN` are discarded, gaps are reported.
- F3: On bus errors the daemon uses the library recovery. A hardware line fault (`DATA` stuck low) is reported and the daemon retries the startup sequence at a configurable interval instead of exiting.
- F4: On shutdown (SIGTERM, SIGINT) the daemon generates a RESET, publishes `offline`, and releases the GPIO lines.
- F5: Slaves that are configured but do not answer are marked unavailable. Slaves that appear on the bus but are not configured are reported and, if enabled, announced with default (raw) topics.

### 4.2 Commands

- F6: Send an application command to one slave, a prefix group, or all slaves (`TARGET_LEN` 0..6) with opcode and argument bytes.
- F7: Trigger `STATUS_CAPTURE` for one slave, a group, or all.
- F8: After a command that changes state, the daemon reports the state according to the configurable `readback` mode (global and per slave): `slave` (default) triggers `STATUS_CAPTURE` and publishes the value read from the slave; `cache` publishes the state the command establishes without contacting the slave again; `none` publishes nothing. Commands are never acknowledged by slaves, so only `slave` mode proves that the state was established.
- F9: Commissioning scan on request (discovery polls, then `REQUEST_INFO` to each slave found), including the suspected-duplicate-MCID result.
- F10: Raw request of `SLAVE_INFO` and of the buffer of one slave on request.

### 4.3 Slave details

- F11: For every slave seen: MCID, protocol version, capability flags, accepted command size, application info bytes (device type, firmware version, serial number), availability, time of the last frame.
- F12: Application bytes of `SLAVE_INFO` are interpreted through the device profiles of section 7.

### 4.4 Statistics and diagnostics

- F13: Master statistics, published periodically and on request: uptime, bursts, polls, frames served, duplicates, gaps, retries, skipped slaves, transactions by type, errors by kind (no response, CRC, mismatch, line, line fault), recoveries, queue depth, commands sent and rejected.
- F14: Per-slave statistics: frames received, duplicates, gaps (lost buffer contents), CRC failures, consecutive failures, last seen.
- F15: Diagnosis from `hubus_master_diagnose()`: suspected duplicate MCIDs and suspected line problem, published as status and as a log entry.
- F16: Measured bus timing is out of scope until hardware measurements exist (`docs/OPEN_ISSUES.md` H2, H3). The timing minimums are configurable.

### 4.5 Reporting limits

The daemon can only report what the protocol carries: `SLAVE_INFO` and the buffer contents. Internal counters or error state of a slave are visible only if the slave firmware writes them into its buffer.

## 5. MQTT Interface

Base topic `hubus` (configurable). Payloads are UTF-8 JSON unless stated otherwise. Availability uses retained messages and an MQTT last will.

| Topic | Direction | Retained | Content |
|---|---|---|---|
| `hubus/master/status` | publish | yes | `online` / `offline` (last will) |
| `hubus/master/state` | publish | yes | bus state, last error, daemon version, uptime |
| `hubus/master/stats` | publish | no | statistics of F13, every `stats_interval` |
| `hubus/master/diagnosis` | publish | yes | result of the duplicate / line problem heuristic |
| `hubus/master/log` | publish | no | notable events (recovery, unknown slave, rejected command) |
| `hubus/master/cmd/scan` | subscribe | | start a commissioning scan; result on `hubus/master/scan` |
| `hubus/master/cmd/stats` | subscribe | | publish statistics now |
| `hubus/slave/<mcid>/availability` | publish | yes | `online` / `offline` |
| `hubus/slave/<mcid>/info` | publish | yes | SLAVE_INFO fields and interpreted device type |
| `hubus/slave/<mcid>/data` | publish | no | every received frame: bytes, `gen`, `more`, timestamp, gap/duplicate flags |
| `hubus/slave/<mcid>/state` | publish | yes | last interpreted value (via device profile) |
| `hubus/slave/<mcid>/stats` | publish | no | per-slave statistics of F14 |
| `hubus/slave/<mcid>/set` | subscribe | | command according to the device profile, for example `ON` / `OFF` |
| `hubus/slave/<mcid>/cmd` | subscribe | | raw command: opcode and argument bytes |
| `hubus/slave/<mcid>/capture` | subscribe | | trigger `STATUS_CAPTURE` |
| `hubus/group/<prefix>/<len>/cmd` | subscribe | | group or broadcast command (`len` 0..5); one-way, no responses |

Requirements:

- M1: Every command is validated (MCID range, payload size against the maximum announced by the slave, valid `TARGET_LEN`). Invalid commands are rejected with an entry on `hubus/master/log`.
- M2: Slave state is published retained so that clients get the last known value after a reconnect.
- M3: The daemon reconnects to the broker automatically and republishes retained state after a reconnect.
- M4: Broker authentication (user, password) and TLS are configurable.
- M5: Home Assistant discovery messages (`homeassistant/<component>/<node>/<object>/config`) are published retained for each slave according to its device profile and removed again when a slave is removed from the configuration. Discovery can be disabled.

## 6. Home Assistant Integration

- Relay profile: `switch` entity (state from `state`, command to `set`).
- Button profile: `binary_sensor` entity, optionally an `event` entity for presses.
- Temperature profile: `sensor` entity with device class `temperature`.
- The master itself appears as a device with diagnostic sensors (uptime, error counters, bus state, diagnosis).
- Every slave appears as a device with MCID, firmware version, and serial number from `SLAVE_INFO`, grouped under the master device.

## 7. Device Profiles

The protocol does not define the meaning of payload bytes; that lives in the slave firmware. A device profile tells the daemon how to interpret a slave.

- P1: Built-in profiles for the three example firmwares in `examples/` (relay, switch, temperature sensor). The device type is taken from the first application byte of `SLAVE_INFO`, so scanned slaves are recognised automatically.
- P2: A profile defines how `SLAVE_DATA` bytes become a value (for example byte 0 as 0/1, or bytes 0..1 as a big-endian raw ADC value with scale and offset), which Home Assistant component to use, and how `set` payloads map to opcodes and arguments.
- P3: Profiles for custom firmware can be defined in the configuration file without recompiling.
- P4: A slave without a known profile is still fully usable through the raw topics (`data`, `cmd`, `capture`).

## 8. Configuration

- C1: One configuration file, default `/etc/hubus/hubusd.yaml`, path overridable on the command line.
- C2: Format: **YAML**, because it is the native configuration format of Home Assistant and its users already know it. It needs a small parser dependency (`libyaml`); JSON would avoid the dependency but is less comfortable to edit. To be confirmed before implementation.
- C3: Content: GPIO chip label and line offsets, real-time priority, timing minimums, retry budget, MQTT broker (host, port, credentials, TLS, base topic, discovery prefix, client id), statistics interval, list of slaves (MCID, name, profile, options such as automatic read-back after `set`), custom profiles.
- C4: The file is validated at start. Errors are reported with line numbers and the daemon refuses to start with an invalid configuration. Unknown keys are reported as warnings.
- C5: Secrets can be read from a separate file or from environment variables, so that the main file can be shared.
- C6: Reload on SIGHUP for the slave list and profiles; changes to GPIO or timing settings require a restart.

## 9. Non-Functional Requirements

- N1: Runs as a systemd service (unit file included) under an unprivileged user with access to `/dev/gpiochip*`; real-time priority via capabilities rather than root.
- N2: Logging to stderr/journald with levels; the MQTT log topic mirrors important events only.
- N3: Memory use independent of bus activity (no unbounded queues, no per-frame heap allocation in the bus thread).
- N4: Works on Raspberry Pi 3 and newer including Pi 5 (same targets as the master).
- N5: Built with the existing CMake setup as an optional target; a missing `libmosquitto` or `libyaml` does not affect the library build.

## 10. Test Requirements

- T1: The daemon logic (queue, command validation, topic mapping, profiles, statistics, availability state machine) is tested against the bus simulation and a fake MQTT client, in the host test preset with sanitizers.
- T2: Fault injection scenarios from the existing simulation are reused to check what is published (stuck line, CRC errors, duplicate frames, lost slave).
- T3: Configuration parsing has tests for valid files, every error class, and unknown keys.
- T4: Discovery messages are checked against the expected JSON for each built-in profile.
- T5: A manual end-to-end test on real hardware with a broker is described in the documentation (Raspberry Pi, relay ATtiny, button ATtiny).

## 11. Open Decisions

- Configuration format: YAML (proposed) versus JSON or TOML (C2).
- Default behavior for unknown slaves found by a scan (announce with raw topics, or ignore).
- Whether the daemon should additionally expose a local control interface besides MQTT (not planned; MQTT covers all requirements).
