# hubusd: Operating the Bus from MQTT

`hubusd` is the HUBUS master as a service for the Raspberry Pi. It runs the bus, publishes events, states, details, and statistics of the slaves through MQTT, and executes commands that arrive over MQTT. Home Assistant finds the devices through MQTT discovery. The requirements are in `DAEMON_REQUIREMENTS.md`.

Example: a Raspberry Pi, an ATtiny as relay controller (MCID 3), and an ATtiny as push button (MCID 5).

## 1. Install and Start

```sh
cmake --preset host && cmake --build --preset host   # builds build/host/daemon/hubusd
sudo install build/host/daemon/hubusd /usr/local/bin/
sudo install -D daemon/hubusd.example.yaml /etc/hubus/hubusd.yaml
sudo install daemon/hubusd.service /etc/systemd/system/
sudo useradd -r -G gpio hubus
hubusd --check -c /etc/hubus/hubusd.yaml       # validates the file and prints warnings
sudo systemctl enable --now hubusd
journalctl -u hubusd -f
```

Build dependencies: `libyaml-dev`, `libmosquitto-dev` (and `libgpiod-dev` for the Raspberry Pi GPIO access). `tools/setup-ubuntu.sh` installs them.

Without hardware, `build/host/test/hubusd_sim -c config.yaml` runs the same daemon on a simulated bus with a relay (MCID 3), a button (5), a temperature sensor (7), and an unconfigured device (9). Console commands: `press`, `release`, `temp <raw>`, `unplug <mcid>`, `plug <mcid>`, `quit`.

## 2. Configuration

`/etc/hubus/hubusd.yaml`, see `daemon/hubusd.example.yaml` for every option. The file is validated at start; errors name the line and the daemon refuses to start, unknown keys are warnings. `SIGHUP` (`systemctl reload hubusd`) reloads the slave list and the profiles; GPIO, timing, and broker settings need a restart. The MQTT password can come from `password`, `password_file`, or the environment variable `HUBUSD_MQTT_PASSWORD` (which wins).

Minimal file for the example:

```yaml
gpio:
  clk: 17
  data: 27
mqtt:
  host: localhost
slaves:
  - mcid: 3
    name: Garden relay
  - mcid: 5
    name: Door button
```

Slaves are recognised from the first application byte of their `SLAVE_INFO` (device type 1 relay, 2 switch, 3 temperature, as in `examples/`), so `profile:` is optional for them. Custom firmware gets its own profile in the file (section 5).

## 3. Controlling and Reading Out (MQTT topics)

`hubus` is the configurable base topic. `<n>` is the MCID.

### Commands (subscribed)

| Topic | Payload | Effect |
|---|---|---|
| `hubus/slave/<n>/set` | `ON` / `OFF` | Command according to the profile (relay: opcode `0x10`). Afterwards the state is read back from the slave and published. |
| `hubus/slave/<n>/cmd` | `0x10 1 2` | Raw command: opcode, then argument bytes (numbers, spaces or commas). Opcode `0x00` (status capture) or `0x10..0xFF`. |
| `hubus/slave/<n>/capture` | any | `STATUS_CAPTURE`: the slave writes its status, the daemon publishes it. |
| `hubus/group/<prefix>/<len>/cmd` | as `cmd` | Group (`len` 0..5) or broadcast (`0/0`) command. Slaves never answer group commands; use opcode `0` to make a group publish its states. |
| `hubus/master/cmd/scan` | any | Commissioning scan; result on `hubus/master/scan`. |
| `hubus/master/cmd/stats` | any | Publish statistics now. |

Commands are never acknowledged by slaves. A published state is therefore always a value that was read from the slave, never the command that was sent. If a command was invalid or could not be executed, `hubus/master/log` says why and `commands_rejected` counts it.

### Published

| Topic | Retained | Content |
|---|---|---|
| `hubus/master/status` | yes | `online` / `offline` (last will) |
| `hubus/master/state` | yes | bus state (`ok`, `line_fault`), last error, version, configured and online slaves, **unknown slaves found on the bus** (not configured, therefore ignored) |
| `hubus/master/stats` | no | uptime, bursts, polls, requests, frames, duplicates, gaps, retries, skipped slaves, commands sent/rejected, errors by kind, recoveries, queue depth, publish errors |
| `hubus/master/diagnosis` | yes | `ok`, `suspected_duplicate_mcid` with the MCIDs, or `line_problem` |
| `hubus/master/scan` | no | result of the last scan |
| `hubus/master/log` | no | `{level, msg, ts}` for recoveries, offline slaves, rejected commands, unknown slaves |
| `hubus/slave/<n>/availability` | yes | `online` / `offline` |
| `hubus/slave/<n>/info` | yes | protocol version, capabilities, accepted command size, application bytes, device type, firmware, serial number, profile |
| `hubus/slave/<n>/state` | yes | interpreted value (`ON`/`OFF`, number) |
| `hubus/slave/<n>/data` | no | every received frame: hex bytes, `gen`, `more`, `result` (`baseline`, `normal`, `gap`, ...), `lost` buffer contents, timestamp |
| `hubus/slave/<n>/stats` | no | frames, duplicates, gaps, lost contents, CRC errors, missing responses, consecutive failures, last seen |

