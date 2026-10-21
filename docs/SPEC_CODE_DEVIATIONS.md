# Deviations Between Specification and Implementation

Functional differences found by reading the specs (`PROTOCOL.md`, `ARBITRATION.md`, `MESSAGE_FORMAT.md`, `SLAVE_PROPERTIES.md`, `DAEMON.md`) against the code (`core/hubus_slave_core.c`, `core/hubus_arb.c`, `master/hubus_master.c`, `daemon/hubusd_core.c`). Findings come from reading the sources.

Name differences are not listed here: the state names of the code were adopted in `SLAVE_PROPERTIES.md` section 2 and `PROTOCOL.md` section 4.

Resolution rules: slave logic stays simple and complexity lives in the master; rare cases are weighed instead of prevented.

Status values: `open`, `resolved`. All items below are resolved.

Not reviewed: `DAEMON_REQUIREMENTS.md`, `master/hubus_master_gpiod.c`, `daemon/hubusd_run.c`, the HAL files, and the tests.

| ID | Topic | Resolution |
|---|---|---|
| D1 | Startup order | Code changed: burst right after RESET |
| D2 | Read-back cost | Docs corrected (three transactions) |
| D3 | Repeated commands | Spec extended (`PROTOCOL.md` 7.3), accepted rare case |
| D4 | Periodic scan | Spec extended (`PROTOCOL.md` 3.5) |
| D5 | Offline after one missed probe | Code changed: startup probes `ping_failures` times |
| D6 | Unconfigured slave data | Docs corrected; behavior needed, no code change |
| D7 | Turnaround checks | Spec wording corrected (`PROTOCOL.md` 1.3) |

## D1: Startup Order Differs From the Spec

- **Status:** resolved (code)
- **Spec:** `PROTOCOL.md` section 3.3 lists clock until `DATA` is high, RESET, and then a full burst to collect data that was pending before the reset.
- **Code:** `bus_start()` in `daemon/hubusd_core.c` runs `hubus_master_startup()` (clock, RESET), then `REQUEST_INFO` to every configured slave, then the scan of MCID 1..63, and only then the burst, followed by `refresh_states()`.
- **Effect:** pending data is collected later than the spec says. The probes do not clear any buffer, so no data is lost; startup is slower until the first burst. If a slave signals during the probes, the master handles it through the idle handshake of the next transaction.
- **Resolution:** `bus_start()` runs the burst right after the RESET, before the probes and the scan. The early burst cannot decode data (no `SLAVE_INFO`, no profile yet), so it is published on the `data` topic only; the probe marks every online slave for a state refresh, which publishes the state. Test: `test_data_pending_before_startup_is_collected_before_the_probes`.

## D2: Read-back Costs Three Transactions, Not Two

- **Status:** resolved (docs)
- **Spec:** `DAEMON.md` section 3 says `readback: slave` "costs two more bus transactions per command".
- **Code:** `send_exact()` sends `STATUS_CAPTURE`, then `run_burst()` runs `POLL_PENDING` followed by `REQUEST_DATA` for the winner. That is three transactions for one pending slave, more if other slaves are pending and get collected in the same burst.
- **Effect:** latency of a `set` command is higher than documented; other slaves' pending data is collected as a side effect.
- **Resolution:** `DAEMON.md` states three transactions and the side effect.

## D3: Retries on Line Errors Are General, Not Only for the Start Race

- **Status:** resolved (spec)
- **Spec:** `PROTOCOL.md` section 7.1 describes the repeat of a transaction for the case that an event signal races with START.
- **Code:** `hubus_master_send_command()` and `hubus_master_poll()` repeat the transaction on any `HUBUS_ERR_LINE` (readback mismatch while sending) up to `retry_budget` times, after `recover()` (clock, RESET). `SEND_COMMAND` is repeated on the assumption that a corrupted frame is rejected by every slave's CRC.
- **Effect:** a command whose corruption is invisible to the CRC (undetectable error patterns, `MESSAGE_FORMAT.md` section 7) would be executed twice in the worst case. This is covered by the accepted CRC limits, but the spec does not state that commands may be repeated.
- **Resolution:** documented as accepted rare case `PROTOCOL.md` 7.3 (idempotent commands recommended).

## D4: Periodic Commissioning Scan

- **Status:** resolved (spec)
- **Spec:** `PROTOCOL.md` section 3.5 describes the scan as a commissioning step.
- **Code:** the daemon repeats the scan every `bus.scan_interval_s` (`hubusd_step()`), marks present slaves online, and follows up with `refresh_states()`.
- **Effect:** about 63 `REQUEST_INFO` transactions (plus retries for MCIDs with CRC failures) per interval, which delays commands and events by that time. Slaves that appear later are found; unconfigured ones are reported in `hubus/master/state`.
- **Resolution:** `PROTOCOL.md` 3.5 allows a periodic scan and names its cost.

## D5: Startup Marks a Slave Offline After One Missed Probe

- **Status:** resolved (code)
- **Spec:** `DAEMON.md` section 6: a slave goes `offline` after `bus.ping_failures` misses.
- **Code:** `bus_start()` calls `ping(d, mcid, 1)`, so a single missed `REQUEST_INFO` at startup publishes the slave as `offline`, regardless of `bus.ping_failures`. During normal operation the configured threshold applies.
- **Effect:** a slave that misses its first probe (for example a CRC error from noise) is reported offline until the next ping or scan finds it.
- **Resolution:** startup probes a slave up to `bus.ping_failures` times; it is `offline` only after all of them missed. Test: `test_startup_probes_a_missing_slave_ping_failures_times`.

## D6: Data From Unconfigured Slaves Is Consumed and Dropped

- **Status:** resolved (docs)
- **Spec:** `DAEMON.md` states that slaves not in the configuration are "ignored".
- **Code:** a burst reads every pending slave, including unconfigured ones. The valid response ends with STOP, which clears the slave's buffer. `on_data()` then only logs "data from unconfigured slave ignored" once and discards the payload.
- **Effect:** the slave's data is lost, not left in its buffer. It is "ignored" by the daemon but not left unread on the bus.
- **Resolution:** reading is necessary: without the STOP after a complete response the slave would announce itself again after every burst. No code change; `DAEMON.md` says the data is read and discarded.

## D7: Turnaround Checks Happen at the Last Frame Bit

- **Status:** resolved (spec)
- **Spec:** `PROTOCOL.md` section 1.3: at the turnaround slot only three checks remain (CRC result, address match, buffer state).
- **Code:** `rx_done()` makes all three decisions when the last CRC bit is received. The `TURN` state only counts the clock edges of the turnaround slot and then enters `ARB` or `TX`.
- **Effect:** none on the wire. The slave reaches `IGNORE` directly (without `TURN`) after a CRC error, an empty buffer, or a `SEND_COMMAND`. The spec wording can say the checks are made as soon as the frame ends.
- **Resolution:** `PROTOCOL.md` 1.3 now describes the check at the last frame bit.
