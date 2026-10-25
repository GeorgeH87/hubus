# Slave Library

Interrupt-driven HUBUS slave for tinyAVR microcontrollers, in `slave/`. The protocol logic lives in the hardware-independent core (`core/hubus_slave_core.c`, shared with the master and the host tests) and follows [§7](spec/07-slave.md); `slave/` adds the public API and one small hardware abstraction layer (HAL) per device family.

## Usage

```c
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include "hubus_slave.h"

static const uint8_t info[] PROGMEM = {0x01 /* device type */, 0x01 /* firmware */, 0x00, 0x00, 0x00, 0x2A /* serial */};

static void on_status_capture(void *ctx)
{
    uint8_t state = read_state();
    hubus_slave_write(&state, 1, 0);           /* the status becomes the event buffer content */
}

static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    if (opcode == 0x10 && len >= 1) set_output(args[0]);
}

int main(void)
{
    static const hubus_slave_config_t cfg = {
        .mcid = 3, .info = info, .info_len = sizeof info,
        .on_status_capture = on_status_capture, .on_command = on_command,
    };
    hubus_slave_init(&cfg);                    /* pin change interrupts for CLK and DATA */
    sei();                                     /* global interrupts: when the application is ready */
    for (;;) {
        /* application; hubus_slave_write() announces new data to the master */
    }
}
```

## API

