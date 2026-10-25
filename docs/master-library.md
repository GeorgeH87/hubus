# Master Library

The HUBUS master in `master/` (logic, `hubus_master.h`) and `master/hubus_master_gpiod.h` (the Raspberry Pi hardware layer). It implements the master side of [§2](spec/02-line-conditions.md) to [§6](spec/06-operation.md) on top of a small hardware abstraction layer (HAL). [hubusd](daemon.md) is built on it; the command line example `examples/master/master_example.c` shows every call.

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
| `hubus_master_shutdown(m)` | Optional: RESET, so that all slaves are idle with `DATA` released. |

`hubus_master_cfg_t`: `t_clk_low_ns`, `t_clk_high_ns`, `t_sample_ns`, `t_cond_ns`, `t_turnaround_ns` (all minimums in nanoseconds), `line_fault_ticks`, `retry_budget`, `follow_more`, `max_fragments`, `dup_fail_threshold`, `line_problem_count`. `hubusd` exposes them as the [`timing` section](daemon.md#timing).

## Hardware Abstraction

`hubus_master_hal_t` has four functions and a context pointer; the logic runs unchanged on the Raspberry Pi and on the simulated bus of the tests.

| Function | Meaning |
|---|---|
| `set_clk(ctx, high)` | Drives `CLK` (push-pull). |
| `set_data_low(ctx, low)` | `1`: pull `DATA` low, `0`: release it. The master never drives `DATA` high ([§1.1](spec/01-physical-layer.md#11-bus-lines)). |
| `read_data(ctx)` | `1`: `DATA` is high. |
| `delay_ns(ctx, ns)` | Wait at least `ns` nanoseconds; waiting longer only slows the bus down. |

## Raspberry Pi Hardware Layer

`hubus_master_gpiod.h` implements the HAL on the Linux GPIO character device with the libgpiod v2 API ([§1.5](spec/01-physical-layer.md#15-nodes)). `DATA` is requested as open-drain output and needs an external pull-up.

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
| `hubus_ihex_parse(text, len, out, size, &img_len)` | Reads an Intel HEX file (`avr-objcopy -O ihex`) into `out`, filled with `FF` where the file has no data; `img_len` is one past the highest address with data. Returns 0, or -1 for a malformed file or a bad checksum. |
| `hubus_update_reason_str(reason)` | Text of a failure reason. |

`hubus_update_image_t` holds `data` and `len` (the image from the start of the application area; for the ATtiny85 the HEX file of the application as built) and `signature` (three bytes such as `1E 93 0B`; all zero skips the check). `progress(ctx, done, total)` is called after every page and may be `NULL`.

`hubus_update_result_t` tells what happened: `reason` (below), `page` (the page that failed), `last_result` (the updater's last write result), `pages` (pages written, including the invalidated and the final info page), and `retries` (repeated pages). A page is tried `retry_budget + 1` times, at least three.

| `reason` | Meaning |
|---|---|
| `HUBUS_UPD_REASON_NOT_SUPPORTED` | The slave announces neither capability bit 3 nor 4. |
| `HUBUS_UPD_REASON_NO_UPDATER` | No answer from the updater after `UPDATE_ENTER`: no updater installed, or no slave at this MCID. |
| `HUBUS_UPD_REASON_BAD_INFO` | The updater information or state is not usable. |
| `HUBUS_UPD_REASON_SIGNATURE` | The device signature differs from `img.signature`. |
| `HUBUS_UPD_REASON_TOO_LARGE` | The image is empty or does not fit in front of the info page. |
| `HUBUS_UPD_REASON_BAD_IMAGE` | With the reset vector rule: the image does not start with `RJMP`. |
| `HUBUS_UPD_REASON_WRITE` | A page failed every attempt. The slave stays in its updater; the update can be repeated. |
| `HUBUS_UPD_REASON_NOT_STARTED` | After `UPDATE_LEAVE` the updater answered again: the application did not check out. |

These failures return `HUBUS_ERR_UPDATE`; bus errors such as `HUBUS_ERR_LINE_FAULT` are returned as they are. The command line example has `hubus_master_example update <mcid> <file.hex>`.

## Protocol Helpers

The hardware-independent code in `core/` is shared by the master, the slave, and the tests. Applications rarely call it directly.

| Header | Content |
|---|---|
| `hubus_proto.h` | Protocol constants: message types, opcodes, `SLAVE_INFO` layout, capability bits, limits. |
| `hubus_frame.h` | `hubus_frame_encode()` / `hubus_frame_decode()` (header, payload, CRC8), `hubus_header_pack()` / `hubus_header_unpack()`, `hubus_mcid_match()` and `hubus_prefix_mask()` for prefix addressing ([§4.5](spec/04-frames.md#45-addressing)). |
| `hubus_crc.h` | CRC8: `hubus_crc8()` for a buffer, `hubus_crc8_update()` / `hubus_crc8_final()` byte by byte ([§4.12](spec/04-frames.md#412-integrity-check-crc8)). CRC-16 of the firmware update: `hubus_crc16()`, `hubus_crc16_update()` ([§6.8](spec/06-operation.md#68-firmware-update)). |
| `hubus_gen.h` | `GEN` evaluation per MCID: `hubus_gen_reset()`, `hubus_gen_eval()`, `hubus_gen_lost()`. |
| `hubus_arb.h` | Arbitration: `hubus_arb_decode()` derives the winner and the pending masks from the sampled slots; `hubus_mask_stack_*()` is the pending mask stack ([§5.5](spec/05-arbitration.md)). |
