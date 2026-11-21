# sigrok Decoder

A protocol decoder for [sigrok](https://sigrok.org): PulseView and `sigrok-cli` show the traffic of a logic analyzer on `CLK` and `DATA` as conditions, bits, slots, frame fields, and frames, and check the CRC of every frame. It is a tool for bringing up and debugging hardware; nothing on the bus depends on it. The decoder is in `tools/sigrok/hubus/`, in Python, as `libsigrokdecode` requires.

## Installation

Copy or link the directory `tools/sigrok/hubus` into the decoder directory of the user, and restart PulseView:

```sh
mkdir -p ~/.local/share/libsigrokdecode/decoders
ln -s "$PWD/tools/sigrok/hubus" ~/.local/share/libsigrokdecode/decoders/hubus
```

Alternatively, `SIGROKDECODE_DIR=tools/sigrok` makes `sigrok-cli` and PulseView look there.

## Use

- **PulseView:** add the decoder HUBUS and assign the channels `CLK` and `DATA`. A sample rate of at least ten times the clock rate is enough; with the default timing (50 µs per half period, [master library](master-library.md)) 1 MHz is plenty.
- **sigrok-cli**, for example on a file recorded with the analyzer:

  ```sh
  SIGROKDECODE_DIR=tools/sigrok sigrok-cli -i capture.sr -P hubus:clk=D0:data=D1 -A hubus=conditions:frames:warnings
  ```

- **Without hardware:** the bus simulation writes VCD files (`hubus_sim_vcd_open()` in `test/sim/hubus_sim.h`), which PulseView opens with *Import Value Change Dump*. `ctest --preset host -R sigrok` leaves the scenarios of the decoder test in `build/host/test/sigrok/` (`<scenario>.vcd`, and the decoded `<scenario>.out`); the channels are called `clk` and `data` there.

## Annotation Rows

| Row | Shows |
|---|---|
| Conditions | START, STOP, RESET ([§2](spec/02-line-conditions.md)), and the idle event handshake of a slave with the number of confirmation pulses ([§6.1](spec/06-operation.md#61-idle-event-handshake)) |
| Bits | every bit as a logical value (`1` = `DATA` low), from the rising `CLK` edge to the falling one; clock pulses outside a transaction as *Pulse* |
| Slots | the turnaround slot, and the arbitration and handoff slots of a group request with the arbitrated MCID bit; *lost here* where a contender lost ([§5.3](spec/05-arbitration.md#53-slot-sequence)) |
| Fields | `TYPE`, `TARGET_LEN` or `MORE` and `GEN`, target or source MCID, `LEN`, the payload bytes or the opcode of a command, the CRC8 ([§4.1](spec/04-frames.md#41-frame-structure)) |
| Frames | one line per frame, for example `REQUEST_DATA to group 10100x`, `SLAVE_DATA (event) from 42, GEN 1: CA`, `SLAVE_INFO from 3: protocol 0, speed class 0, HWID ...`; *No response* if the first bit after the slots stays high ([§3.4](spec/03-transactions.md#34-response-detection)) |
| Warnings | a wrong CRC, a frame cut off by a condition, a source MCID other than the addressed one or the winner, a slave type that does not fit the request, a request with `LEN` other than 0, clock pulses after the end of a frame, an event signal still held at START |

The decoder follows a transaction as the master does: it takes the length from `LEN`, the slots from `TARGET_LEN`, and expects a response after every request but `SEND_COMMAND`. A capture that starts in the middle of a transaction is decoded from the next START on; before it, bits show as clock pulses, and `DATA` low may show as an event signal.

## Not Yet Included

- The burst as a whole: the pending masks the handoff slots push, and whether every slave pending at the start was served ([§6.2](spec/06-operation.md#62-burst)).
- A check of the timing against the minimums `T_CLK_LOW`, `T_CLK_HIGH`, `T_COND`, and `T_TURNAROUND` ([§1.4](spec/01-physical-layer.md#14-timing-parameters)); PulseView measures the times on the raw signals.
- Output for stacked decoders.

## Test

`sigrok_decoder` ([development](development.md#tests)) records bus scenarios with the real master and slave cores (`test/tools/sigrok_scenarios.c`): RESET, `REQUEST_INFO`, `REQUEST_STATE`, no response, commands, the burst of [§5.6](spec/05-arbitration.md#56-example-four-pending-slaves) with its idle event handshake, a fragment with `MORE`, and a bit error on the line. The decoded output has to contain the lines of `test/sigrok/<scenario>.txt` in their order, and no other warnings.
