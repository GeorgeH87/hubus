# Master Library

The HUBUS master in `master/` (logic, `hubus_master.h`) and `master/hubus_master_gpiod.h` (the Raspberry Pi hardware layer). It implements the master side of [§2](spec/02-line-conditions.md) to [§6](spec/06-operation.md) on top of a small hardware abstraction layer (HAL). [hubusd](daemon.md) is built on it; the command line tool `hubus-cli` (`master/hubus_cli.c`) uses every call.

The master owns every timeout, retry, and recovery decision ([§7.6](spec/07-slave.md#76-no-timeouts)). All functions are blocking and not thread-safe: use one master from one thread.

## Usage

```c
#include "hubus_master_gpiod.h"

static void on_data(void *ctx, const hubus_slave_frame_t *f, hubus_gen_result_t gen, uint8_t lost)
{
    /* f->mcid, f->payload[0 .. f->len - 1], f->more */
}

int main(void)
{
    hubus_gpiod_cfg_t gcfg = {.chip_label = "pinctrl-rp1", .clk_line = 17, .data_line = 27};
    hubus_gpiod_t *gpio = hubus_gpiod_open(&gcfg);
    hubus_master_hal_t hal = hubus_gpiod_hal(gpio);
    hubus_master_cfg_t cfg;
    hubus_master_t master;
    hubus_burst_stats_t stats;

    hubus_master_cfg_default(&cfg);
    hubus_master_init(&master, &hal, &cfg);
    hubus_master_startup(&master);
    hubus_master_burst(&master, on_data, NULL, &stats);   /* collect what was pending before the reset */
    for (;;) {
        hubus_master_service(&master, on_data, NULL, &stats);   /* idle check, and a burst if a slave announced data */
        /* sleep about 1 ms */
    }
}
```

Every function returns a `hubus_status_t`; `hubus_status_str()` turns it into text.

## Status Codes

| Code | Meaning |
|---|---|
| `HUBUS_OK` | Success. |
| `HUBUS_ERR_ARG` | Invalid argument (for example `target_len` out of range). |
| `HUBUS_ERR_NO_RESPONSE` | Silence after the turnaround slot ([§3.4](spec/03-transactions.md#34-response-detection)). |
| `HUBUS_ERR_CRC` | Response with an invalid CRC8. |
| `HUBUS_ERR_MISMATCH` | Valid frame, but an unexpected type or source MCID. |
| `HUBUS_ERR_LINE` | The readback of `DATA` differs from what the master transmitted; the recovery sequence was run. |
| `HUBUS_ERR_LINE_FAULT` | `DATA` stays low for `line_fault_ticks` ticks: a hardware fault ([§6.6](spec/06-operation.md#66-startup-shutdown-and-recovery)). |
| `HUBUS_ERR_STACK` | The pending mask stack overflowed (corrupted arbitration); the master recovers. |
| `HUBUS_ERR_UPDATE` | A firmware update was refused or failed; `hubus_update_result_t` tells why ([firmware update](#firmware-update)). |

## Setup

| Function | Purpose |
|---|---|
| `hubus_master_cfg_default(cfg)` | Fills the configuration with conservative defaults ([timing](daemon.md#timing) lists the fields and values). |
| `hubus_master_init(m, hal, cfg)` | Initialises the master and drives `CLK` low and `DATA` released. A `t_sample_ns` above `t_clk_high_ns` is limited to it, and a `t_turnaround_ns` below `t_clk_low_ns` is raised to it. |
| `hubus_master_startup(m)` | Startup sequence: clock until `DATA` is high, then RESET ([§6.6](spec/06-operation.md#66-startup-shutdown-and-recovery)). Also the recovery after a fault. Does not collect data: call `hubus_master_burst()` afterwards. |
| `hubus_master_reset(m)` | RESET ([§6.5](spec/06-operation.md#65-communication-stack-reset)): every slave goes idle with `DATA` released and keeps its buffer. Also ends a blind time ([§6.9](spec/06-operation.md#69-blind-time-after-a-command)). |
| `hubus_master_shutdown(m)` | Optional: RESET, so that all slaves are idle with `DATA` released. |

`hubus_master_cfg_t`: `t_clk_low_ns`, `t_clk_high_ns`, `t_sample_ns`, `t_cond_ns`, `t_turnaround_ns` (all minimums in nanoseconds), `t_class_margin_ns`, `line_fault_ticks`, `retry_budget`, `follow_more`, `max_fragments`, `dup_fail_threshold`, `line_problem_count`. `hubusd` exposes them as the [`timing` section](daemon.md#timing).

### Speed Classes

The master notes the speed class of every valid `SLAVE_INFO` it receives, in `REQUEST_INFO`, a group `REQUEST_INFO`, and the commissioning scan, also from slaves without a module and of an unknown protocol version ([§1.5](spec/01-physical-layer.md#15-speed-classes), [§4.7](spec/04-frames.md#47-slave-info-slave_info)). `m->cfg` holds the timing in use: the configuration given to `hubus_master_init()` (kept in `m->base`), with `t_clk_low_ns`, `t_clk_high_ns`, `t_cond_ns`, and `t_turnaround_ns` raised to the response time `R` of the slowest known class plus `t_class_margin_ns` (default 10 µs). The timing never goes below the configuration; class `0` and slaves the master has not read stand for it. A class stays known until `hubus_master_init()`, also when the slave goes silent.

| Function | Purpose |
|---|---|
| `hubus_speed_class_ns(k)` | `R` of class `k` in nanoseconds; 0 for class 0 and values above 15. |
| `hubus_master_speed_limit(m)` | The MCID whose class raised the timing (the lowest one among equally slow slaves), 0 if the configuration applies. `m->speed_class[mcid]` holds the class. |
| `hubus_master_note_speed_class(m, mcid, k)` | Notes a class and recomputes the timing; the master calls it itself. |

`hubus-cli` warns after a `scan` when a slave slows down the bus.

## Hardware Abstraction

`hubus_master_hal_t` has four functions and a context pointer; the logic runs unchanged on the Raspberry Pi and on the simulated bus of the tests.

| Function | Meaning |
|---|---|
| `set_clk(ctx, high)` | Drives `CLK` (push-pull). |
| `set_data_low(ctx, low)` | `1`: pull `DATA` low, `0`: release it. The master never drives `DATA` high ([§1.1](spec/01-physical-layer.md#11-bus-lines)). |
| `read_data(ctx)` | `1`: `DATA` is high. |
| `delay_ns(ctx, ns)` | Wait at least `ns` nanoseconds; waiting longer only slows the bus down. |

## Raspberry Pi Hardware Layer

`hubus_master_gpiod.h` implements the HAL on the Linux GPIO character device with the libgpiod v2 API ([§1.6](spec/01-physical-layer.md#16-nodes)). `DATA` is requested as open-drain output and needs an external pull-up.

| Function | Purpose |
|---|---|
| `hubus_gpiod_open(cfg)` | Opens the chip and requests both lines. Returns `NULL` and sets `errno` on failure. `hubus_gpiod_cfg_t`: `chip_label` (for example `pinctrl-rp1` on the Pi 5, `pinctrl-bcm2711` on the Pi 4; `NULL` takes the first chip with a known label), `chip_path` (alternative, for example `/dev/gpiochip0`), `clk_line`, `data_line`, `internal_pullup` (bench tests only). |
| `hubus_gpiod_hal(h)` | The HAL for `hubus_master_init()`. The handle must outlive the master. |
| `hubus_gpiod_close(h)` | Releases both lines (`CLK` low, `DATA` released) and frees the handle. |
| `hubus_gpiod_set_realtime(priority)` | `SCHED_FIFO` for the calling thread, `1..99`. Needs privileges (`CAP_SYS_NICE`). Returns 0 on success. |

## Idle Bus and Bursts

| Function | Purpose |
|---|---|
| `hubus_master_check_event(m, &event)` | Idle event handshake ([§6.1](spec/06-operation.md#61-idle-event-handshake)): if a slave holds `DATA` low on the idle bus, clocks until it is released and sets `*event = 1`. Call it periodically. |
| `hubus_master_burst(m, on_data, ctx, &stats)` | Collects the data of every slave that is pending, in descending MCID order, with one group `REQUEST_DATA` per slave ([§6.2](spec/06-operation.md#62-burst)). Retries failed requests within `retry_budget`, follows fragments if `follow_more` is set (at most `max_fragments` per slave), and evaluates `GEN` ([§4.9](spec/04-frames.md#49-generation-counter-gen)). |
| `hubus_master_service(m, on_data, ctx, &stats)` | One iteration of a service loop: `check_event`, then a burst if there was an event. |

`on_data(ctx, frame, gen, lost)` is called for every `SLAVE_DATA` frame that is not a duplicate. `gen` is the `GEN` evaluation (`HUBUS_GEN_BASELINE`, `NORMAL`, `DUPLICATE`, `WRAPPED`, `GAP`); `lost` is the number of overwritten buffer contents for `HUBUS_GEN_GAP`, otherwise 0. `frame->more` says that a further fragment follows. `hubus_burst_stats_t` counts `polls`, `served`, `duplicates`, `gaps`, `retries`, and `skipped` (slaves given up on after the retry budget).

`hubus_slave_frame_t` holds `type` (`HUBUS_S_DATA` or `HUBUS_S_INFO`), `mcid`, `more`, `gen`, `len`, and `payload`.

## Transactions

| Function | Purpose |
|---|---|
| `hubus_master_send_command(m, target_len, prefix, opcode, args, args_len)` | `SEND_COMMAND` to one slave (`target_len` 6, `prefix` = MCID) or a group (`0..5`); `0` is a broadcast. No response ([§4.4](spec/04-frames.md#44-command-opcodes)). |
| `hubus_master_status_capture(m, target_len, prefix)` | `STATUS_CAPTURE` (opcode `0x00`) to one slave or a group. The slaves answer later through the event buffer. |
| `hubus_master_request_info(m, mcid, &out)` | `REQUEST_INFO` to exactly one slave ([§4.7](spec/04-frames.md#47-slave-info-slave_info)). |
| `hubus_master_request_data(m, mcid, &out)` | `REQUEST_DATA` to exactly one slave, without arbitration. |
| `hubus_master_request_state(m, mcid, &out)` | `REQUEST_STATE`: the live state as a `SLAVE_DATA` frame ([§4.10](spec/04-frames.md#410-state)). Clears nothing. |
| `hubus_master_request_group(m, req_type, prefix, target_len, &res, &out)` | Group request (`HUBUS_M_REQUEST_INFO`, `_DATA`, or `_STATE`, `target_len` `0..5`): the matching slaves arbitrate and the winner answers in the same transaction. `res` is always filled; `res->contender` is 0 if nobody answered (the return value is then `HUBUS_OK`). On `HUBUS_ERR_CRC` or `HUBUS_ERR_MISMATCH` the winner and the masks in `res` are still valid. |
| `hubus_master_read_states(m, on_state, ctx, &count)` | Reads the state of every slave that has one ([§6.3](spec/06-operation.md#63-reading-states)), in descending MCID order, and calls `on_state(ctx, frame)` for each. `count` (may be `NULL`) gets the number of frames. A winner with an invalid frame is asked again within the retry budget, then skipped. |

## Commissioning and Diagnosis

| Function | Purpose |
|---|---|
| `hubus_master_scan(m, &scan)` | Commissioning scan with group `REQUEST_INFO` ([§6.4](spec/06-operation.md#64-commissioning-scan-and-duplicate-mcids)). `scan->state[mcid]` is `HUBUS_SCAN_ABSENT`, `HUBUS_SCAN_PRESENT` (with `scan->info[mcid]`), or `HUBUS_SCAN_SUSPECT` (repeated CRC failures: a duplicate MCID or a line problem). The structure is large; keep it static or on the heap. |
| `hubus_master_diagnose(m, &suspects)` | Heuristic over the failure counters: `HUBUS_DIAG_OK`, `HUBUS_DIAG_DUPLICATE` (up to `line_problem_count` MCIDs fail repeatedly), or `HUBUS_DIAG_LINE_PROBLEM` (more). `suspects` gets one bit per MCID. |

`m->counters` (`hubus_master_counters_t`) counts polls, requests, commands, `no_response`, `crc_errors`, `mismatches`, `line_errors`, `line_faults`, and `recoveries`; `m->mcid_counters[mcid]` (`hubus_mcid_counters_t`) counts `frames`, `crc_errors`, `no_response`, `duplicates`, `gaps`, and `lost` per slave. hubusd publishes them as [statistics](daemon.md#statistics-and-diagnosis).

## Firmware Update

`master/hubus_update.h` updates the firmware of a slave that carries the updater ([§6.8](spec/06-operation.md#68-firmware-update), [slave library](slave-library.md#firmware-update)).

| Function | Purpose |
|---|---|
| `hubus_master_update(m, mcid, &img, progress, ctx, &res)` | Switches the slave to its updater (`UPDATE_ENTER`, unless the updater already runs), checks the image against the updater information, invalidates the old application, writes every page with a readback of the result, writes the info page, and starts the new application. Blocks for the whole update, about 10 s for 6 KB with the default timing; nothing else runs on the bus meanwhile. Returns `HUBUS_OK` once the new application answers. |
| `hubus_master_set_mcid(m, mcid, new_mcid, modules, n_modules, &res)` | Gives the slave another MCID through its updater (`UPDATE_SET_MCID`, [§6.8](spec/06-operation.md#mcid)): refuses a new MCID that already answers, switches the slave to its updater, sends the new MCID until the updater answers to it, and starts the application again, which then answers under the new MCID if it takes the stored one. A slave that waited in its updater stays there, under the new MCID. `modules` may be `NULL`, as for the update. Takes about a second with the default timing. |
| `hubus_ihex_parse(text, len, out, size, &first, &end)` | Reads an Intel HEX file (`avr-objcopy -O ihex`) into `out` at the addresses of the file, filled with `FF` where the file has no data; `first` (may be `NULL`) is the lowest address with data, `end` one past the highest. Returns 0, or -1 for a malformed file or a bad checksum. The image of an application is then `out + first`, `end - first` bytes, linked for `first`. |
| `hubus_update_reason_str(reason)` | Text of a failure reason. |

`hubus_update_image_t` holds:

- `data` and `len`: the image from the start of the application area, which is 0 on the ATtiny85 and the end of the boot section on the tinyAVR 0/1/2-series ([slave library](slave-library.md#tinyavr-012-series)).
- `signature`: three bytes such as `1E 93 0B`; all zero skips the check.
- optionally `start` with `check_start`: the address the image is linked for, from `hubus_ihex_parse()`. An image for another application area is refused before anything is written: its code would run at the wrong addresses. A module image has no such address; for it the signature is the check.
- optionally `modules` and `n_modules`: the modules of the master. `progress(ctx, done, total)` is called after every page and may be `NULL`.

`hubus_update_result_t` tells what happened: `reason` (below), `page` (the page that failed), `last_result` (the updater's last write result), `pages` (pages written, including the invalidated and the final info page), and `retries` (repeated pages). A page is tried `retry_budget + 1` times, at least three.

| `reason` | Meaning |
|---|---|
| `HUBUS_UPD_REASON_NO_UPDATER` | No answer from the updater after `UPDATE_ENTER`: no updater installed, an application without update support whose module the master does not know, or no slave at this MCID. |
| `HUBUS_UPD_REASON_NO_SUPPORT` | With `img.modules`: the module of the slave states no update support, so nothing was sent. |
| `HUBUS_UPD_REASON_BAD_INFO` | The updater information or state is not usable. |
| `HUBUS_UPD_REASON_SIGNATURE` | The device signature differs from `img.signature`. |
| `HUBUS_UPD_REASON_TOO_LARGE` | The image is empty or does not fit in front of the info page. |
| `HUBUS_UPD_REASON_BAD_IMAGE` | With the reset vector rule: the image does not start with `RJMP`. |
| `HUBUS_UPD_REASON_WRONG_START` | With `img.check_start`: the image is linked for another application area than the one the updater announces, for example an ATtiny85 image for a device with a boot section. Nothing was written; the slave waits in its updater. |
| `HUBUS_UPD_REASON_WRITE` | A page failed every attempt. The slave stays in its updater; the update can be repeated. |
| `HUBUS_UPD_REASON_NOT_STARTED` | After `UPDATE_LEAVE` the updater answered again: the application did not check out. |
| `HUBUS_UPD_REASON_MCID_TAKEN` | `set_mcid`: another slave answers to the new MCID; nothing was sent. |
| `HUBUS_UPD_REASON_NO_SET_MCID` | `set_mcid`: the updater has no flag bit 1, it cannot change the MCID. The application was started again under its MCID. |
| `HUBUS_UPD_REASON_MCID_NOT_SET` | `set_mcid`: the updater did not answer to the new MCID within the attempts. |
| `HUBUS_UPD_REASON_MCID_NOT_KEPT` | `set_mcid`: the application answered under the old MCID, it takes its MCID from elsewhere (pins, a constant). The master undid the change with `UPDATE_ENTER` and `UPDATE_LEAVE`, so that the updater keeps the MCID of the application. |

These failures return `HUBUS_ERR_UPDATE`; bus errors such as `HUBUS_ERR_LINE_FAULT` are returned as they are; `set_mcid` returns `HUBUS_ERR_ARG` for an MCID outside `1..63` or a new MCID equal to the old. The command line tool has `hubus-cli update <mcid> <file.hex>` (which checks the start of the file against the application area) and `hubus-cli mcid <mcid> <new>`; with `--modules <dir>` it loads the plugins of that directory, prints the module of a slave for `scan` and `info`, and hands the modules to the update and the change of the MCID.

## Master Modules

`master/hubus_module.h` is the typed interface of the modules that come with a slave firmware image ([§6.10](spec/06-operation.md#610-master-modules)), and their selection by the hardware identifier ([§4.7](spec/04-frames.md#hardware-identifier-hwid)). hubusd selects the module of every slave with it and calls its functions ([hubusd](daemon.md#module-functions)).

A module is a constant structure: what the bus thread of a master needs is data (rules, properties, blind times), everything else is up to four functions. Every slave that goes into operation gets an **instance** of its module, and the instance decides which topics below the slave it takes and what it publishes there; the master only passes messages, frames, and commands between the instance, the broker, and the bus.

Most devices are described by a table of entities of Home Assistant, and `master/hubus_module_ha.h` turns such a table into topics, values, and discovery messages. Such a module needs no code of its own:

```c
#include "hubus_module_ha.h"

static const hubus_module_rule_t rules[] = {
    {.pattern = {0x83}, .mask = {0xFF}, .len = 1},
};

static const hubus_entity_t entities[] = {
    {
        .key = "temperature",
        .name = "Temperature",
        .component = HUBUS_COMPONENT_SENSOR,
        .device_class = HUBUS_DEVICE_CLASS_TEMPERATURE,
        .unit = HUBUS_UNIT_CELSIUS,
        .decode = {.offset = 0, .len = 2, .big_endian = true, .scale = 1.0, .add = -273.0},
    },
};

static const hubus_ha_t table = {entities, HUBUS_MODULE_LEN(entities), NULL};

static const hubus_module_t module = {
    HUBUS_MODULE_INIT,
    .name = "temperature",
    .rules = rules,
    .n_rules = HUBUS_MODULE_LEN(rules),
    .max_command = 1,
    .has_state = true,
    .has_events = true,
    .data = &table,
    HUBUS_HA_FUNCTIONS,
};

const hubus_module_t *hubus_module_entry(void) { return &module; }
```

Built as a plugin together with the helper library, which a plugin links in: `gcc -shared -fPIC -I master -I core -o temperature.so temperature.c master/hubus_module_ha.c master/hubus_module_topic.c master/hubus_json.c` (in the CMake build: `target_link_libraries(... hubus_module_ha)`, as in `examples/plugin-dimmer`).

### Functions and Host

Every module has all four functions; `hubus_module_validate()` refuses one without them:

| Function | Purpose |
|---|---|
| `create(host)` | The instance for one slave, `hubus_module_instance_t`, or `NULL` if it cannot. It usually subscribes to its topics and publishes its discovery messages. |
| `destroy(self)` | Ends the instance; its topics go with it. |
| `on_frame(self, host, kind, data, len, gap)` | A frame of the slave, an event or a state that was read. `gap`: frames were lost before it. |
| `on_message(self, host, topic, payload, len)` | A message on a topic the instance subscribed to: the topic below the slave, and `len` bytes, followed by a NUL that is not counted. |

A master calls them on a thread of the module, never on its bus thread, and one after the other for one module. A function has to return in a short time: in hubusd a call that takes longer than `modules.call_timeout_ms` stops the module ([hubusd](daemon.md#module-functions)). A function reaches the master only through `hubus_module_host_t`, which it may also keep until `destroy` has returned:

| Function | Purpose |
|---|---|
| `host->send(host, opcode, args, len)` | Queues a command to this slave. Opcode `0` or `0x10..0xFF`, at most 31 bytes with the opcode. |
| `host->subscribe(host, topic)`, `host->unsubscribe(host, topic)` | Takes a topic below the slave, or gives it up; `on_message` gets what arrives there. `+` and `#` as in MQTT, but not in the first level; at most 16 topics per instance. |
| `host->publish(host, topic, payload, len, retain)` | Publishes up to 1023 bytes on a topic below the slave. A retained message is published again when the master reconnects, and removed when the slave leaves the configuration or gets another module; an empty one removes the topic. hubusd keeps up to 40 retained topics per slave, discovery messages included; it publishes more, but logs that it cannot publish them again. |
| `host->discovery(host, component, object, json)` | A discovery message of Home Assistant, retained, on `<prefix>/<component>/hubus_<n>_<object>/config`. The master adds `unique_id`, the availability, and the device of the slave, and publishes it only while discovery is on. |
| `host->topic(host, topic, out, size)` | The full name of a topic of this slave, `<base>/slave/<n>/<topic>`, for a discovery message. |
| `host->log(host, text)`, `host->reject(host, text)` | A line for the log of the master; `reject` says why a message was not taken and counts it as a rejected command. |
| `host->setting(host, key, out, size)`, `host->save(host, key, value)` | Settings: short texts the master keeps for this slave and module across its restarts (hubusd: in its [runtime file](daemon.md#configuration-over-mqtt)), such as a value the user chose that the slave forgets at a power-up. `setting` copies the value to `out` and returns its length, 0 if there is none; it answers at once with what the instance saved last, or what the master had at `create`. `save` keeps a value, `NULL` removes the key; only a change is written. Keys like a topic level, at most 23 characters, not `module`; values of at most 95 printable characters; at most 8 per slave (`hubus_module_setting_valid()`). They go with the slave, and another module of the slave does not get them. |
| `host->readback` | What the master does after a command ([readback](daemon.md#state-after-a-command-readback)): `HUBUS_READBACK_CACHE` asks the module to publish the state its command establishes. |

A topic below the slave has levels of letters, digits, `_` and `-`, separated by `/`, at most 63 characters. Its first level is none of those the master has for itself: `availability`, `capture`, `cmd`, `config`, `data`, `firmware`, `info`, `mcid`, `read`, `stats`, `update` (`hubus_module_topic_valid()`). The functions of the host only queue, so they return at once, with `HUBUS_MODULE_REJECTED` for what is never allowed and `HUBUS_MODULE_FAILED` if the queue is full. What depends on the bus shows up in the log only; `setting` is the one that answers.

### Entities of Home Assistant

`hubus_module_ha.h` describes a slave by a table `hubus_ha_t` in `hubus_module_t.data`: entities with a key, a name, a kind (`sensor`, `binary_sensor`, `switch`, `light`, `text`, `number`), a unit and device class, where the value lies in a frame (`decode`), for a number its range, and for a switch the commands for `ON` and `OFF`. Keys are a level of a topic: letters, digits, `_` and `-`, at most 23, and none of the reserved first levels; a table has at most 8 entities. For each entity the library uses

- `<key>/state`, retained: the value, from every frame that holds it, or from the module with `hubus_ha_publish_state()`;
- `<key>/set`: a switch, a light, a text, or a number takes values here; `<key>/rgb/set` the colour of a light, `r,g,b`.

`HUBUS_HA_FUNCTIONS` are the four functions of a module that is all table. A switch with fixed commands needs nothing else; with `readback: cache` the library also publishes the state the command establishes. A switch without fixed commands, a light, a text, and a number need `on_set` in the table: it gets the value parsed and checked (a bool, RGB, text, or a number within the range) and returns `HUBUS_MODULE_REJECTED` for one it does not take. A module with more to do calls `hubus_ha_announce()`, `hubus_ha_on_frame()`, and `hubus_ha_on_message()` from its own functions, as `daemon/hubusd_ledstrip.c`, `daemon/hubusd_distance.c`, and `examples/plugin-dimmer` do.

| Function | Purpose |
|---|---|
| `hubus_ha_valid(module)` | Checks the table: keys, kinds, values, commands within the command size of the module, and `on_set` where an entity needs it. `hubus_ha_announce()` refuses a table that fails. |
| `hubus_ha_announce(host)` | Subscribes to the topics of the entities that take values and publishes their discovery messages. |
| `hubus_ha_publish_state(host, key, text)` | The value of an entity that no frame reports, such as the state of a light. |
| `hubus_ha_find_entity(table, key)` | Index of the entity with this key, or `-1`. |
| `hubus_entity_decode(entity, data, len, out, size)` | The value of an entity in a frame as text: a number with the decimals of the entity for a sensor, otherwise `ON` or `OFF`. Returns 0 if the frame is too short or the entity has no value in a frame. |

### Selection and Checks

| Function | Purpose |
|---|---|
| `hubus_hwid_form(hwid, len)` | `HUBUS_HWID_SHORT`, `HUBUS_HWID_FULL`, `HUBUS_HWID_UPDATER`, or `HUBUS_HWID_INVALID`, with the length checked against the form. |
| `hubus_module_validate(module)` | Checks a module before its first use: interface version, rules, properties, blind times, functions, firmware image. The compiler cannot check what comes from a plugin. |
| `hubus_module_select(modules, n, hwid, len, &out)` | `HUBUS_MODULE_FOUND` with the module of the rule with the most mask bits, `HUBUS_MODULE_UNKNOWN`, `HUBUS_MODULE_INVALID`, or `HUBUS_MODULE_CONFLICT`. |
| `hubus_module_select_info(modules, n, payload, len, &out)` | The same from a `SLAVE_INFO` payload; a protocol version this master does not know counts as invalid. |
| `hubus_module_conflicts(a, b)` | Whether rules of two modules have the same number of mask bits and can match the same HWID: `select` would find no winner. |
| `hubus_module_has_blind_time(module)` | Whether the slave can be blind after any command; it then keeps quiet until the master ends that phase. |
| `hubus_module_blind_ms(module, opcode)` | The blind time after a command: the value for the opcode, otherwise the general one ([§7.8](spec/07-slave.md#78-blind-sections)). |
| `hubus_module_topic_valid(topic, filter)`, `hubus_module_topic_matches(filter, topic)` | The rules for topics below a slave, and MQTT matching of `+` and `#`. |

`test/test_module.c`, `test/test_module_ha.c`, and `daemon/hubusd_ledstrip.c` have complete examples.

### Firmware Image in a Module

A module can bring the application it belongs to, so that hubusd can [update a slave](daemon.md#firmware-updates) on a trigger. The image is part of the plugin, so module and firmware cannot be mixed up, and the checks on a plugin file also cover the image:

```sh
avr-objcopy -O binary --gap-fill 0xFF app.elf app.bin
xxd -i app.bin > app_image.h
```

```c
#include "app_image.h"

static const hubus_module_t module = {
    /* ... */
    .has_update = true,
    .firmware = app_bin,
    .firmware_len = sizeof app_bin,
    .firmware_version = 3,                      /* the version byte in the HWID of this firmware */
    .firmware_signature = {0x1E, 0x93, 0x0B},   /* of the device; all zero: not checked */
};
```

The image starts at the start of the application area, like the one of `hubus_master_update()` ([firmware update](#firmware-update)). `hubus_module_validate()` refuses an image without a length, or one without `has_update`, with `HUBUS_MODULE_ERR_FIRMWARE`. A slave with the short form of the HWID reports no version, so hubusd cannot compare it and shows no installed version.

### Loading Plugins

`master/hubus_module_loader.h` keeps the modules of a master in a `hubus_module_set_t`: modules that are linked in (`hubus_module_set_add()`) and plugins, shared libraries that export `hubus_module_entry()` (`hubus_module_set_load_file()`, `hubus_module_set_load_dir()`). The set is what `hubus_module_select()` takes (`set.module`, `set.n`). Link with `-ldl`; the CMake target `hubus_master` does.

Every module is checked before it joins the set, and a plugin that fails is closed again. The result says which check failed:

| Status | Meaning |
|---|---|
| `HUBUS_LOAD_UNSAFE` | The file or directory is writable by its group or by others, or belongs to someone other than root and the current user. A plugin runs with the rights of the master, so it must not be changeable by others. |
| `HUBUS_LOAD_OPEN` | Not a shared library, a missing file, or an unreadable directory; `detail` has the message. |
| `HUBUS_LOAD_NO_ENTRY`, `HUBUS_LOAD_NO_MODULE` | No `hubus_module_entry`, or it returns `NULL`. |
| `HUBUS_LOAD_INVALID` | `hubus_module_validate()` found an error; `invalid` says which. |
| `HUBUS_LOAD_DUPLICATE` | A module of the set has this name; `module` points to it. |
| `HUBUS_LOAD_CONFLICT` | A module of the set has rules that conflict with its own ([above](#master-modules)); `module` points to it. |
| `HUBUS_LOAD_FULL` | The set holds `HUBUS_MODULE_SET_MAX` (64) modules. |

`hubus_module_set_load_dir()` reads the files ending in `.so` in the order of their names, so that the module that loses a conflict is always the same, and calls `report` for each file. `hubus_module_set_unload()` closes the plugins; do it after the last use of a module. `hubus_module_set_load_file()` goes by the file itself, not by its name: a file that is loaded gives the same module in a second set, and a file that was replaced gives a new one next to the old, so that a master can load a new set before it gives up the old one. `hubus_module_set_keep()` keeps a plugin in memory for good, for a module whose code a thread that cannot be stopped may still run. `set.trust_all` skips the permission check, for tests on file systems that have no permissions.

A plugin is built as [above](#master-modules), with the helper library if it uses a table of entities. `examples/plugin-dimmer/dimmer_plugin.c` is a complete one with functions of its own, a light for a dimmer; the build makes it, and `cmake --install` puts it into `lib/hubus/modules`, the directory in the example of [`modules.dir`](daemon.md#configuration). `test/plugins/a_good.c` is one that is all table.

A plugin is machine code, so it is built per architecture: for 32-bit Raspberry Pi OS (`armhf`), for 64-bit Raspberry Pi OS (`arm64`), each with the compiler of that system or a matching cross compiler such as `arm-linux-gnueabihf-gcc` or `aarch64-linux-gnu-gcc`. The kernel can be 64-bit under a 32-bit system, so what counts is the architecture of hubusd (`file $(which hubusd)`). A plugin of the wrong architecture is refused at load time with `HUBUS_LOAD_OPEN`, and `detail` says so. Build it against the headers of the same version as hubusd; `hubus_module_validate()` refuses another interface version.

## Protocol Helpers

The hardware-independent code in `core/` is shared by the master, the slave, and the tests. Applications rarely call it directly.

| Header | Content |
|---|---|
| `hubus_proto.h` | Protocol constants: message types, opcodes, `SLAVE_INFO` layout, hardware identifier, limits. |
| `hubus_frame.h` | `hubus_frame_encode()` / `hubus_frame_decode()` (header, payload, CRC8), `hubus_header_pack()` / `hubus_header_unpack()`, `hubus_mcid_match()` and `hubus_prefix_mask()` for prefix addressing ([§4.5](spec/04-frames.md#45-addressing)). |
| `hubus_crc.h` | CRC8: `hubus_crc8()` for a buffer, `hubus_crc8_update()` / `hubus_crc8_final()` byte by byte ([§4.12](spec/04-frames.md#412-integrity-check-crc8)). CRC-16 of the firmware update: `hubus_crc16()`, `hubus_crc16_update()` ([§6.8](spec/06-operation.md#68-firmware-update)). |
| `hubus_gen.h` | `GEN` evaluation per MCID: `hubus_gen_reset()`, `hubus_gen_eval()`, `hubus_gen_lost()`. |
| `hubus_arb.h` | Arbitration: `hubus_arb_decode()` derives the winner and the pending masks from the sampled slots; `hubus_mask_stack_*()` is the pending mask stack ([§5.5](spec/05-arbitration.md)). |