Try it:

```sh
mosquitto_sub -v -t 'hubus/#'
mosquitto_pub -t hubus/slave/3/set -m ON          # relay on; state ON follows once read back
mosquitto_pub -t hubus/slave/5/capture -n         # read the button state now
mosquitto_pub -t hubus/master/cmd/stats -n
```

A button press needs no request: the slave announces it on the bus, the daemon collects it, and `hubus/slave/5/state` becomes `ON`, with the frame on `hubus/slave/5/data`.

## 4. Home Assistant

With `mqtt.discovery: true` (default) the daemon publishes MQTT discovery messages (retained, under `homeassistant/`):

- relay profile: `switch`; switch profile: `binary_sensor`; temperature profile: `sensor` (device class `temperature`);
- each slave is a device with MCID, firmware version, and serial number, grouped under the **HUBUS master** device, available only while the master and the slave are online;
- the master device has diagnostic sensors: uptime, bus errors, retries, duplicate frames, lost buffer contents, rejected commands, bus state.

Slaves that are not in the configuration are not announced; they only show up in `hubus/master/state`. Removing a slave from the file and sending `SIGHUP` removes its entities. `mqtt.discovery_cleanup: true` additionally clears discovery entries of unconfigured MCIDs at every start.

## 5. Device Profiles

The bus protocol does not define what payload bytes mean, so the daemon interprets them with a profile: built in `relay` (device type 1), `switch` (2), `temperature` (3), or your own in the `profiles:` section:

```yaml
profiles:
  dimmer:
    component: switch              # switch | binary_sensor | sensor
    device_type: 0x20              # first application byte of SLAVE_INFO
    on:  {opcode: 0x30, args: [255]}
    off: {opcode: 0x30, args: [0]}
  pressure:
    component: sensor
    byte_offset: 0
    byte_length: 2
    byte_order: big
    scale: 0.1                     # value = raw * scale + offset
    offset: 900
    decimals: 1
    unit: hPa
    device_class: pressure
```

A slave without a profile stays fully usable through the raw topics (`data`, `cmd`, `capture`). The built-in temperature profile converts the raw ADC value of the ATtiny85 internal sensor with `raw - 273` degrees Celsius; that needs a per-device calibration for accurate values.

## 6. How the Daemon Works

- **Bus thread:** the only thread that touches GPIO. It handles queued commands (at most four per loop), then waits for idle events (a slave announcing data), collects them in a burst, and publishes. The MQTT thread only validates messages and puts them in a bounded queue (32 entries); a full queue is counted in `queue_full` and never blocks the bus.
- **Availability:** slaves do not speak unless asked, so the daemon checks every configured slave with `REQUEST_INFO` every `bus.ping_interval_s` (one slave per loop). After `bus.ping_failures` misses a slave is `offline`; when it answers again it is `online` and its state is read with `STATUS_CAPTURE`. Valid data frames also count as proof of life.
- **Startup:** startup sequence with RESET, probe of all configured slaves, scan for unconfigured ones, a burst for data that was pending, and a state read of every online slave.
- **Faults:** a stuck `DATA` line puts the bus in `line_fault` (published with the error); commands are rejected meanwhile and the startup sequence is retried every `bus.startup_retry_s`. CRC failures are retried within the retry budget; repeated failures of one MCID raise `suspected_duplicate_mcid` in the diagnosis.
- **Reconnect:** the MQTT client reconnects by itself; on every connection the retained messages (status, state, info, discovery) are published again.
- **Shutdown:** `SIGTERM`/`SIGINT` publish `offline`, generate a RESET on the bus, and release the GPIO lines.

## 7. Limits

- Latency is bus latency: milliseconds to a few hundred milliseconds, depending on the timing configuration (defaults are conservative placeholders until measured on hardware, `OPEN_ISSUES.md` H3).
- The daemon reports only what the protocol carries: `SLAVE_INFO` and the buffer contents. Counters inside a slave appear only if the slave firmware writes them into its buffer.
- Only the libgpiod v2 API is supported for the GPIO access (`OPEN_ISSUES.md` H2).
- Not yet run on a Raspberry Pi with real slaves; tested against the bus simulation (unit tests, and an end-to-end test with a real mosquitto broker: `test/tools/e2e.sh`).