| Function | Purpose |
|---|---|
| `hubus_slave_init(cfg)` | Configures the pins, starts the protocol engine, and enables the pin change interrupts. Global interrupts are left to the application: it calls `sei()` after its own setup. Returns 0, and the slave stays silent, for an MCID outside `1..63`, more than 28 info bytes, or a state longer than 31 bytes. |
| `hubus_slave_write(data, len, more)` | Writes the event buffer: overwrites unread content, increments `GEN`, sets the pending flag, and announces the data ([§4.8](spec/04-frames.md#48-event-buffer), [§6.1](spec/06-operation.md#61-idle-event-handshake)). `more = 1` marks a fragment followed by more ([§4.11](spec/04-frames.md#411-fragments)). Returns `HUBUS_WRITE_OK`, `HUBUS_WRITE_TOO_LONG`, `HUBUS_WRITE_BUSY` (the buffer is being transmitted, [§3.6](spec/03-transactions.md#36-transaction-end-and-buffer-clearing); retry later), or `HUBUS_WRITE_DISABLED` (init failed). Safe from the main loop and from callbacks. |
| `hubus_slave_is_pending()` | 1 while the buffer has not been read. Write the next fragment when it is 0. |
| `hubus_slave_isr()` | The pin change handler. The HAL vectors call it; the application only calls it itself in host tests, where there are no interrupts. |

The functions below the library are the hardware-independent core (`core/hubus_slave_core.h`), used by the library, the host tests, and the bus simulation: `hubus_core_init(core, cfg, clk, data)`, `hubus_core_pins(core, clk, data)` (feed the pin levels after every change), `hubus_core_data_low(core)` (the `DATA` drive to apply: 1 pulls low, 0 releases), `hubus_core_write()`, and `hubus_core_is_pending()`. Firmware does not call them; `hubus_core_config_t` is the same type as `hubus_slave_config_t`.

Configuration (`hubus_slave_config_t`):

| Field | Meaning |
|---|---|
| `mcid` | `1..63`, unique on the bus ([§7.2](spec/07-slave.md#72-mcid)) |
| `info`, `info_len` | Application bytes of `SLAVE_INFO`, in flash (`PROGMEM`), 0..28 bytes; include a device-unique value ([§4.7](spec/04-frames.md#47-slave-info-slave_info)) |
| `state`, `state_len` | Live state for `REQUEST_STATE`: RAM the application keeps up to date, read while transmitting, 1..31 bytes; `NULL` for no state ([§4.10](spec/04-frames.md#410-state)) |
| `on_status_capture` | Writes the current status with `hubus_slave_write()`; `NULL`: `STATUS_CAPTURE` not supported (capability bit 0 cleared) |
| `on_command` | Application commands, opcodes `0x10..0xFF`; may be `NULL` |
| `ctx` | Passed to the callbacks |

- `hubus_slave_write()` and `hubus_slave_is_pending()` exist only with the event buffer; `on_status_capture` is ignored without it.
- Callbacks run in the interrupt handler and must be short. Group commands are executed without any response; results go through the event buffer.
- Writes from the main loop disable interrupts for the duration of the copy (at most 31 bytes).

## Optional Parts

The event buffer and the state ([§7.4](spec/07-slave.md#74-optional-parts)) are independent build options. Both are on by default, and every combination is valid. Flash and RAM of the reference firmware on the ATtiny85:

| `HUBUS_ENABLE_EVENTS` | `HUBUS_ENABLE_STATE` | Device | Flash / RAM (bytes) |
|---|---|---|---:|
| 1 | 1 | push button with readable inputs | 1930 / 97 |
| 1 | 0 | event source only | 1830 / 94 |
| 0 | 1 | sensor that is only read, relay with readback | 1434 / 58 |
| 0 | 0 | relay without feedback (the master caches its state) | 1354 / 55 |

The event buffer costs about 500 bytes of flash and 39 bytes of RAM (including the capture and write code of the reference firmware), the state about 100 bytes of flash and 3 bytes of RAM.

## Examples

`examples/` has complete firmware for the ATtiny85, one for each combination of the optional parts. The CMake build compiles each example with its own options, independent of the options of the build directory.

| Example | Device | Events | State |
|---|---|---|---|
| `slave-switch` | push button: announces changes, input level readable | 1 | 1 |
| `slave-relay` | relay: state readable, `STATUS_CAPTURE` | 1 | 1 |
| `slave-sensor` | temperature sensor: value collected with `STATUS_CAPTURE` | 1 | 0 |
| `slave-sensor-passive` | temperature sensor that is only read (the master polls) | 0 | 1 |
| `slave-relay-simple` | relay without feedback (the master caches its state) | 0 | 0 |

The device types in their `SLAVE_INFO` (1 relay, 2 switch, 3 temperature) match the built-in profiles of [hubusd](daemon.md#device-profiles). The build also produces `example_switch_update`, the switch with [firmware update](#firmware-update).

## Configuration

Build options, set on the compiler command line; the CMake build takes them as cache variables (`-DHUBUS_CLK_PORT=...`):

| Macro | Meaning |
|---|---|
| `HUBUS_CLK_PORT`, `HUBUS_CLK_PIN` | Port letter and bit of `CLK` (CMake default per device, for example `B`, `2` on the ATtiny25/45/85) |
| `HUBUS_DATA_PORT`, `HUBUS_DATA_PIN` | Port letter and bit of `DATA` (for example `B`, `3`) |
| `HUBUS_ENABLE_EVENTS` | Event buffer: `1` (default) or `0` |
| `HUBUS_ENABLE_STATE` | State for `REQUEST_STATE`: `1` (default) or `0` |
| `HUBUS_ENABLE_UPDATE` | Switch to the updater on `UPDATE_ENTER`: `0` (default) or `1`; needs `HUBUS_UPDATER_ADDR` and an installed updater ([firmware update](#firmware-update)) |
| `HUBUS_UPDATER_ADDR` | Byte address of the updater, `0x1800` on the ATtiny85 (CMake cache variable of the same name) |
| `HUBUS_TX_BUF_SIZE` | Event buffer size, `1..31`, default 31 (20 on 128 B RAM devices with the event buffer) |
| `HUBUS_RX_BUF_SIZE` | Largest accepted command payload including the opcode, `1..31`, default 31 (20 on 128 B RAM devices with the event buffer); announced in `SLAVE_INFO` byte 2. Longer commands are received to the end and ignored ([§4.4](spec/04-frames.md#44-command-opcodes)). |
| `HUBUS_CLK_PCGROUP`, `HUBUS_DATA_PCGROUP` | Pin change group of a pin on classic devices the HAL does not know (see below) |

`DATA` needs an external pull-up to 3.3 V ([§1.1](spec/01-physical-layer.md#11-bus-lines)); the library never enables internal pull-ups.

## Firmware Update

A slave can receive new firmware over the bus if it carries the updater ([§6.8](spec/06-operation.md#68-firmware-update)). This is an optional part: the updater takes 2 KB of flash, so it suits devices with 8 KB or more. It exists for classic tinyAVR devices, built and tested for the ATtiny85; the tinyAVR 0/1/2-series has no updater yet, and `HUBUS_ENABLE_UPDATE` stops the build there.

| ATtiny85 flash | Content |
|---|---|
| `0x0000..0x17BF` | application, at most 6080 bytes; word 0 jumps to the updater |
| `0x17C0..0x17FF` | info page: length, CRC-16, and entry of the application |
| `0x1800..0x1FFF` | updater (`updater/`, 2006 bytes) |

**Application.** Build it with `HUBUS_ENABLE_UPDATE=1` and `HUBUS_UPDATER_ADDR=0x1800`, and keep it below `0x17C0`; nothing else changes in the firmware. The library then announces capability bit 3 and jumps to the updater on `UPDATE_ENTER`, with the MCID handed over in `GPIOR1`. This costs about 60 bytes of flash. Without an updater at that address the command is ignored. `example_switch_update` is the switch built this way, with a check that it fits.

**Updater.** `build/avr-attiny85/updater/hubus_updater.hex`, linked to `0x1800`. It runs without interrupts, starts the application at reset if the info page checks out (a CRC-16 over the application, about 30 ms at 8 MHz), and otherwise waits for the master. Its MCID comes from the application and is kept in the last EEPROM byte; before an application has stored one, the build option `HUBUS_UPDATER_MCID` (default 63) applies. The application must not use that EEPROM byte.

**Installing.** The first installation needs an in-system programmer, once: the updater, an application, and the fuses. `SELFPRGEN` (extended fuse bit 0) allows the updater to write flash; brown-out detection protects the write against a sinking supply ([§8.10](spec/08-faults.md#810-power-loss-during-a-firmware-update)); `WDTON` must stay unprogrammed. With the internal 8 MHz oscillator and BOD at 2.7 V:

```sh
avrdude -c usbasp -p t85 -U lfuse:w:0xE2:m -U hfuse:w:0xDD:m -U efuse:w:0xFE:m
avrdude -c usbasp -p t85 -U flash:w:build/avr-attiny85/updater/hubus_updater.hex
```

The updater image also holds the first word of flash, the jump to the updater, so the freshly programmed device starts the updater. It waits for the master, which loads the application with `hubus_master_update()`, for example with `hubus_master_example update 5 example_switch_update.hex` ([master library](master-library.md#firmware-update)). Writing the application with the programmer as well does not work: its first word would not jump to the updater, and the info page would be missing.

## Supported Devices

The HAL is selected from the compiler's device macro (`-mmcu=...`):

| Family | Devices | HAL |
|---|---|---|
| Classic tinyAVR | ATtiny25/45/85, 24/44/84, 261/461/861, 48/88, 441/841, 2313/2313A/4313, 1634 | `hal/hubus_hal_classic.h`: `DDRx`/`PORTx`/`PINx`, pin change interrupts |
| tinyAVR 0/1/2-series | ATtiny202/402/412, 804/814/1614/1616, 3216/3217, 424/824/1624 | `hal/hubus_hal_modern.h`: `PORTx`/`VPORTx`, per-pin interrupt sense, `PORTx_PORT_vect` |
| Host (tests) | – | `hal/hubus_hal_host.h`: pins are variables |

- **Reference target:** the ATtiny85 is always fully supported, with 31-byte payloads in both directions. Every library change must keep its reference build within the budget of 2050 bytes of flash and 112 bytes of RAM; the CMake build fails otherwise (`HUBUS_BUDGET_FLASH`, `HUBUS_BUDGET_RAM` in `slave/CMakeLists.txt`).
- **Minimum device:** 2 KB flash and 128 B RAM. Smaller devices (for example ATtiny13A) and the reduced-core ATtiny4/5/9/10 are not supported. On 2 KB devices the library with both optional parts leaves about 120 bytes of flash for the application, without the event buffer about 600.
- **128 B RAM devices** (ATtiny24/25, 261, 2313/2313A, 202/204/212/214) default to 20-byte buffers when they have the event buffer: with two 31-byte buffers the library takes 94 bytes of RAM, which leaves too little for the stack (the interrupt path alone needs about 40–50 bytes). Without the event buffer only the command buffer remains, and 31 bytes fit.
- **Pins:** `CLK` and `DATA` can be any pins with pin change or edge interrupts. For every classic device listed above the HAL derives the pin change group (register, enable bit, vector) from the port; for other classic devices `HUBUS_CLK_PCGROUP` and `HUBUS_DATA_PCGROUP` name it. On classic devices, both pins on the same port share one interrupt vector.
- Build-tested with `gcc-avr` 14.x for every device listed (`tools/build-avr-matrix.sh`). Only classic devices can be simulated with `simavr`; none of the HALs has run on real hardware yet ([open issues](project/open-issues.md)).

## Behavior Notes

- The slave has no timers and no timeouts ([§7.6](spec/07-slave.md#76-no-timeouts)).
- A buffer write is refused while the `SLAVE_DATA` response is being transmitted; a write between the end of the response and the STOP is kept ([§3.6](spec/03-transactions.md#36-transaction-end-and-buffer-clearing)).
- Event buffer, `GEN`, and pending flag survive a RESET ([§6.5](spec/06-operation.md#65-communication-stack-reset)).
