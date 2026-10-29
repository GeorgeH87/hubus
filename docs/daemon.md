# hubusd: the Bus as an MQTT Service

`hubusd` is the HUBUS master as a long-running Linux service on the Raspberry Pi, in `daemon/`. It runs the bus, publishes events, states, device details, and statistics through MQTT, and executes commands that arrive over MQTT. Home Assistant finds the devices through MQTT discovery.

It is written in C on top of the master library (`master/`) and its libgpiod HAL, uses `libmosquitto` for MQTT and `libyaml` for the configuration, and makes no real-time promises towards MQTT clients: latency is bus latency, milliseconds to a few hundred milliseconds.

Example setup: a Raspberry Pi, a relay (MCID 3), and a push button (MCID 5).

## Install and Start

```sh
cmake --preset host && cmake --build --preset host   # builds build/host/daemon/hubusd
sudo install build/host/daemon/hubusd /usr/local/bin/
sudo install -D daemon/hubusd.example.yaml /etc/hubus/hubusd.yaml
sudo install daemon/hubusd.service /etc/systemd/system/
sudo useradd -r -G gpio hubus
hubusd --check -c /etc/hubus/hubusd.yaml       # validates the file and prints warnings
hubusd --version                               # prints the version; -v: verbose logging (info messages)
sudo systemctl enable --now hubusd
journalctl -u hubusd -f
```

The service runs as an unprivileged user with access to `/dev/gpiochip*`; real-time priority comes from the capability `CAP_SYS_NICE` in the unit file, not from root. Build dependencies: `libyaml-dev`, `libmosquitto-dev`, and `libgpiod-dev`; `tools/setup-ubuntu.sh` installs them. The daemon is an optional build target: without these libraries the libraries and tests still build.

Without hardware, `build/host/test/hubusd_sim -c config.yaml` runs the same daemon on a simulated bus with a relay (MCID 3), a button (5), a temperature sensor (7), and an unconfigured device (9). Console commands: `press`, `release`, `temp <raw>`, `unplug <mcid>`, `plug <mcid>`, `quit`.

## Configuration

One YAML file, `/etc/hubus/hubusd.yaml` by default (`-c` overrides it); `daemon/hubusd.example.yaml` documents every option. It holds the GPIO chip and lines, the real-time priority, the timing minimums and retry budget of the master, the MQTT broker (host, port, credentials, TLS, base topic, discovery prefix, client id), intervals, the slaves, and custom profiles.

- The file is validated at start. Errors name the line, and the daemon refuses to start; unknown keys are warnings.
- The MQTT password can come from `password`, `password_file`, or the environment variable `HUBUSD_MQTT_PASSWORD` (which wins), so the main file can be shared.
- `SIGHUP` (`systemctl reload hubusd`) reloads the slave list, the profiles, the `bus` section (readback, intervals), `mqtt.stats_interval_s`, and `mqtt.discovery`. GPIO, timing, and the broker connection need a restart.

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

### Timing

