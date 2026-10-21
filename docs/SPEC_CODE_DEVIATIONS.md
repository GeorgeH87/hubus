# Deviations Between Specification and Implementation

Functional differences found by reading the specs (`PROTOCOL.md`, `ARBITRATION.md`, `MESSAGE_FORMAT.md`, `SLAVE_PROPERTIES.md`, `DAEMON.md`) against the code (`core/hubus_slave_core.c`, `core/hubus_arb.c`, `master/hubus_master.c`, `daemon/hubusd_core.c`). Findings come from reading the sources; none of them was reproduced with a test.

Name differences are not listed here: the state names of the code were adopted in `SLAVE_PROPERTIES.md` section 2 and `PROTOCOL.md` section 4.

Status values: `open` (decide: change code or change spec), `documented` (behavior is intended, the spec should say so).

Not reviewed: `DAEMON_REQUIREMENTS.md`, `master/hubus_master_gpiod.c`, `daemon/hubusd_run.c`, the HAL files, and the tests.

## D1: Startup Order Differs From the Spec

- **Status:** open
- **Spec:** `PROTOCOL.md` section 3.3 lists clock until `DATA` is high, RESET, and then a full burst to collect data that was pending before the reset.
- **Code:** `bus_start()` in `daemon/hubusd_core.c` runs `hubus_master_startup()` (clock, RESET), then `REQUEST_INFO` to every configured slave, then the scan of MCID 1..63, and only then the burst, followed by `refresh_states()`.
- **Effect:** pending data is collected later than the spec says. The probes do not clear any buffer, so no data is lost; startup is slower until the first burst. If a slave signals during the probes, the master handles it through the idle handshake of the next transaction.
- **Options:** run the burst right after the RESET in the daemon, or relax the spec ("probe and scan may precede the burst").

## D2: Read-back Costs Three Transactions, Not Two

- **Status:** documented (fix the wording)
- **Spec:** `DAEMON.md` section 3 says `readback: slave` "costs two more bus transactions per command".
- **Code:** `send_exact()` sends `STATUS_CAPTURE`, then `run_burst()` runs `POLL_PENDING` followed by `REQUEST_DATA` for the winner. That is three transactions for one pending slave, more if other slaves are pending and get collected in the same burst.
- **Effect:** latency of a `set` command is higher than documented; other slaves' pending data is collected as a side effect.

## D3: Retries on Line Errors Are General, Not Only for the Start Race

- **Status:** documented
- **Spec:** `PROTOCOL.md` section 7.1 describes the repeat of a transaction for the case that an event signal races with START.
- **Code:** `hubus_master_send_command()` and `hubus_master_poll()` repeat the transaction on any `HUBUS_ERR_LINE` (readback mismatch while sending) up to `retry_budget` times, after `recover()` (clock, RESET). `SEND_COMMAND` is repeated on the assumption that a corrupted frame is rejected by every slave's CRC.
- **Effect:** a command whose corruption is invisible to the CRC (undetectable error patterns, `MESSAGE_FORMAT.md` section 7) would be executed twice in the worst case. This is covered by the accepted CRC limits, but the spec does not state that commands may be repeated.

## D4: Periodic Commissioning Scan

- **Status:** documented
- **Spec:** `PROTOCOL.md` section 3.5 describes the scan as a commissioning step.
- **Code:** the daemon repeats the scan every `bus.scan_interval_s` (`hubusd_step()`), marks present slaves online, and follows up with `refresh_states()`.
- **Effect:** about 63 `REQUEST_INFO` transactions (plus retries for MCIDs with CRC failures) per interval, which delays commands and events by that time. Slaves that appear later are found; unconfigured ones are reported in `hubus/master/state`.

## D5: Startup Marks a Slave Offline After One Missed Probe

- **Status:** open
- **Spec:** `DAEMON.md` section 6: a slave goes `offline` after `bus.ping_failures` misses.
- **Code:** `bus_start()` calls `ping(d, mcid, 1)`, so a single missed `REQUEST_INFO` at startup publishes the slave as `offline`, regardless of `bus.ping_failures`. During normal operation the configured threshold applies.
- **Effect:** a slave that misses its first probe (for example a CRC error from noise) is reported offline until the next ping or scan finds it.

## D6: Data From Unconfigured Slaves Is Consumed and Dropped

- **Status:** open
- **Spec:** `DAEMON.md` states that slaves not in the configuration are "ignored".
- **Code:** a burst reads every pending slave, including unconfigured ones. The valid response ends with STOP, which clears the slave's buffer. `on_data()` then only logs "data from unconfigured slave ignored" once and discards the payload.
- **Effect:** the slave's data is lost, not left in its buffer. It is "ignored" by the daemon but not left unread on the bus.

## D7: Turnaround Checks Happen at the Last Frame Bit

- **Status:** documented
- **Spec:** `PROTOCOL.md` section 1.3: at the turnaround slot only three checks remain (CRC result, address match, buffer state).
- **Code:** `rx_done()` makes all three decisions when the last CRC bit is received. The `TURN` state only counts the clock edges of the turnaround slot and then enters `ARB` or `TX`.
- **Effect:** none on the wire. The slave reaches `IGNORE` directly (without `TURN`) after a CRC error, an empty buffer, or a `SEND_COMMAND`. The spec wording can say the checks are made as soon as the frame ends.