The `timing` section configures the clock and the retry behaviour of the master ([§1.4](spec/01-physical-layer.md#14-timing-parameters)). All times are **minimums** in microseconds; the bus can only get slower than configured, never faster.

| Key | Default | Range | Meaning |
|---|---|---|---|
| `clk_low_us` | 50 | 1 to 10000000 | Minimum `CLK` low time (`T_CLK_LOW`). |
| `clk_high_us` | 50 | 1 to 10000000 | Minimum `CLK` high time (`T_CLK_HIGH`). |
| `sample_us` | 25 | 1 to `clk_high_us` | Sample point of `DATA` after the rising edge (`T_SAMPLE`). A larger value than `clk_high_us` is a configuration error. |
| `cond_us` | 50 | 1 to 10000000 | `CLK` high time around the `DATA` change of START and STOP (`T_COND`). |
| `turnaround_us` | 200 | 1 to 10000000 | `CLK` low time after the turnaround slot (`T_TURNAROUND`); raised to `clk_low_us` if smaller. |
| `line_fault_ticks` | 300 | 280 to 65000 | Clock ticks of a `DATA` line stuck low before it is a line fault; at least one maximum frame. |
| `retry_budget` | 2 | 0 to 20 | Retries of a failed `REQUEST_DATA` within a burst. |
| `follow_fragments` | true | true, false | Request the next fragment right after `MORE = 1`. |
| `max_fragments` | 32 | 1 to 255 | Consecutive fragments of one slave per burst. |
| `duplicate_threshold` | 3 | 1 to 100 | Consecutive CRC failures of one MCID that make it a suspect for a duplicate MCID. |
| `line_problem_count` | 3 | 1 to 100 | If more MCIDs than this are suspects at the same time, the diagnosis is a line problem instead of a duplicate MCID. |

The clock period is `clk_low_us + clk_high_us`: 100 µs with the defaults, which is about 10 kbit/s on the wire. The throughput is lower because of the turnaround slots, the arbitration, and the line conditions between transactions. A bit-banged clock in user space only meets the minimums; with `gpio.realtime` set, the scheduling jitter stays small and the real period stays close to the configured one.

To adapt the bus:

- **Slower slaves, long wires, or interference:** raise `clk_low_us` (the slave needs it for its interrupt latency and for updating `DATA`) and `turnaround_us`. Slaves running at 1 MHz are the usual limit; the values must suit the slowest slave on the bus.
- **Faster bus:** lower `clk_low_us`, `clk_high_us`, and `sample_us` together, keeping `sample_us <= clk_high_us`, and watch the statistics for CRC errors ([Statistics and Diagnosis](#statistics-and-diagnosis)).
- The defaults are conservative placeholders until measured on hardware ([open issues](project/open-issues.md#h3-concrete-timing-values)). Changes need a restart of the daemon.

The `gpio` section selects the chip by label (`chip`, preferred) or device path (`path`), the offsets of `clk` and `data` (different lines), `realtime` (`SCHED_FIFO` priority 1 to 99, 0 = off, needs `CAP_SYS_NICE`), and `internal_pullup` (the SoC pull-up on `DATA`, for bench tests only; use an external pull-up).

Slaves are recognised from the first application byte of their `SLAVE_INFO` (device type 1 relay, 2 switch, 3 temperature, as in the [examples](slave-library.md#examples)), so `profile:` is optional for them. Slaves on the bus that are not configured are ignored: they are logged and listed in `hubus/master/state`, but get no topics and no discovery entries.

## MQTT Topics

`hubus` is the configurable base topic, `<n>` the MCID. Payloads are UTF-8, JSON unless stated otherwise.

### Commands (subscribed)

| Topic | Payload | Effect |
|---|---|---|
| `hubus/slave/<n>/set` | `ON` / `OFF` | Command according to the profile (relay: opcode `0x10`). Afterwards the state is published according to `readback` (below). |
| `hubus/slave/<n>/cmd` | `0x10 1 2` | Raw command: opcode, then argument bytes (numbers, separated by spaces or commas). Opcode `0x00` (status capture) or `0x10..0xFF`. |
| `hubus/slave/<n>/capture` | any | `STATUS_CAPTURE`: the slave writes its status, the daemon collects and publishes it. |
| `hubus/slave/<n>/read` | any | `REQUEST_STATE`: reads the live state of a slave that has one in one transaction and publishes it. Rejected for slaves without a state. |
| `hubus/group/<prefix>/<len>/cmd` | as `cmd` | Group (`len` 0..5) or broadcast (`0/0`) command. Slaves never answer group commands; opcode `0` makes a group capture its states, which a burst then collects. |
| `hubus/master/cmd/scan` | any | Commissioning scan ([§6.4](spec/06-operation.md#64-commissioning-scan-and-duplicate-mcids)); result on `hubus/master/scan`. |
| `hubus/master/cmd/probe` | MCID `1..63` | One `REQUEST_INFO` to a single MCID, configured or not; result on `hubus/master/probe`. There is no function that polls all 63 MCIDs. |
| `hubus/master/cmd/stats` | any | Publish the statistics now. |

Every command is validated (MCID range, `TARGET_LEN`, payload size against the maximum the slave announces). Slaves never acknowledge commands; an invalid command or one that cannot be executed is reported on `hubus/master/log` and counted in `commands_rejected`.

### Published

| Topic | Retained | Content |
|---|---|---|
| `hubus/master/status` | yes | `online` / `offline` (last will) |
| `hubus/master/state` | yes | bus state (`ok`, `line_fault`), last error, version, configured and online slaves, unknown slaves on the bus |
| `hubus/master/stats` | no | statistics (below), every `mqtt.stats_interval_s` and on request |
| `hubus/master/diagnosis` | yes | `ok`, `suspected_duplicate_mcid` with the MCIDs, or `line_problem` |
| `hubus/master/scan` | no | result of the last scan |
| `hubus/master/probe` | no | result of the last probe: `mcid`, `state` (`present`, `absent`, `suspect` after repeated CRC failures), `configured`, device type, `state_capable` |
| `hubus/master/log` | no | `{level, msg, ts}` for recoveries, offline slaves, rejected commands, unknown slaves |
| `hubus/slave/<n>/availability` | yes | `online` / `offline` |
| `hubus/slave/<n>/info` | yes | protocol version, capabilities (`events`, `status_capture`, `state`, `update`: can switch to its updater, `updater`: the updater runs), speed class (`speed_class`, [§1.5](spec/01-physical-layer.md#15-speed-classes)), blind class (`blind_class`, [§7.8](spec/07-slave.md#78-blind-sections)), accepted command size, application bytes, device type, firmware, serial number, profile |
| `hubus/slave/<n>/state` | yes | interpreted value (`ON`/`OFF`, a number), so clients get the last value after a reconnect |
| `hubus/slave/<n>/data` | no | every received frame except duplicates: hex bytes, `gen`, `more`, `result` (`baseline`, `normal`, `wrapped`, `gap`, or `state` for a state read), `lost` buffer contents, timestamp |
| `hubus/slave/<n>/stats` | no | frames, duplicates, gaps, lost contents, CRC errors, missing responses, consecutive failures, last seen |

Try it:

```sh
mosquitto_sub -v -t 'hubus/#'
mosquitto_pub -t hubus/slave/3/set -m ON          # relay on; state ON follows once read back
mosquitto_pub -t hubus/slave/5/capture -n         # read the button state now (STATUS_CAPTURE)
mosquitto_pub -t hubus/slave/7/read -n            # read the live state of the sensor (REQUEST_STATE)
mosquitto_pub -t hubus/master/cmd/probe -m 12     # is anybody at MCID 12?
mosquitto_pub -t hubus/master/cmd/stats -n
```

A button press needs no request: the slave announces it, the daemon collects it in a burst, and `hubus/slave/5/state` becomes `ON`, with the frame on `hubus/slave/5/data`.

## State After a Command (`readback`)

What `hubus/slave/<n>/state` shows after a `set` command is configured globally with `bus.readback` and per slave with `readback:`.

```mermaid
flowchart TD
    cmd(["set command sent"]) --> mode{"readback mode"}
    mode -- none --> nothing["publish nothing"]
    mode -- cache --> cache["publish the commanded state from the cache"]
    mode -- "slave (default)" --> state{"slave has a state?"}
    state -- yes --> read["REQUEST_STATE, publish the answer"]
    state -- no --> capture{"STATUS_CAPTURE supported?"}
    capture -- yes --> burst["STATUS_CAPTURE and a burst, publish the answer"]
    capture -- no --> cache
```

| Mode | Behavior |
|---|---|
| `slave` (default) | The daemon reads the answer from the slave: `REQUEST_STATE` in one transaction if the slave has a state, otherwise `STATUS_CAPTURE` and a burst (two more transactions). The published state is what the slave reports, so a defective slave shows its real state. A slave with neither ([§7.4](spec/07-slave.md#74-optional-parts)) cannot be read back and is treated as `cache`. |
| `cache` | The slave is not contacted again; the daemon publishes the state the command establishes. Fast and without extra bus traffic, but optimistic: a command that did not arrive or was not executed stays unnoticed ([§8.8](spec/08-faults.md#88-commands-without-readback)) until the next event, refresh, or capture. |
| `none` | Nothing is published after the command; the state changes only through events, reads, and captures. |

Raw commands (`cmd` topic) have no known effect and publish no state in `cache` mode. `true`/`false` are accepted as `slave`/`none`.


### Slaves That Are Blind After a Command

A slave with a blind class in `SLAVE_INFO` may ignore the bus for a while after every command to it, for example while it sends a frame to an LED strip ([§7.8](spec/07-slave.md#78-blind-sections)). The daemon takes the class from `SLAVE_INFO`; nothing has to be configured.

- After a command to such a slave, exact or through a group, the daemon waits the blind time of its class before it contacts the slave again ([§6.9](spec/06-operation.md#69-blind-time-after-a-command)). The other slaves are served meanwhile.
- With `readback: slave` the readback follows when the blind time is over; `cache` and `none` behave as usual.
- Queued commands, reads, captures, and probes for the slave wait in order; a group command or scan that would include it waits as well. Pings and periodic reads of it are postponed.
- When the time is over, the daemon generates a RESET, which brings the slave back, and then reads it back.
- A `STATUS_CAPTURE` is a command too: the captured status of such a slave arrives with its next announcement after the blind time.

### Slaves That Never Announce Changes (`poll_interval_s`)

A slave without an event buffer, for example a sensor that is only read, never announces a new value ([§6.3](spec/06-operation.md#63-reading-states)). With `poll_interval_s: <seconds>` in its slave entry the daemon reads its state periodically (`REQUEST_STATE`, or `STATUS_CAPTURE` and a burst for a slave without a state) and publishes it. With the default `0` the state is read only at startup, after an outage, after commands, and on the `read` topic.

## Home Assistant

With `mqtt.discovery: true` (default) the daemon publishes retained discovery messages under `<discovery prefix>/<component>/<object>/config` (prefix `homeassistant` by default):

- relay profile: `switch`; switch profile: `binary_sensor`; temperature profile: `sensor` with device class `temperature`;
- each slave is a device with MCID, firmware version, and serial number, grouped under the **HUBUS master** device and available only while master and slave are online;
- the master device has diagnostic sensors: uptime, bus errors, retries, duplicate frames, lost buffer contents, rejected commands, bus state. The diagnosis is on `hubus/master/diagnosis`.

Removing a slave from the configuration and sending `SIGHUP` removes its entities. `mqtt.discovery_cleanup: true` also clears discovery entries of unconfigured MCIDs at every start.

## Device Profiles

The protocol does not define what payload bytes mean, so the daemon interprets them with a profile: the built-in `relay` (device type 1), `switch` (2), and `temperature` (3), or custom profiles in the `profiles:` section, without recompiling:

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

A slave without a profile stays usable through the raw topics (`data`, `cmd`, `capture`). The built-in temperature profile converts the raw ADC value of the ATtiny85 internal sensor as `raw - 273` °C; accurate values need a per-device calibration.

## How It Works

```mermaid
flowchart LR
    broker[("MQTT broker")]
    subgraph hubusd
        mqtt["MQTT thread<br/>libmosquitto"]
        queue["command queue<br/>32 entries"]
        bus["bus thread"]
        master["master library"]
    end
    gpio["CLK and DATA<br/>libgpiod"]
    broker -- commands --> mqtt -- validated --> queue --> bus --> master --> gpio
    bus -- "events, states, statistics" --> broker
```

- **Bus thread:** the only thread that touches GPIO and the master library. It executes queued commands (at most four per loop), then serves idle events with bursts, and publishes events, states, and statistics itself through the thread-safe MQTT client. A burst is never interrupted.
- **MQTT thread:** the `libmosquitto` network loop. It validates incoming messages and puts them into the bounded command queue. A full queue drops the command and counts it in `queue_full`; it never blocks the bus thread.
- **Startup:** startup sequence with RESET ([§6.6](spec/06-operation.md#66-startup-shutdown-and-recovery)), a probe of every configured slave, a scan for unconfigured ones ([§6.4](spec/06-operation.md#64-commissioning-scan-and-duplicate-mcids)), a burst for data that was pending, and a state read of every online slave.
- **Availability:** slaves only speak when asked, so the daemon checks every configured slave with `REQUEST_INFO` every `bus.ping_interval_s` (one slave per loop). After `bus.ping_failures` misses a slave is `offline`; when it answers again it is `online`, and its state is read. Valid data frames also count as a sign of life.
- **States and events:** states are read at startup, after an outage, after commands (`readback`), on the `read` topic, and with `poll_interval_s`; events arrive through bursts ([§6.2](spec/06-operation.md#62-burst)). Only slaves with an event buffer take part in a burst.
- **Faults:** a stuck `DATA` line puts the bus into `line_fault` (published with the error); commands are rejected meanwhile, and the startup sequence is retried every `bus.startup_retry_s`. CRC failures are retried within the retry budget; repeated failures of one MCID raise `suspected_duplicate_mcid` in the diagnosis ([§6.4](spec/06-operation.md#64-commissioning-scan-and-duplicate-mcids)).
- **Reconnect:** the MQTT client reconnects by itself and publishes the retained messages (status, state, info, discovery) again on every connection.
- **Shutdown:** `SIGTERM` and `SIGINT` publish `offline`, generate a RESET on the bus, and release the GPIO lines.
- **Logging:** to stderr with syslog priority prefixes for journald; `hubus/master/log` mirrors the important events only.
- **Memory:** independent of bus activity; no unbounded queues and no heap allocation per frame.

### Statistics and Diagnosis

- `hubus/master/stats`: uptime, bursts, group requests (`polls`), requests by type, frames, duplicates, gaps, retries, skipped slaves, commands sent, executed, and rejected, errors by kind (no response, CRC, mismatch, line error, line fault), recoveries, startups, scans, queue depth and overflows, publish errors.
- `hubus/slave/<n>/stats`: see the table above.
- `hubus/master/diagnosis`: the master's duplicate MCID and line problem heuristic ([§6.4](spec/06-operation.md#64-commissioning-scan-and-duplicate-mcids)), also logged when it changes.

## Limits

- The daemon reports only what the protocol carries: `SLAVE_INFO`, states, and event data. Counters inside a slave appear only if its firmware writes them into its event buffer or state.
- The timing defaults are conservative placeholders until measured on hardware ([open issues](project/open-issues.md)).
- Only the libgpiod v2 API is supported.
- Firmware updates ([§6.8](spec/06-operation.md#68-firmware-update)) are not run by the daemon: it shows which slaves can be updated (`update` in `info`) and which wait in their updater (`updater`), and the raw `cmd` topic refuses the update opcodes. To update, stop the service and run `hubus_master_example update <mcid> <file.hex>` ([master library](master-library.md#firmware-update)).
- Not yet run on a Raspberry Pi with real slaves; tested against the bus simulation, with a fake MQTT client and end to end with a real broker ([development](development.md#daemon-tests)).
